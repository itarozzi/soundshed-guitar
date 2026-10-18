#pragma once

#include "NAM/dsp.h"
#include "dsp/FiniteCheck.h"

// ResamplingContainer pulls in WDL, which includes <windows.h> on Win32. That
// defines UpdateResource as UpdateResourceA, which then mangles unrelated member
// calls such as ResourceLibrary::UpdateResource in every translation unit that
// includes this header. NOMINMAX is already set for the core target; drop the
// remaining macros that collide with our own API names.
#include "dsp/ResamplingContainer/ResamplingContainer.h"

#ifdef UpdateResource
    #undef UpdateResource
#endif
#ifdef RemoveResource
    #undef RemoveResource
#endif

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <memory>
#include <vector>

namespace guitarfx
{
inline constexpr int kNamOversamplingMaxIndex = 5;
inline constexpr int kNamAntiAliasPhaseMaxIndex = 2;
inline constexpr int kNamOversamplingIndexDefault = 0;
inline constexpr int kNamAntiAliasPhaseIndexDefault = 0;
/// Half-width, in input samples, of the Lanczos kernel the resampler falls back on when the host
/// rate is not a power-of-two multiple of the model's (44.1 kHz against a 48 kHz model, say). 32
/// cost 121 ns per host sample there and 12 costs 82, against about 830 for a standard model
/// (docs/dsp-performance-review-2026-10-14.md), and the latency at 44.1 kHz falls from 66 samples to
/// 27. tools/audio-ab at 44.1 kHz found no band of the DI guitar or the sweep more than 0.05 dB
/// apart through a NAM amp; only a lone impulse, whose resampled shape a nonlinear model magnifies,
/// came out different.
inline constexpr std::size_t kNamResamplerLanczosHalfWidth = 12;

[[nodiscard]] inline int NamOversamplingFactorFromIndex(double value)
{
    const int index = std::clamp(static_cast<int>(std::llround(value)), 0, kNamOversamplingMaxIndex);
    return 1 << index;
}

[[nodiscard]] inline int NamOversamplingIndexFromFactor(int factor)
{
    int index = 0;
    int normalized = std::max(1, factor);

    while (normalized > 1 && index < kNamOversamplingMaxIndex)
    {
        normalized >>= 1;
        ++index;
    }

    return index;
}

[[nodiscard]] inline int SanitizeNamOversamplingIndex(double value)
{
    if (!IsFinite(value))
    {
        return kNamOversamplingIndexDefault;
    }

    return std::clamp(static_cast<int>(std::llround(value)), 0, kNamOversamplingMaxIndex);
}

[[nodiscard]] inline int SanitizeNamAntiAliasPhaseIndex(double value)
{
    if (!IsFinite(value))
    {
        return kNamAntiAliasPhaseIndexDefault;
    }

    return std::clamp(static_cast<int>(std::llround(value)), 0, kNamAntiAliasPhaseMaxIndex);
}

// Oversampling and its anti-alias filter phase are owned per NAM node, not by a
// process-wide global: a DAW loads every plugin instance into one process, so a
// shared static would force one quality tier on every instance in the project.
// PluginController pushes each instance's values down as node config, and
// SignalGraphExecutor keeps them as type defaults so nodes built later inherit them.
[[nodiscard]] inline dsp::EAntiAliasFilterPhase NamAntiAliasPhaseFromIndex(double value)
{
    switch (std::clamp(static_cast<int>(std::llround(value)), 0, kNamAntiAliasPhaseMaxIndex))
    {
    case 1:
        return dsp::EAntiAliasFilterPhase::LinearCascadedFIRShort;
    case 2:
        return dsp::EAntiAliasFilterPhase::LinearCascadedFIRLong;
    default:
        return dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR;
    }
}

[[nodiscard]] inline double ResolveNamOversampledRenderingRate(double modelSampleRate, double hostSampleRate,
                                                               int factor)
{
    if (modelSampleRate <= 0.0 || hostSampleRate <= 0.0)
    {
        return modelSampleRate;
    }

    if (factor <= 1)
    {
        return modelSampleRate;
    }

    const double requestedRate = hostSampleRate * static_cast<double>(factor);
    const double timeScale = std::max(1.0, std::round(requestedRate / modelSampleRate));
    return modelSampleRate * timeScale;
}

/**
 * One mono NAM lane using the NAM-Oversampler processing model:
 *
 *   host audio -> stateful AA upsampler -> time-scaled NAM -> AA downsampler
 *
 * The model's temporal dilation is multiplied by the same integer scale used
 * for its rendering rate, preserving the physical convolution timing.
 */
class NamOversamplingProcessor
{
  public:
    void Prepare(::nam::DSP& model, double hostSampleRate, double modelSampleRate, int maxBlockSize, int factor,
                 dsp::EAntiAliasFilterPhase filterPhase)
    {
        mHostSampleRate = hostSampleRate;
        mModelSampleRate = modelSampleRate;
        mMaxBlockSize = std::max(1, maxBlockSize);
        const int oversamplingFactor = std::clamp(factor, 1, 32);
        mFilterPhase = filterPhase;

        mRenderingSampleRate =
            ResolveNamOversampledRenderingRate(mModelSampleRate, mHostSampleRate, oversamplingFactor);

        if (mRenderingSampleRate <= 0.0)
        {
            mRenderingSampleRate = mHostSampleRate;
        }

        mTimeScale =
            static_cast<int>(std::max(1.0, std::round(mRenderingSampleRate / std::max(mModelSampleRate, 1.0))));
        mMaxRenderingBlockSize =
            std::max(1, static_cast<int>(std::ceil(static_cast<double>(mMaxBlockSize) * mRenderingSampleRate /
                                                   std::max(mHostSampleRate, 1.0))) +
                            1);

        mResamplingActive = std::abs(mRenderingSampleRate - mHostSampleRate) > 1.0e-6;

        if (mResamplingActive)
        {
            mResampler = std::make_unique<dsp::ResamplingContainer<NAM_SAMPLE, 1, kNamResamplerLanczosHalfWidth>>(
                mRenderingSampleRate, mFilterPhase, mModelSampleRate);
            mResampler->Reset(mHostSampleRate, mMaxBlockSize);

            if (mFilterPhase == dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR &&
                mResampler->HasSingleProcessCallbackPerBlock())
            {
                PrimeResampler();
            }

            mLatencySamples = MeasureRoundTripLatency();
        }
        else
        {
            mResampler.reset();
            mLatencySamples = 0;
        }

        model.SetTimeScale(mTimeScale);
        model.Reset(mRenderingSampleRate, mMaxRenderingBlockSize);
        mPrepared = true;
    }

    void Reset(::nam::DSP& model)
    {
        if (!mPrepared)
        {
            return;
        }

        if (mResampler)
        {
            mResampler->SetAntiAliasFilterPhase(mFilterPhase);
            mResampler->Reset(mHostSampleRate, mMaxBlockSize);
        }

        model.SetTimeScale(mTimeScale);
        model.Reset(mRenderingSampleRate, mMaxRenderingBlockSize);
    }

    void Process(::nam::DSP& model, NAM_SAMPLE* input, NAM_SAMPLE* output, int numFrames)
    {
        if (!input || !output || numFrames <= 0)
        {
            return;
        }

        NAM_SAMPLE* inputPointers[1] = {input};
        NAM_SAMPLE* outputPointers[1] = {output};

        if (!mResampler)
        {
            model.process(inputPointers, outputPointers, numFrames);
            return;
        }

        mResampler->ProcessBlock(
            inputPointers, outputPointers, numFrames,
            [&model](NAM_SAMPLE** renderingInput, NAM_SAMPLE** renderingOutput, int renderingFrames) {
                model.process(renderingInput, renderingOutput, renderingFrames);
            });
    }

    /// The delay the round trip through the resampler adds, as measured when it was prepared.
    [[nodiscard]] int GetLatencySamples() const noexcept
    {
        return mResampler ? mLatencySamples : 0;
    }

    [[nodiscard]] double GetRenderingSampleRate() const noexcept
    {
        return mRenderingSampleRate;
    }

    [[nodiscard]] int GetMaxRenderingBlockSize() const noexcept
    {
        return mMaxRenderingBlockSize;
    }

    [[nodiscard]] int GetTimeScale() const noexcept
    {
        return mTimeScale;
    }

    [[nodiscard]] bool IsResamplingActive() const noexcept
    {
        return mResamplingActive;
    }

  private:
    /// The resampler's own GetLatency() counts only the upsampler's pre-roll, not the
    /// downsampler's kernel or the anti-alias filters' delay, so it came out short whenever the
    /// rates differ: 27 samples reported against 33 heard at 44.1 kHz with a 48 kHz model, 70
    /// against 120 at 192 kHz, 91 against 210 with a linear-phase filter. The dry path of a NAM
    /// amp at Mix below 1 and the lanes of a blend are aligned by this figure, so the shortfall
    /// was a comb filter, and the host's delay compensation fell short by as much. Measured
    /// here instead, once: a smooth pulse goes through with the model passed by, and the shift
    /// of its first moment is the delay at low frequencies, where a guitar's energy is. Off the
    /// audio thread; the resampler is reset afterwards.
    [[nodiscard]] int MeasureRoundTripLatency()
    {
        const int fallback = mResampler->GetLatency();
        const double sigma = std::max(2.0, 0.0005 * mHostSampleRate);
        const double centre = 6.0 * sigma;
        const auto length = static_cast<int>(std::ceil(2.0 * centre + 0.03 * mHostSampleRate));
        std::vector<NAM_SAMPLE> input(static_cast<std::size_t>(length), static_cast<NAM_SAMPLE>(0.0));
        std::vector<NAM_SAMPLE> output(static_cast<std::size_t>(length), static_cast<NAM_SAMPLE>(0.0));

        for (int n = 0; n < length; ++n)
        {
            const double x = (static_cast<double>(n) - centre) / sigma;
            input[static_cast<std::size_t>(n)] = static_cast<NAM_SAMPLE>(0.5 * std::exp(-0.5 * x * x));
        }

        for (int start = 0; start < length; start += mMaxBlockSize)
        {
            const int frames = std::min(mMaxBlockSize, length - start);
            NAM_SAMPLE* inputPointers[1] = {input.data() + start};
            NAM_SAMPLE* outputPointers[1] = {output.data() + start};
            mResampler->ProcessBlock(
                inputPointers, outputPointers, frames,
                [](NAM_SAMPLE** in, NAM_SAMPLE** out, int numFrames) { std::copy_n(in[0], numFrames, out[0]); });
        }

        mResampler->Reset(mHostSampleRate, mMaxBlockSize);
        double inputSum = 0.0;
        double inputMoment = 0.0;
        double outputSum = 0.0;
        double outputMoment = 0.0;

        for (int n = 0; n < length; ++n)
        {
            const auto in = static_cast<double>(input[static_cast<std::size_t>(n)]);
            const auto out = static_cast<double>(output[static_cast<std::size_t>(n)]);
            inputSum += in;
            inputMoment += in * n;
            outputSum += out;
            outputMoment += out * n;
        }

        // A round trip that loses most of the pulse is not one to trust a moment from.
        if (!IsFinite(outputMoment) || std::abs(outputSum) < 0.5 * inputSum)
        {
            return fallback;
        }

        const double delay = outputMoment / outputSum - inputMoment / inputSum;
        return std::max(0, static_cast<int>(std::lround(delay)));
    }

    void PrimeResampler()
    {
        if (!mResampler)
        {
            return;
        }

        // The minimum-phase backend lazily creates one decimator state array on
        // its first block. Exercise that path during Prepare(), then clear its
        // state, so the audio-thread call is allocation-free from block one.
        std::vector<NAM_SAMPLE> silence(static_cast<std::size_t>(mMaxBlockSize), static_cast<NAM_SAMPLE>(0.0));
        std::vector<NAM_SAMPLE> discarded(static_cast<std::size_t>(mMaxBlockSize), static_cast<NAM_SAMPLE>(0.0));
        NAM_SAMPLE* inputPointers[1] = {silence.data()};
        NAM_SAMPLE* outputPointers[1] = {discarded.data()};
        mResampler->ProcessBlock(inputPointers, outputPointers, mMaxBlockSize,
                                 [](NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) {
                                     std::copy_n(input[0], numFrames, output[0]);
                                 });
        mResampler->Reset(mHostSampleRate, mMaxBlockSize);
    }

    std::unique_ptr<dsp::ResamplingContainer<NAM_SAMPLE, 1, kNamResamplerLanczosHalfWidth>> mResampler;
    double mHostSampleRate = 48000.0;
    double mModelSampleRate = 48000.0;
    double mRenderingSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    int mMaxRenderingBlockSize = 512;
    int mTimeScale = 1;
    int mLatencySamples = 0;
    dsp::EAntiAliasFilterPhase mFilterPhase = dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR;
    bool mResamplingActive = false;
    bool mPrepared = false;
};

/** Allocation-free fixed delay used to align an effect's dry signal with a
 * linear-phase oversampled wet path. Prepare() is called off the audio thread. */
class NamDryDelay
{
  public:
    void Prepare(int latencySamples, int maxBlockSize)
    {
        mLatencySamples = std::max(0, latencySamples);
        const std::size_t capacity = static_cast<std::size_t>(mLatencySamples + std::max(1, maxBlockSize) + 1);
        mBuffer.assign(capacity, 0.0f);
        mWritePosition = 0;
    }

    void Reset()
    {
        std::fill(mBuffer.begin(), mBuffer.end(), 0.0f);
        mWritePosition = 0;
    }

    void Process(float* samples, int numSamples)
    {
        if (!samples || numSamples <= 0 || mLatencySamples <= 0 || mBuffer.empty())
        {
            return;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            std::size_t readPosition = mWritePosition + mBuffer.size() - static_cast<std::size_t>(mLatencySamples);

            if (readPosition >= mBuffer.size())
            {
                readPosition -= mBuffer.size();
            }

            const float delayed = mBuffer[readPosition];
            mBuffer[mWritePosition] = samples[sampleIndex];
            samples[sampleIndex] = delayed;
            ++mWritePosition;

            if (mWritePosition == mBuffer.size())
            {
                mWritePosition = 0;
            }
        }
    }

  private:
    std::vector<float> mBuffer;
    std::size_t mWritePosition = 0;
    int mLatencySamples = 0;
};
} // namespace guitarfx
