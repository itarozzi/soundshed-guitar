#pragma once

#include "dsp/effects/SignalsmithLatency.h"
#include "signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Shared live configuration for every Signalsmith Stretch node.
 *
 * `presetCheaper()` couples two knobs that are actually independent: total
 * latency is exactly `blockSamples`, while spectral quality follows the overlap
 * ratio `blockSamples / intervalSamples`. The preset picks a large block (which
 * is latency) *and* a large interval (ratio 2.5, which is quality) together, so
 * it pays for latency twice over.
 *
 * Measured at 48 kHz over six guitar-range notes x three analysis windows at
 * -12/-7/-5/-2/+7 st, with per-callback CPU at a 64-sample buffer:
 *
 *   config                           latency   mean|cents|  purity   mean    p99
 *   presetCheaper 4800/1920 (2.5x)    100 ms       3.0     -1.26 dB   9 us   306 us
 *   this policy   3840/960  (4x)       80 ms       3.0     -0.93 dB  14 us   248 us
 *   presetDefault 5760/1440 (4x)      120 ms       1.9     -0.32 dB  14 us   369 us
 *
 * So this is 20 ms faster than the preset it replaced, with better tone and a
 * smaller worst-case block. It stops at 80 ms because quality falls off a cliff
 * below ~60 ms (50 ms -> 9 cents and -3.5 dB, 40 ms -> 11 cents and -4.7 dB);
 * the live deep-drop target needs a different engine, not a smaller window here.
 * See docs/plans/transpose-improvements.md.
 *
 * Split computation trades that 20 ms back for a ~10x smaller CPU spike (p99
 * 32 us rather than 248 us) because latency becomes `block + interval` again.
 * We keep it off while latency is the scarcer resource; flip the constant if
 * small-buffer dropouts ever matter more.
 */
constexpr double kSignalsmithBlockSeconds = 0.08;
constexpr double kSignalsmithIntervalSeconds = 0.02;
constexpr bool kSignalsmithSplitComputation = false;

inline void ConfigureSignalsmithLive(signalsmith::stretch::SignalsmithStretch<float>& stretch, int channels,
                                     double sampleRate)
{
    const int block = std::max(64, static_cast<int>(std::lround(sampleRate * kSignalsmithBlockSeconds)));
    const int interval = std::max(16, static_cast<int>(std::lround(sampleRate * kSignalsmithIntervalSeconds)));
    stretch.configure(channels, block, interval, kSignalsmithSplitComputation);
}

/**
 * The latency-aligned dry signal for a Signalsmith node, and the input history a
 * clean re-engage needs. Both used to be open-coded per effect, and both were
 * wrong in the same way -- the ring was only written when it was about to be
 * read.
 *
 *  - The dry delay was only filled while `mix < 1`, so the first time a player
 *    turned Mix down they got `latency` samples of whatever was stale in it.
 *  - Nothing reset Stretch when a node left its transparent (0 st) bypass, and
 *    Stretch was not fed at all during the bypass, so on re-engage it replayed
 *    the audio from whenever the shift was last on.
 *
 * So Push() is unconditional -- during bypass too -- and Engage() hands Stretch
 * real history via reset() + seek(), the start-of-playback recipe from the
 * Signalsmith docs. Measured with a frequency sweep (so the output's pitch says
 * which moment of input it came from), source-time error in the first 10 ms
 * after re-engaging, relative to the steady-state alignment:
 *
 *   today (no reset, no seek)      -2010 ms, plus a mute window; correct by 80 ms
 *   reset() only                     +374 ms; correct by ~80 ms
 *   reset() + seek(inputLatency())    +21 ms; correct by ~50 ms
 *   reset() + seek(seekLength())       +9 ms; correct by ~30 ms
 *
 * seek() wants `seekLength()` = one block plus one interval, which is more than
 * `inputLatency()`, so the ring is sized for the larger of the two.
 */
class SignalsmithDryHistory
{
  public:
    /// Sized once, from a configuration that cannot change afterwards: Stretch's
    /// latency depends on block size, not on the shift. Nothing here allocates
    /// on the audio thread.
    void Prepare(int latencySamples, int seekLengthSamples, int maxBlockSize)
    {
        mSeekLength = std::max(seekLengthSamples, 0);

        const int history = std::max(std::max(latencySamples, 0), mSeekLength);
        const size_t needed = static_cast<size_t>(history + std::max(maxBlockSize, 1) + 8);

        mLeft.assign(needed, 0.0f);
        mRight.assign(needed, 0.0f);
        mSeekLeft.assign(static_cast<size_t>(std::max(mSeekLength, 1)), 0.0f);
        mSeekRight.assign(static_cast<size_t>(std::max(mSeekLength, 1)), 0.0f);
        mSeekPtrs[0] = mSeekLeft.data();
        mSeekPtrs[1] = mSeekRight.data();
        mWritePos = 0;
    }

    void Reset()
    {
        std::fill(mLeft.begin(), mLeft.end(), 0.0f);
        std::fill(mRight.begin(), mRight.end(), 0.0f);
        mWritePos = 0;
    }

    [[nodiscard]] bool IsPrepared() const
    {
        return !mLeft.empty();
    }

    /// Records one input sample. Called for every sample the node sees, whether
    /// or not the dry path or the shift is in use.
    void Push(float inL, float inR)
    {
        if (mLeft.empty())
        {
            return;
        }

        mLeft[mWritePos] = inL;
        mRight[mWritePos] = inR;
        mWritePos = (mWritePos + 1) % mLeft.size();
    }

    /// Records one input sample and reads the one from `delaySamples` ago, so a
    /// partial wet/dry mix blends against the wet path's own alignment.
    void PushAndRead(float inL, float inR, int delaySamples, float& outL, float& outR)
    {
        if (mLeft.empty() || delaySamples <= 0)
        {
            Push(inL, inR);
            outL = inL;
            outR = inR;
            return;
        }

        const size_t size = mLeft.size();
        mLeft[mWritePos] = inL;
        mRight[mWritePos] = inR;

        const size_t delay = static_cast<size_t>(std::min(delaySamples, static_cast<int>(size) - 1));
        const size_t readPos = (mWritePos + size - delay) % size;
        outL = mLeft[readPos];
        outR = mRight[readPos];

        mWritePos = (mWritePos + 1) % size;
    }

    /// The most recent `mSeekLength` samples, laid out contiguously for
    /// `Stretch::seek()`. Null when there is nothing to hand it.
    [[nodiscard]] float** SeekHistory()
    {
        if (mLeft.empty() || mSeekLength <= 0)
        {
            return nullptr;
        }

        const size_t size = mLeft.size();
        const size_t count = static_cast<size_t>(mSeekLength);
        size_t readPos = (mWritePos + size - count) % size;

        for (size_t i = 0; i < count; ++i)
        {
            mSeekLeft[i] = mLeft[readPos];
            mSeekRight[i] = mRight[readPos];
            readPos = (readPos + 1) % size;
        }

        return mSeekPtrs;
    }

    [[nodiscard]] int SeekLength() const
    {
        return mSeekLength;
    }

  private:
    std::vector<float> mLeft;
    std::vector<float> mRight;
    std::vector<float> mSeekLeft;
    std::vector<float> mSeekRight;
    float* mSeekPtrs[2] = {nullptr, nullptr};
    size_t mWritePos = 0;
    int mSeekLength = 0;
};

/// Re-engages a Stretch instance that has not been fed while a node sat in its
/// transparent bypass: clear the stale overlap-add state, then seek it onto the
/// real input history so the first block out is already the right audio.
inline void EngageSignalsmith(signalsmith::stretch::SignalsmithStretch<float>& stretch, SignalsmithDryHistory& history)
{
    stretch.reset();

    if (float** seekInput = history.SeekHistory())
    {
        stretch.seek(seekInput, history.SeekLength(), 1.0);
    }
}
} // namespace guitarfx
