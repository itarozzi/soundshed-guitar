#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/SignalsmithSupport.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Transpose effect using Signalsmith Stretch for integer semitone steps.
 *
 * Latency contract (Signalsmith docs):
 *   - When shifting: report inputLatency() + outputLatency(); delay dry by that
 *     amount before wet/dry mix so partial mix does not comb.
 *   - When transparent (0 st): bypass Stretch and report 0 latency.
 *
 * The bypass keeps feeding the dry history even though Stretch is idle, so that
 * re-engaging can seek Stretch onto real audio instead of replaying whatever it
 * was left holding. See SignalsmithSupport.h for the configuration policy and
 * the measurements behind both.
 */
class TransposeEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

        ConfigureSignalsmithLive(mStretch, 2, sampleRate);
        mConfigured = true;
        ApplyTranspose();
        mDry.Prepare(SignalsmithTotalLatencySamples(mStretch), mStretch.seekLength(), maxBlockSize);
        Reset();
    }

    void Reset() override
    {
        if (mConfigured)
        {
            mStretch.reset();
        }

        mDry.Reset();
        // Nothing has been fed yet, so the next shifting block re-seeks Stretch
        // onto whatever history has accumulated by then.
        mNeedsEngage = true;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        if (numSamples <= 0)
        {
            return;
        }

        // Transparent at 0 st: no Stretch latency, report 0 via GetLatencySamples().
        if (IsTransparent())
        {
            // Stretch is idle but the history is not: it is what re-engaging
            // seeks onto, so keep recording the input.
            for (int i = 0; i < numSamples; ++i)
            {
                mDry.Push(inputs[0] ? inputs[0][i] : 0.0f, inputs[1] ? inputs[1][i] : 0.0f);
            }

            CopyStereoInputToOutput(inputs, outputs, numSamples);
            mNeedsEngage = true;
            return;
        }

        if (!mConfigured)
        {
            return;
        }

        if (mNeedsEngage)
        {
            EngageSignalsmith(mStretch, mDry);
            mNeedsEngage = false;
        }

        if (static_cast<size_t>(numSamples) > mWetL.size())
        {
            mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
            mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        float* inputPtrs[2] = {inputs[0] ? inputs[0] : mZero.data(), inputs[1] ? inputs[1] : mZero.data()};
        float* wetPtrs[2] = {mWetL.data(), mWetR.data()};

        // Equal in/out lengths = pitch-only (no time stretch).
        mStretch.process(inputPtrs, numSamples, wetPtrs, numSamples);

        const float dryMix = static_cast<float>(1.0 - mMix);
        const float wetMix = static_cast<float>(mMix);

        const int latency = SignalsmithTotalLatencySamples(mStretch);

        for (int i = 0; i < numSamples; ++i)
        {
            // Unconditional: the history has to stay current for a later Mix
            // turn-down or re-engage, not just for this block's blend.
            float dryL = 0.0f;
            float dryR = 0.0f;
            mDry.PushAndRead(inputs[0] ? inputs[0][i] : 0.0f, inputs[1] ? inputs[1][i] : 0.0f, latency, dryL, dryR);

            if (outputs[0])
            {
                outputs[0][i] = dryL * dryMix + mWetL[static_cast<size_t>(i)] * wetMix;
            }

            if (outputs[1])
            {
                outputs[1][i] = dryR * dryMix + mWetR[static_cast<size_t>(i)] * wetMix;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "semitones")
        {
            const int clamped = static_cast<int>(std::round(std::clamp(value, -36.0, 12.0)));

            if (clamped != mSemitones)
            {
                mSemitones = clamped;
                ApplyTranspose();
            }
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
        if (key == "semitones")
        {
            return static_cast<double>(mSemitones);
        }

        if (key == "mix")
        {
            return mMix;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "transpose";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        if (!mConfigured || IsTransparent())
        {
            return 0;
        }

        return SignalsmithTotalLatencySamples(mStretch);
    }

  private:
    [[nodiscard]] bool IsTransparent() const
    {
        return mSemitones == 0;
    }

    void ApplyTranspose()
    {
        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }

        // Tonality limit is normalised to sample rate (Signalsmith API contract).
        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mStretch.setTransposeSemitones(static_cast<float>(mSemitones), tonalityLimit);
    }

    static constexpr double kTonalityLimitHz = 16000.0; // 8000

    int mSemitones = 0;
    double mMix = 1.0;
    bool mConfigured = false;
    bool mNeedsEngage = true;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    SignalsmithDryHistory mDry;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
};

inline void RegisterTransposeEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTranspose;
    info.aliases = {"transpose"};
    info.displayName = "Transpose";
    info.category = "pitch";
    info.description = "High-quality transpose effect";
    info.requiresResource = false;
    info.parameters = {{"semitones", "Semitones", 0.0, -36.0, 12.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<TransposeEffect>(); });
}
} // namespace guitarfx
