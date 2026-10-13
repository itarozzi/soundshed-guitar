#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace guitarfx
{
/**
 * The noise gate's Learn: listens to the gate's own detector for a couple of seconds while the
 * strings are muted, and works out where Threshold belongs, just above the noise it heard.
 *
 * It measures the detector, not the input, because the detector is what the thresholds are
 * compared against: band-passed, peak-held, and on the same scale as Threshold. The noise it
 * reports is therefore the level the gate itself sees, so a threshold set from it is right by
 * construction, wherever the gate sits in the chain.
 *
 * The measurement is the peak of each 10 ms slice, and the noise floor is the 95th percentile of
 * those peaks rather than the loudest: a cable knock or a pick brushing a string, held by the
 * detector for a few slices, must not drag the threshold up with it. Anything under about 90 ms
 * is left out that way. Anything longer lifts that percentile, and steady noise keeps it within
 * a dB or two of the median, so when it stands far above the median something was played, and
 * the result says so instead of setting a threshold above the playing.
 *
 * The gate never sets its own Threshold from this. The result goes back to the UI, which
 * applies it the way a turned knob would, so it lands in the preset, undo and the host.
 *
 * Threads: Start() and Cancel() are the message thread's, Snapshot() anyone's, Feed() and
 * EndBlock() the audio thread's. Nothing on the audio side locks or allocates.
 */
class GateNoiseLearner
{
  public:
    enum class State : std::uint8_t
    {
        Idle = 0,
        Listening = 1,
        Done = 2,
    };

    struct Snapshot
    {
        int session = 0;
        State state = State::Idle;
        /// 0..1 through the listen.
        float progress = 0.0f;
        /// The detector's level now, while listening (dBFS).
        float levelDb = kSilenceDb;
        /// Done: the noise the gate heard (dBFS, the 95th percentile of the slice peaks).
        float noiseFloorDb = kSilenceDb;
        /// Done: how far the noise floor stood above the median slice (dB).
        float spreadDb = 0.0f;
    };

    /// How long it listens, in 10 ms slices, after a short settle.
    static constexpr int kSlices = 200;
    static constexpr double kSliceMs = 10.0;
    static constexpr double kSettleMs = 100.0;
    /// Detector levels are floored here for the dB maths; digital silence reads as this.
    static constexpr float kSilenceDb = -140.0f;
    /// Steady noise keeps its 95th-percentile slice within a dB or two of the median. Further
    /// apart than this and something was played, or the strings rang, while it listened.
    static constexpr float kPlayingSpreadDb = 15.0f;
    /// Above this the "noise" is a held note or a feedback howl, not a noise floor.
    static constexpr float kLoudestNoiseFloorDb = -12.0f;
    /// Between the noise and the level that lets the gate close, so noise alone cannot hold it open.
    static constexpr double kMarginDb = 3.0;

    /// Audio thread is not running (or not yet): sizes the slices for the rate.
    void Prepare(double sampleRate)
    {
        mSliceSamples = std::max(1, static_cast<int>(std::lround(kSliceMs * 0.001 * sampleRate)));
        mSettleSamples = static_cast<int>(std::lround(kSettleMs * 0.001 * sampleRate));
    }

    /// Message thread: listen afresh under `session` (any non-zero number), replacing any listen
    /// in progress. The audio thread starts it on its next block.
    void Start(int session)
    {
        mRequested.store(session, std::memory_order_release);
    }

    /// Message thread: stop listening. A finished result stays readable.
    void Cancel()
    {
        mRequested.store(0, std::memory_order_release);
    }

    [[nodiscard]] Snapshot Read() const
    {
        Snapshot snapshot;
        const int packed = mPublished.load(std::memory_order_acquire);
        snapshot.session = packed >> 2;
        snapshot.state = static_cast<State>(packed & 3);
        snapshot.progress = mProgress.load(std::memory_order_relaxed);
        snapshot.levelDb = mLevelDb.load(std::memory_order_relaxed);
        snapshot.noiseFloorDb = mNoiseFloorDb.load(std::memory_order_relaxed);
        snapshot.spreadDb = mSpreadDb.load(std::memory_order_relaxed);
        return snapshot;
    }

    /// Audio thread, once at the top of each block: picks up a Start() or Cancel(). True while
    /// listening, so the gate only pays for Feed() on blocks that need it.
    [[nodiscard]] bool BeginBlock()
    {
        const int requested = mRequested.load(std::memory_order_acquire);

        if (requested != mSession)
        {
            mSession = requested;

            if (requested == 0)
            {
                mListening = false;
                Publish(State::Idle);
            }
            else
            {
                mListening = true;
                mSettleRemaining = mSettleSamples;
                mSliceRemaining = mSliceSamples;
                mSlicePeak = 0.0f;
                mSliceCount = 0;
                mBlockPeak = 0.0f;
                mProgress.store(0.0f, std::memory_order_relaxed);
                Publish(State::Listening);
            }
        }

        return mListening;
    }

    /// Audio thread, per sample while BeginBlock() said so: the detector's value.
    void Feed(float detector)
    {
        mBlockPeak = std::max(mBlockPeak, detector);

        if (mSettleRemaining > 0)
        {
            --mSettleRemaining;
            return;
        }

        mSlicePeak = std::max(mSlicePeak, detector);

        if (--mSliceRemaining > 0)
        {
            return;
        }

        mSlices[static_cast<std::size_t>(mSliceCount++)] = mSlicePeak;
        mSlicePeak = 0.0f;
        mSliceRemaining = mSliceSamples;

        if (mSliceCount == kSlices)
        {
            Finish();
        }
    }

    /// Audio thread, after a listening block: publishes the progress and live level.
    void EndBlock()
    {
        mLevelDb.store(ToDb(mBlockPeak), std::memory_order_relaxed);
        mBlockPeak = 0.0f;

        if (mListening)
        {
            mProgress.store(static_cast<float>(mSliceCount) / static_cast<float>(kSlices), std::memory_order_relaxed);
        }
    }

    /// Where Threshold goes for a noise floor: far enough above it that, with the gate's
    /// hysteresis taken off, the close level still sits a margin over the noise.
    [[nodiscard]] static double ThresholdFor(double noiseFloorDb, double hysteresisDb, double minDb, double maxDb)
    {
        return std::clamp(noiseFloorDb + std::max(0.0, hysteresisDb) + kMarginDb, minDb, maxDb);
    }

    /// Whether a finished listen heard noise rather than playing.
    [[nodiscard]] static bool HeardOnlyNoise(const Snapshot& snapshot)
    {
        return snapshot.spreadDb <= kPlayingSpreadDb && snapshot.noiseFloorDb <= kLoudestNoiseFloorDb;
    }

  private:
    [[nodiscard]] static float ToDb(float level)
    {
        return level > 1.0e-7f ? 20.0f * std::log10(level) : kSilenceDb;
    }

    void Finish()
    {
        // Sorted in a scratch copy: nth_element works in place and allocates nothing.
        mSorted = mSlices;
        const auto at = [this](int index) {
            std::nth_element(mSorted.begin(), mSorted.begin() + index, mSorted.end());
            return mSorted[static_cast<std::size_t>(index)];
        };
        const float median = at(kSlices / 2);
        const float floor = at(kSlices * 95 / 100);

        mNoiseFloorDb.store(ToDb(floor), std::memory_order_relaxed);
        mSpreadDb.store(std::max(0.0f, ToDb(floor) - ToDb(median)), std::memory_order_relaxed);
        mProgress.store(1.0f, std::memory_order_relaxed);
        mListening = false;
        Publish(State::Done);
    }

    void Publish(State state)
    {
        mPublished.store((mSession << 2) | static_cast<int>(state), std::memory_order_release);
    }

    // Message thread → audio thread.
    std::atomic<int> mRequested{0};
    // Audio thread → anyone. Session and state share one word so a reader never pairs one
    // listen's state with another's session.
    std::atomic<int> mPublished{0};
    std::atomic<float> mProgress{0.0f};
    std::atomic<float> mLevelDb{kSilenceDb};
    std::atomic<float> mNoiseFloorDb{kSilenceDb};
    std::atomic<float> mSpreadDb{0.0f};

    // Audio thread only.
    int mSession = 0;
    bool mListening = false;
    int mSliceSamples = 480;
    int mSettleSamples = 4800;
    int mSettleRemaining = 0;
    int mSliceRemaining = 0;
    int mSliceCount = 0;
    float mSlicePeak = 0.0f;
    float mBlockPeak = 0.0f;
    std::array<float, kSlices> mSlices{};
    std::array<float, kSlices> mSorted{};
};
} // namespace guitarfx
