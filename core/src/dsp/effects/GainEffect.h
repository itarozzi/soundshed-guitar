#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/LinearRamp.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace guitarfx
{
/**
 * Linear gain stage driven by a dB parameter. A change glides over kRampMs rather than
 * stepping at a block edge: automation and MIDI set it on the audio thread, and a pedal on
 * Gain moves in steps that would zipper.
 */
class GainEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mRampSamples = std::max(1, static_cast<int>(std::lround(kRampMs * 0.001 * sampleRate)));
        Reset();
    }

    /// The next block lands on the gain at once: a node's stored gain is applied after
    /// Prepare, and a fresh one should not fade from 0 dB.
    void Reset() override
    {
        mPrimed = false;
        mRemaining = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        const double target = std::pow(10.0, mGainDb.load(std::memory_order_relaxed) / 20.0);

        if (!mPrimed)
        {
            mGain.target = target;
            mGain.Finish();
            mPrimed = true;
        }
        else if (target != mGain.target)
        {
            mGain.target = target;
            mRemaining = mRampSamples;
            mGain.Begin(mRampSamples);
        }

        float* inL = inputs[0];
        float* inR = inputs[1];
        float* outL = (inL != nullptr) ? outputs[0] : nullptr;
        float* outR = (inR != nullptr) ? outputs[1] : nullptr;
        int i = 0;

        // The glide, a sample at a time; missing channels stay untouched.
        for (; i < numSamples && mRemaining > 0; ++i)
        {
            mGain.Advance();

            if (--mRemaining == 0)
            {
                mGain.Finish();
            }

            const auto gain = static_cast<float>(mGain.current);

            if (outL)
            {
                outL[i] = inL[i] * gain;
            }

            if (outR)
            {
                outR[i] = inR[i] * gain;
            }
        }

        const auto gain = static_cast<float>(mGain.current);

        for (; i < numSamples; ++i)
        {
            if (outL)
            {
                outL[i] = inL[i] * gain;
            }

            if (outR)
            {
                outR[i] = inR[i] * gain;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "gain" || key == "gainDb")
        {
            mGainDb.store(value, std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "gain" || key == "gainDb")
        {
            return mGainDb.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "gain";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

  private:
    static constexpr double kRampMs = 10.0;

    std::atomic<double> mGainDb{0.0};
    LinearRamp<double> mGain{1.0, 1.0, 0.0};
    int mRampSamples = 480;
    int mRemaining = 0;
    bool mPrimed = false;
};

// Register the effect
inline void RegisterGainEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kGain;
    info.aliases = {"gain"};
    info.displayName = "Gain";
    info.category = "utility";
    info.description = "Simple gain stage";
    info.requiresResource = false;
    info.parameters = {{"gainDb", "Gain", 0.0, -24.0, 24.0, "dB"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<GainEffect>(); });
}
} // namespace guitarfx
