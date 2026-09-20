#pragma once

#include "dsp/BiquadFrequency.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
namespace
{
constexpr double kSimpleCabPi = 3.14159265358979323846;
}

/**
 * Lightweight, filter-based 4x12 cabinet voicing. A broad low resonance and
 * sixth-order treble roll-off approximate the envelope of a close-miked cab;
 * an IR remains the option for a particular speaker and microphone's notches.
 */
class SimpleCabEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        UpdateCoefficients();
        Reset();
    }

    void Reset() override
    {
        for (auto& filter : mFilters)
        {
            filter.s1.fill(0.0);
            filter.s2.fill(0.0);
            filter.current = filter.target;
        }
        mCurrentMix = mMix;
        mRampSamplesRemaining = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        if (!mEnabled)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            AdvanceRamp();
            for (int ch = 0; ch < 2; ++ch)
            {
                if (!inputs[ch] || !outputs[ch])
                {
                    continue;
                }

                const double dry = static_cast<double>(inputs[ch][i]);
                double wet = dry;
                for (auto& filter : mFilters)
                {
                    wet = filter.Process(wet, ch);
                }
                outputs[ch][i] = static_cast<float>(dry * (1.0 - mCurrentMix) + wet * mCurrentMix);
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!std::isfinite(value))
        {
            return;
        }

        if (key == "bass")
        {
            mBass = std::clamp(value, 0.0, 1.0);
            UpdateCoefficients();
        }
        else if (key == "presence")
        {
            mPresence = std::clamp(value, 0.0, 1.0);
            UpdateCoefficients();
        }
        else if (key == "brightness")
        {
            mBrightness = std::clamp(value, 0.0, 1.0);
            UpdateCoefficients();
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
            BeginRamp();
        }
        else if (key == "enabled")
        {
            mEnabled = value > 0.5;
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bass")
        {
            return mBass;
        }
        if (key == "presence")
        {
            return mPresence;
        }
        if (key == "brightness")
        {
            return mBrightness;
        }
        if (key == "mix")
        {
            return mMix;
        }
        if (key == "enabled")
        {
            return mEnabled ? 1.0 : 0.0;
        }
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "cab_simple";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "cab";
    }

  private:
    struct Coefficients
    {
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct Biquad
    {
        Coefficients current, target, step;
        std::array<double, 2> s1 = {}, s2 = {};

        double Process(double input, int channel)
        {
            const double output = current.b0 * input + s1[channel];
            s1[channel] = current.b1 * input - current.a1 * output + s2[channel];
            s2[channel] = current.b2 * input - current.a2 * output;
            return output;
        }
    };

    enum FilterIndex
    {
        kHighPass,
        kLowResonance,
        kPresence,
        kLowPass1,
        kLowPass2,
        kLowPass3,
        kFilterCount
    };

    static Coefficients HighPass(double freq, double Q, double sampleRate)
    {
        const double w0 = 2.0 * kSimpleCabPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
        const double cosine = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double invA0 = 1.0 / (1.0 + alpha);
        return {(1.0 + cosine) * 0.5 * invA0, -(1.0 + cosine) * invA0,
                (1.0 + cosine) * 0.5 * invA0, -2.0 * cosine * invA0, (1.0 - alpha) * invA0};
    }

    static Coefficients LowPass(double freq, double Q, double sampleRate)
    {
        const double w0 = 2.0 * kSimpleCabPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
        const double cosine = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double invA0 = 1.0 / (1.0 + alpha);
        return {(1.0 - cosine) * 0.5 * invA0, (1.0 - cosine) * invA0,
                (1.0 - cosine) * 0.5 * invA0, -2.0 * cosine * invA0, (1.0 - alpha) * invA0};
    }

    static Coefficients PeakingEQ(double freq, double Q, double gainDb, double sampleRate)
    {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kSimpleCabPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
        const double cosine = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * Q);
        const double invA0 = 1.0 / (1.0 + alpha / A);
        return {(1.0 + alpha * A) * invA0, -2.0 * cosine * invA0,
                (1.0 - alpha * A) * invA0, -2.0 * cosine * invA0, (1.0 - alpha / A) * invA0};
    }

    void UpdateCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        // Bass changes the depth and size of the broad 4x12 low resonance.
        mFilters[kHighPass].target = HighPass(90.0 - mBass * 50.0, 0.707, mSampleRate);
        mFilters[kLowResonance].target = PeakingEQ(140.0, 0.6, 6.0 + mBass * 8.0, mSampleRate);

        const double presenceFreq = 2000.0 + mPresence * 1500.0;
        mFilters[kPresence].target = PeakingEQ(presenceFreq, 1.5, -1.0 + mPresence * 9.0, mSampleRate);

        // Three Butterworth sections give a smooth sixth-order speaker roll-off.
        const double lowPassFreq = 4200.0 + mBrightness * 2100.0;
        mFilters[kLowPass1].target = LowPass(lowPassFreq, 0.5176380902, mSampleRate);
        mFilters[kLowPass2].target = LowPass(lowPassFreq, 0.7071067812, mSampleRate);
        mFilters[kLowPass3].target = LowPass(lowPassFreq, 1.9318516526, mSampleRate);
        BeginRamp();
    }

    void BeginRamp()
    {
        const int samples = std::max(1, static_cast<int>(std::lround(mSampleRate * 0.015)));
        mRampSamplesRemaining = samples;
        for (auto& filter : mFilters)
        {
            const auto& from = filter.current;
            const auto& to = filter.target;
            filter.step = {(to.b0 - from.b0) / samples, (to.b1 - from.b1) / samples,
                           (to.b2 - from.b2) / samples, (to.a1 - from.a1) / samples,
                           (to.a2 - from.a2) / samples};
        }
        mMixStep = (mMix - mCurrentMix) / samples;
    }

    void AdvanceRamp()
    {
        if (mRampSamplesRemaining <= 0)
        {
            return;
        }

        if (--mRampSamplesRemaining == 0)
        {
            for (auto& filter : mFilters)
            {
                filter.current = filter.target;
            }
            mCurrentMix = mMix;
            return;
        }

        for (auto& filter : mFilters)
        {
            auto& c = filter.current;
            const auto& step = filter.step;
            c.b0 += step.b0;
            c.b1 += step.b1;
            c.b2 += step.b2;
            c.a1 += step.a1;
            c.a2 += step.a2;
        }
        mCurrentMix += mMixStep;
    }

    double mBass = 0.5;
    double mPresence = 0.5;
    double mBrightness = 0.5;
    double mMix = 1.0;
    double mCurrentMix = 1.0;
    double mMixStep = 0.0;
    int mRampSamplesRemaining = 0;
    std::array<Biquad, kFilterCount> mFilters = {};
};

inline void RegisterSimpleCabEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kCabSimple;
    info.aliases = {"cab_simple"};
    info.displayName = "Simple Cabinet";
    info.category = "cab";
    info.description = "Lightweight 4x12-style cabinet voicing (no IR required)";
    info.requiresResource = false;
    info.parameters = {{"bass", "Bass", 0.5, 0.0, 1.0, "amount"},
                       {"presence", "Presence", 0.5, 0.0, 1.0, "amount"},
                       {"brightness", "Brightness", 0.5, 0.0, 1.0, "amount"},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<SimpleCabEffect>(); });
}
} // namespace guitarfx
