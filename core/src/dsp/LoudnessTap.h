#pragma once

#include "dsp/Loudness.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace guitarfx
{
/**
 * The loudness of one point in a signal graph, for the per-node DSP status readout: momentary,
 * short-term and integrated, as BS.1770 and EBU R128 define them (loudness::StreamingLoudnessMeter).
 *
 * Built like SpectrumTap so that watching costs the audio thread next to nothing. The audio thread
 * only copies samples in (Push), and only for the one node holding the tap, which it holds only
 * while the readout is open. The K-weighting, the windows and the gating all run on the message
 * thread (Analyze). So a tap costs two short copies per block of the node it watches, outside that
 * node's timing, and a node without one pays a single pointer load.
 *
 * Unlike a spectrum, loudness needs every sample rather than the latest window, so the reader takes
 * the ring from where it last stopped. The ring holds kRingFrames a side (2.7 s at 48 kHz, 0.68 s
 * at 192 kHz), several times the gap between reads. A reader that falls more than half a ring
 * behind skips to the newest half, losing that stretch rather than reading samples the writer may
 * be overwriting.
 *
 * Samples stop arriving when the node stops running: it has no input, or the mixer has put its
 * graph to sleep on silence (SilenceSleepGate). Both mean silence, so once nothing has arrived for
 * longer than any host block, the reader feeds the meter silence for the time that passes, and the
 * momentary and short-term values fall away as a meter watching a silent signal would show.
 *
 * Threading: one writer at a time, whichever thread processes the watched node that block, and one
 * reader, the message thread.
 */
class LoudnessTap
{
  public:
    static constexpr std::size_t kRingFrames = std::size_t{1} << 17;

    struct Reading
    {
        std::optional<double> momentaryLufs;
        std::optional<double> shortTermLufs;
        std::optional<double> integratedLufs;
        /// How long the integrated value has been measuring, since the tap last restarted.
        double measuredSeconds = 0.0;
    };

    LoudnessTap();
    ~LoudnessTap();

    LoudnessTap(const LoudnessTap&) = delete;
    LoudnessTap& operator=(const LoudnessTap&) = delete;

    /// Audio thread. `right` is null for a mono signal, which counts as the same signal on both
    /// sides, as BS.1770 reads a mono file played on two speakers.
    void Push(const float* left, const float* right, int numSamples) noexcept;

    /// Message thread. Forgets everything measured so far, integrated value included: for when the
    /// tap moves to another node, or the readout starts measuring again.
    void Restart();

    /// Message thread. Measures what has been pushed since the last call and returns the readings
    /// as of `now`.
    Reading Analyze(double sampleRate, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  private:
    static constexpr std::size_t kRingMask = kRingFrames - 1;
    /// How far the reader may trail the writer before it skips ahead.
    static constexpr std::size_t kMaxBacklogFrames = kRingFrames / 2;
    /// How long a tap can go without new samples before it counts as silence. Longer than any host
    /// block, so a large buffer is never mistaken for a pause.
    static constexpr double kSilentAfterSeconds = 0.3;
    /// The most silence fed in one read. More than the 3 s short-term window: past that, silence
    /// changes no readout, and the integrated history drops it at the gate anyway.
    static constexpr double kMaxSilenceSeconds = 3.5;

    std::unique_ptr<float[]> mLeft;
    std::unique_ptr<float[]> mRight;
    std::atomic<std::uint64_t> mWritten{0};

    // Everything below is the message thread's.
    std::uint64_t mRead = 0;
    double mSampleRate = 0.0;
    bool mClockStarted = false;
    std::chrono::steady_clock::time_point mLastNewSamplesAt{};
    std::chrono::steady_clock::time_point mSilenceFedUntil{};
    loudness::StreamingLoudnessMeter mMeter;
};
} // namespace guitarfx
