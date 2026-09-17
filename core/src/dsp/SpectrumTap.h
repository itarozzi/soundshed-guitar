#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace guitarfx
{
class SimdFFT;

/**
 * The spectrum of one point in a signal graph, drawn behind an EQ curve so the user can
 * see which frequencies the source is actually using.
 *
 * The audio thread only copies samples in (Push). The FFT, the log-frequency binning and
 * the smoothing all run on the message thread (Analyze), at the rate the UI is fed, so a
 * tap costs the audio thread one short copy per block and nothing at all when detached.
 *
 * It keeps a window of its own rather than reusing InputAnalyzerEffect's spectrogram. That
 * one runs a Goertzel filter per bin over each audio block, so its resolution is the
 * host's block size: at 64 samples and 48 kHz everything below ~750 Hz lands in one smear,
 * which is exactly where guitar EQ happens. Here the window is sized from the sample rate
 * (~6 Hz bins) whatever the block size is.
 *
 * Threading: one writer at a time, whichever thread processes the tapped node that block,
 * and one reader, the message thread. The ring is twice the largest window, so the samples
 * the reader copies can only be overwritten if the writer gets through half a ring during a
 * copy that takes microseconds.
 */
class SpectrumTap
{
  public:
    static constexpr int kBins = 128;
    static constexpr double kMinFrequencyHz = 20.0;
    static constexpr double kMaxFrequencyHz = 20000.0;
    static constexpr double kFloorDb = -96.0;
    static constexpr double kCeilingDb = 0.0;
    /// Each FFT bin of pink noise falls 3 dB an octave, so this tilt draws it flat -- the
    /// usual reference for a music analyzer, and it keeps a guitar's top end readable.
    static constexpr double kTiltDbPerOctave = 3.0;
    static constexpr double kTiltPivotHz = 1000.0;

    /// dB per bin, log-spaced from kMinFrequencyHz to kMaxFrequencyHz inclusive, tilted and
    /// clamped to [kFloorDb, kCeilingDb].
    using Bins = std::array<float, kBins>;

    SpectrumTap();
    ~SpectrumTap();

    SpectrumTap(const SpectrumTap&) = delete;
    SpectrumTap& operator=(const SpectrumTap&) = delete;

    /// Audio thread. `right` is null for a mono signal.
    void Push(const float* left, const float* right, int numSamples) noexcept;

    /// Message thread. Forgets everything pushed so far, smoothing included -- for when the
    /// tap moves to another node and the old one's spectrum must not bleed into the new.
    void Restart();

    /// Message thread. The smoothed spectrum as of `now`.
    void Analyze(double sampleRate, Bins& out,
                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /// Window length at a sample rate: the power of two giving bins of about 6 Hz.
    [[nodiscard]] static int FftSizeFor(double sampleRate);

    /// Centre frequency of display bin `bin`.
    [[nodiscard]] static double BinFrequencyHz(int bin);

  private:
    static constexpr std::size_t kRingSize = 32768;
    static constexpr std::size_t kRingMask = kRingSize - 1;

    /// How long a tap can go without new samples before it reads as silence. Longer than
    /// any host block, so a large buffer does not flicker between frames.
    static constexpr double kSilentAfterSeconds = 0.3;
    static constexpr double kAttackSeconds = 0.02;
    static constexpr double kReleaseSeconds = 0.3;

    struct BinSource
    {
        // Inclusive FFT bin range. Empty (first > last) at low frequencies, where one FFT
        // bin is wider than a display bin; those interpolate at `centre` instead.
        int first = 0;
        int last = -1;
        double centre = 0.0;
        float tiltDb = 0.0f;
        bool aboveNyquist = false;
    };

    void Configure(double sampleRate);
    void ComputeTarget();

    std::unique_ptr<float[]> mRing;
    std::atomic<std::uint64_t> mWritten{0};

    // Everything below is the message thread's.
    std::uint64_t mStartedAt = 0;
    std::uint64_t mLastAnalyzedAt = 0;
    std::chrono::steady_clock::time_point mLastNewSamplesAt{};
    std::chrono::steady_clock::time_point mLastAnalyzeAt{};
    bool mHasSmoothed = false;

    double mSampleRate = 0.0;
    int mFftSize = 0;
    std::unique_ptr<SimdFFT> mFft;
    std::vector<float> mWindow;
    std::vector<std::complex<float>> mFftIn;
    std::vector<std::complex<float>> mFftOut;
    std::vector<float> mPower;
    std::array<BinSource, kBins> mBinSources{};

    Bins mTarget{};
    Bins mSmoothed{};
};
} // namespace guitarfx
