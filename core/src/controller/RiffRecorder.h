#pragma once

// RiffRecorder — riff capture's recorder: the looper's recorder (LooperEngine), with playback off,
// recording one take at a time.
//
// It is fed the raw host input, ahead of the demo clip and of the input fold and calibration, so a
// take is what the guitar played: replayed later through the rig (DemoPreviewService), it is
// calibrated once, there. Its engine is a second instance, not the mixer's looper, so a riff take
// never disturbs a loop and a loop never lands in a riff.
//
// Threads. Process() runs on the audio thread under the DSP lock, and everything that changes the
// engine — Start, Close, Drop, Release — is called holding that same lock, which stands in for the
// engine's audio thread (LooperEngine.h). Poll() and LivePeak() read what the engine publishes and
// need no lock. Buffers are allocated before the lock is taken and handed in and out by move.

#include "dsp/TransportClock.h"
#include "dsp/looper/LooperEngine.h"

#include <cstdint>
#include <vector>

namespace guitarfx
{
class RiffRecorder
{
  public:
    /// The first note must reach about -60 dBFS (riff capture's threshold since it began). The
    /// engine raises it above a hum it measures while it waits.
    static constexpr float kArmThreshold = 0.001f;

    struct Status
    {
        bool started = false;  ///< Recording has begun (an armed take has heard its first note)
        bool finished = false; ///< The take has closed, by a stop or by filling up
        std::int64_t recorded = 0;
        /// Samples from the end of the count-in to the take's first sample, for an armed take.
        std::int64_t armLead = 0;
    };

    /// A finished take's audio, still in recording order: `head` is where it starts in `left` and
    /// `right`, which may wrap round. Rotate() puts it at the front.
    struct Audio
    {
        std::vector<float> left;
        std::vector<float> right;
        std::int64_t head = 0;
        std::int64_t length = 0;

        /// Off the lock: the take at the front of the buffers, exactly `length` long.
        void Rotate();
    };

    void Prepare(double sampleRate, int maxBlockSize);

    // ── Holding the DSP lock ─────────────────────────────────────────────────────────
    /// Counting in, armed or recording.
    [[nodiscard]] bool Busy() const noexcept;
    /// Holding a take, finished or not, that Release() or Drop() has not handed back.
    [[nodiscard]] bool HasTake() const noexcept
    {
        return !mLeft.empty();
    }
    /// Starts a take into `left` and `right`, each as long as the longest take allowed: counting in
    /// `countInBars` bars of `samplesPerBar`, then recording at once or, `armed`, on the first note.
    void Start(std::vector<float> left, std::vector<float> right, bool armed, double samplesPerBar,
               int countInBars);
    /// Ends the take where it is. False when it had not started recording, and it is then dropped.
    bool Close();
    /// Drops the take, finished or not.
    void Drop();
    /// A finished take's audio; the recorder holds nothing after.
    Audio Release();

    // ── Audio thread, under the DSP lock ─────────────────────────────────────────────
    /// One block of the raw input (`inputs[1]` may be null). True on the block the take stops
    /// recording, by filling up: the caller stops the guidance click there.
    bool Process(float** inputs, int numSamples) noexcept;

    // ── Message thread, no lock ──────────────────────────────────────────────────────
    [[nodiscard]] Status Poll() const;
    /// The DI peaks so far, per bucket of an equal share of the longest take.
    [[nodiscard]] float LivePeak(int bucket) const noexcept
    {
        return mEngine.LivePeak(true, bucket);
    }

  private:
    [[nodiscard]] static bool Recording(looper::LooperState state) noexcept;

    looper::LooperEngine mEngine;
    TransportClock mClock;
    std::vector<float> mLeft;
    std::vector<float> mRight;
    looper::LayerAudio mCapture;
    double mSampleRate = 0.0;
    bool mWasRecording = false;
};
} // namespace guitarfx
