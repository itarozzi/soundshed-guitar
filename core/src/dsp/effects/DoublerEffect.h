#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/DelayLineSupport.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Doubler: mixes a short delayed copy with opposite polarity on the right
 * channel to create stereo width and thickness.
 */
class DoublerEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        // Room for the full 100 ms, the sample being written, and the one beyond it that a read
        // between samples blends in: a delay as long as the buffer would read back the sample
        // just written, which is no delay at all.
        const size_t maxSamples = static_cast<size_t>(std::ceil(sampleRate * 0.1)) + 2;
        mBufferL.resize(maxSamples, 0.0f);
        mBufferR.resize(maxSamples, 0.0f);
        mGlide.Prepare(sampleRate);
        mGlide.SetTimeConstantMs(kGlideMs);
        mGlide.SetMaxStep(delay_line::kMaxGlideStep);

        UpdateDelaySamples();
        Reset();
    }

    /// The next block lands on Time at once: a node's stored settings are applied after
    /// Prepare, and a fresh doubler should not glide to them.
    void Reset() override
    {
        std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
        std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
        mWriteIndex = 0;
        mGlidePrimed = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (mDelaySamples <= 0)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        const std::size_t bufSize = mBufferL.size();

        if (bufSize == 0)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        // Time glides, read between samples: a head that jumps from one whole sample to another
        // clicks on every step of the knob.
        const double target = std::min(static_cast<double>(mDelaySamples), static_cast<double>(bufSize) - 2.0);

        if (!mGlidePrimed)
        {
            mGlide.Snap(target);
            mGlidePrimed = true;
        }

        mGlide.SetTarget(target);

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : inL;

            // Circular delay keeps channel separation; polarity inversion on R widens
            // the image without summing extra low-frequency energy to mono.
            mBufferL[mWriteIndex] = inL;
            mBufferR[mWriteIndex] = inR;

            const double delay = std::max(0.0, mGlide.Next());
            const auto whole = static_cast<std::size_t>(delay);
            const auto fraction = static_cast<float>(delay - static_cast<double>(whole));
            const std::size_t readA = (mWriteIndex + bufSize - whole) % bufSize;
            const std::size_t readB = (readA == 0) ? bufSize - 1 : readA - 1;
            const float delayedL = mBufferL[readA] + (mBufferL[readB] - mBufferL[readA]) * fraction;
            const float delayedR = mBufferR[readA] + (mBufferR[readB] - mBufferR[readA]) * fraction;

            const float mixAmount = static_cast<float>(mMix);

            if (outputs[0])
            {
                outputs[0][i] = inL + delayedL * mixAmount;
            }

            if (outputs[1])
            {
                outputs[1][i] = inR - delayedR * mixAmount;
            }

            mWriteIndex = (mWriteIndex + 1) % bufSize;
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "time" || key == "timeMs")
        {
            mDelayMs = std::clamp(value, 0.0, 100.0);
            UpdateDelaySamples();
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "time" || key == "timeMs")
        {
            return mDelayMs;
        }

        if (key == "mix")
        {
            return mMix;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "delay_doubler";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "delay";
    }

  private:
    void UpdateDelaySamples()
    {
        mDelaySamples = static_cast<int>(mSampleRate * mDelayMs * 0.001);
    }

    static constexpr double kGlideMs = 30.0;

    double mDelayMs = 6.0; // Default 6ms delay
    double mMix = 0.3;     // Default mix level
    int mDelaySamples = 0;
    delay_line::GlideRamp mGlide; ///< the read head's delay, in samples, gliding to Time
    bool mGlidePrimed = false;    ///< false until the first block after Prepare or Reset

    std::vector<float> mBufferL;
    std::vector<float> mBufferR;
    std::size_t mWriteIndex = 0;
};

inline void RegisterDoublerEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDelayDoubler;
    info.aliases = {"delay_doubler"};
    info.displayName = "Doubler";
    info.category = "delay";
    info.description = "Creates stereo width by mixing a delayed copy of the signal";
    info.requiresResource = false;
    info.parameters = {{"time", "Delay Time", 6.0, 0.0, 100.0, "ms"}, {"mix", "Mix", 0.3, 0.0, 1.0, "amount"}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<DoublerEffect>(); });
}
} // namespace guitarfx
