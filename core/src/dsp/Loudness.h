#pragma once

/**
 * Loudness.h - ITU-R BS.1770-4 loudness: the K-weighting filter, the gated integration, and an
 * offline meter for a rendered buffer.
 *
 * Three users share it. The Signal Analyzer (InputAnalyzerEffect) runs the K-weighting and the
 * gating live on the audio thread for its momentary, short-term and integrated readouts, so the
 * filter and GatedLoudness() never allocate. measurePresetLevel (controller/PresetLevelMeter)
 * renders a preset over a demo clip and measures the result with IntegratedLoudnessMeter. The
 * per-node DSP status readout measures one node's output on the message thread with
 * StreamingLoudnessMeter, fed by a LoudnessTap.
 *
 * The K-weighting is designed from the standard's analogue prototype for any sample rate (the
 * pre-filter shelf and the RLB high-pass), which at 48 kHz reproduces the coefficient table
 * BS.1770 publishes. Blocks are 400 ms, starting every 100 ms from the first sample; the
 * absolute gate is -70 LUFS and the relative gate 10 LU below the mean of the blocks that pass
 * it. Left and right are weighted 1.0 each. That is the content pipeline's own reference
 * (soundshed-guitar-private content/preset-packs/scripts/lib/loudness.mjs), and LoudnessTests
 * holds this to it: a 997 Hz sine at -20 dBFS on both channels reads -20.0 LUFS, and -23.0 on one.
 */

#include "dsp/BiquadDesign.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace guitarfx::loudness
{
inline constexpr double kBlockSeconds = 0.4;
inline constexpr double kHopSeconds = 0.1;
inline constexpr double kAbsoluteGateLufs = -70.0;
inline constexpr double kRelativeGateLu = -10.0;
/// The -0.691 in L = -0.691 + 10 log10(sum of channel mean squares).
inline constexpr double kLoudnessOffsetDb = -0.691;

/// Stage 1 of the K-weighting: the head's high shelf, about +4 dB above 2 kHz.
[[nodiscard]] inline BiquadCoefficients DesignKWeightingHighShelf(double sampleRate) noexcept
{
    constexpr double kGainDb = 3.99984385397;
    constexpr double kQ = 0.7071752369554193;
    constexpr double kFc = 1681.9744509555319;
    const double K = std::tan(3.14159265358979323846 * kFc / sampleRate);
    const double Vh = std::pow(10.0, kGainDb / 20.0);
    const double Vb = std::pow(Vh, 0.499666774155);
    const double a0 = 1.0 + K / kQ + K * K;

    BiquadCoefficients coeffs;
    coeffs.b0 = (Vh + Vb * K / kQ + K * K) / a0;
    coeffs.b1 = 2.0 * (K * K - Vh) / a0;
    coeffs.b2 = (Vh - Vb * K / kQ + K * K) / a0;
    coeffs.a1 = 2.0 * (K * K - 1.0) / a0;
    coeffs.a2 = (1.0 - K / kQ + K * K) / a0;
    return coeffs;
}

/// Stage 2 of the K-weighting: the RLB high-pass at about 38 Hz.
///
/// The numerator stays 1, -2, 1, as the standard's table has it, rather than being divided by
/// a0 like the denominator: that leaves the passband about +0.04 dB above unity, and the -0.691
/// in the loudness formula is calibrated against exactly that filter. The Signal Analyzer had
/// normalised it and read 0.043 LU low against BS.1770 and the content pipeline.
[[nodiscard]] inline BiquadCoefficients DesignKWeightingHighPass(double sampleRate) noexcept
{
    constexpr double kQ = 0.5003270373253953;
    constexpr double kFc = 38.13547087613982;
    const double K = std::tan(3.14159265358979323846 * kFc / sampleRate);
    const double a0 = 1.0 + K / kQ + K * K;

    BiquadCoefficients coeffs;
    coeffs.b0 = 1.0;
    coeffs.b1 = -2.0;
    coeffs.b2 = 1.0;
    coeffs.a1 = 2.0 * (K * K - 1.0) / a0;
    coeffs.a2 = (1.0 - K / kQ + K * K) / a0;
    return coeffs;
}

/// One channel's K-weighting, both stages, in double precision (transposed direct form II).
/// Allocation-free; Prepare() designs it for a rate, Reset() clears its state.
class KWeightingFilter
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mShelf = DesignKWeightingHighShelf(sampleRate);
        mHighPass = DesignKWeightingHighPass(sampleRate);
        Reset();
    }

    void Reset() noexcept
    {
        mShelfZ1 = mShelfZ2 = mHighPassZ1 = mHighPassZ2 = 0.0;
    }

    [[nodiscard]] double Process(double input) noexcept
    {
        const double shelved = Run(mShelf, mShelfZ1, mShelfZ2, input);
        return Run(mHighPass, mHighPassZ1, mHighPassZ2, shelved);
    }

  private:
    static double Run(const BiquadCoefficients& c, double& z1, double& z2, double input) noexcept
    {
        const double output = (c.b0 * input) + z1;
        z1 = (c.b1 * input) - (c.a1 * output) + z2;
        z2 = (c.b2 * input) - (c.a2 * output);
        return output;
    }

    BiquadCoefficients mShelf;
    BiquadCoefficients mHighPass;
    double mShelfZ1 = 0.0;
    double mShelfZ2 = 0.0;
    double mHighPassZ1 = 0.0;
    double mHighPassZ2 = 0.0;
};

/// Loudness of a block whose channel mean squares sum to `meanSquare`. Positive input only.
[[nodiscard]] inline double MeanSquareToLufs(double meanSquare) noexcept
{
    return kLoudnessOffsetDb + 10.0 * std::log10(meanSquare);
}

[[nodiscard]] inline double LufsToMeanSquare(double lufs) noexcept
{
    return std::pow(10.0, (lufs - kLoudnessOffsetDb) / 10.0);
}

/// Gated (integrated) loudness of a run of 400 ms blocks, each given as its summed channel mean
/// square: the blocks above the absolute gate, then of those the ones above the relative gate,
/// averaged. Empty when no block passes, which silence never does. Non-finite entries are
/// skipped. Allocation-free, so the analyzer can call it on the audio thread.
[[nodiscard]] std::optional<double> GatedLoudness(const double* blockMeanSquares, std::size_t count) noexcept;

/// A rendered buffer's levels.
struct LevelMeasurement
{
    /// BS.1770 integrated loudness. Empty for silence or anything too short or quiet to gate.
    std::optional<double> integratedLufs;
    /// Highest absolute sample, in dBFS (a sample peak, not true peak). Empty for silence.
    std::optional<double> peakDbfs;
    /// RMS over both channels and every frame, unweighted, in dBFS. Empty for silence.
    std::optional<double> rmsDbfs;
    std::size_t frames = 0;
    double sampleRate = 0.0;

    [[nodiscard]] double DurationSeconds() const noexcept
    {
        return sampleRate > 0.0 ? static_cast<double>(frames) / sampleRate : 0.0;
    }
};

/**
 * Measures a stream pushed through it in pieces, so a render never has to be held whole.
 * Pushing the same samples in any split gives the same answer. It keeps one running sum per
 * stretch of gcd(block, hop) frames (100 ms at the usual rates), so memory grows by a few
 * doubles a second.
 */
class IntegratedLoudnessMeter
{
  public:
    explicit IntegratedLoudnessMeter(double sampleRate);

    /// `right` may be null for a mono signal, which counts as the same signal on both channels
    /// (the pipeline's reference reads a one-channel file that way too).
    void Push(const float* left, const float* right, std::size_t frames);

    [[nodiscard]] LevelMeasurement Result() const;

  private:
    double mSampleRate;
    std::size_t mBlockFrames;
    std::size_t mHopFrames;
    std::size_t mGranuleFrames;
    KWeightingFilter mLeftFilter;
    KWeightingFilter mRightFilter;
    std::vector<double> mGranuleEnergy;
    double mPendingEnergy = 0.0;
    std::size_t mPendingFrames = 0;
    std::size_t mFrames = 0;
    double mSumSquares = 0.0;
    float mPeak = 0.0f;
};

/**
 * Momentary, short-term and integrated loudness of a live stream: the three readouts of an EBU R128
 * meter, refreshed every 100 ms. For the message thread, fed from a LoudnessTap; it allocates as the
 * integrated history grows.
 *
 * The stream is cut into 100 ms granules (the hop). Momentary is the last 4 of them (400 ms),
 * short-term the last 30 (3 s), and each run of 4 consecutive granules is one gating block, which
 * is the blocks GatedLoudness sees: 400 ms long, starting every 100 ms. A readout is empty until its
 * window has filled, so momentary needs 400 ms of signal and short-term 3 s; a window of silence
 * reads empty too. A block below the absolute gate is dropped as it closes, since the gate would
 * only discard it later: that keeps a long silence from growing the history, at one double per
 * 100 ms of signal otherwise.
 */
class StreamingLoudnessMeter
{
  public:
    static constexpr std::size_t kMomentaryGranules = 4;
    static constexpr std::size_t kShortTermGranules = 30;

    /// Designs the K-weighting for `sampleRate` and forgets everything measured.
    void Prepare(double sampleRate);
    void Reset();

    /// `right` may be null for a mono signal, which counts as the same signal on both channels.
    void Push(const float* left, const float* right, std::size_t frames);
    void PushSilence(std::size_t frames);

    [[nodiscard]] std::optional<double> MomentaryLufs() const noexcept
    {
        return WindowLufs(kMomentaryGranules);
    }

    [[nodiscard]] std::optional<double> ShortTermLufs() const noexcept
    {
        return WindowLufs(kShortTermGranules);
    }

    /// Gated over everything since Prepare() or Reset(). Recomputed as each granule closes.
    [[nodiscard]] std::optional<double> IntegratedLufs() const noexcept
    {
        return mIntegrated;
    }

    /// How much of the stream has been pushed, silence included.
    [[nodiscard]] double MeasuredSeconds() const noexcept
    {
        return static_cast<double>(mFrames) / mSampleRate;
    }

  private:
    void AddFrameEnergy(double energy);
    [[nodiscard]] std::optional<double> WindowLufs(std::size_t granules) const noexcept;

    double mSampleRate = 48000.0;
    std::size_t mGranuleFrames = 4800;
    KWeightingFilter mLeftFilter;
    KWeightingFilter mRightFilter;
    double mPendingEnergy = 0.0;
    std::size_t mPendingFrames = 0;
    /// The last kShortTermGranules granules' summed channel mean squares, as a ring.
    std::array<double, kShortTermGranules> mRecentGranules{};
    std::size_t mRecentWrite = 0;
    std::size_t mGranulesClosed = 0;
    /// Mean squares of the blocks above the absolute gate.
    std::vector<double> mGatedBlocks;
    std::optional<double> mIntegrated;
    std::size_t mFrames = 0;
};

/// IntegratedLoudnessMeter over a whole buffer. `right` may be null (see Push).
[[nodiscard]] LevelMeasurement MeasureIntegratedLoudness(const float* left, const float* right, std::size_t frames,
                                                         double sampleRate);

[[nodiscard]] inline LevelMeasurement MeasureIntegratedLoudness(const std::vector<float>& left,
                                                                const std::vector<float>& right, double sampleRate)
{
    const std::size_t frames = right.empty() ? left.size() : std::min(left.size(), right.size());
    return MeasureIntegratedLoudness(left.data(), right.empty() ? nullptr : right.data(), frames, sampleRate);
}
} // namespace guitarfx::loudness
