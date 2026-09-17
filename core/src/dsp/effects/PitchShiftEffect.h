#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/SignalsmithLatency.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Pitch-shift effect using Signalsmith Stretch with a direct semitone control.
 *
 * The shift applied is `semitones` held inside the node's own range (`minSemitones` to
 * `maxSemitones`), rounded to a whole semitone while `stepMode` is on. Automation reads the
 * same range through GetAutomationRange(), so an expression pedal sweeps exactly that
 * interval: gliding when free, stepping when snapped.
 *
 * Latency contract (Signalsmith docs):
 *   - When shifting: report inputLatency() + outputLatency(); delay dry by that
 *     amount before wet/dry mix so partial mix does not comb.
 *   - When transparent (0 st): bypass Stretch and report 0 latency.
 *   - presetCheaper(..., splitComputation=false) keeps total latency lower;
 *     enabling splitComputation adds one hop of output latency for smoother CPU.
 */
class PitchShiftEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        mWetL.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mWetR.assign(static_cast<size_t>(maxBlockSize), 0.0f);
        mZero.assign(static_cast<size_t>(maxBlockSize), 0.0f);

        // splitComputation=false: avoid the extra interval of output latency.
        mStretch.presetCheaper(2, static_cast<float>(sampleRate), false);
        // Mark configured before ApplyTranspose so preloaded semitones (SetParam
        // before Prepare) are applied to the stretch engine. Matches TransposeEffect.
        mConfigured = true;
        ApplyTranspose();
        EnsureDryDelayCapacity();
        Reset();
    }

    void Reset() override
    {
        if (mConfigured)
        {
            mStretch.reset();
        }

        std::fill(mDryDelayL.begin(), mDryDelayL.end(), 0.0f);
        std::fill(mDryDelayR.begin(), mDryDelayR.end(), 0.0f);
        mDryWritePos = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs)
        {
            return;
        }

        // Clamp to allocated buffer size to prevent out-of-bounds writes
        numSamples = std::min(numSamples, mMaxBlockSize);

        if (numSamples <= 0)
        {
            return;
        }

        // Transparent at 0 st: no Stretch latency, report 0 via GetLatencySamples().
        if (IsTransparent())
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        if (!mConfigured)
        {
            return;
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
        const bool needDry = dryMix > 0.0f;

        if (needDry)
        {
            EnsureDryDelayCapacity();
        }

        const int latency = SignalsmithTotalLatencySamples(mStretch);

        for (int i = 0; i < numSamples; ++i)
        {
            float dryL = 0.0f;
            float dryR = 0.0f;

            if (needDry)
            {
                PushAndReadDry(inputs[0] ? inputs[0][i] : 0.0f, inputs[1] ? inputs[1][i] : 0.0f, latency, dryL, dryR);
            }

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
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "semitones")
        {
            mSemitones = std::clamp(value, kHardMinSemitones, kHardMaxSemitones);
        }
        else if (key == "minSemitones")
        {
            mMinSemitones = std::clamp(value, kHardMinSemitones, kHardMaxSemitones);
        }
        else if (key == "maxSemitones")
        {
            mMaxSemitones = std::clamp(value, kHardMinSemitones, kHardMaxSemitones);
        }
        else if (key == "stepMode")
        {
            mStepMode = value >= 0.5;
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
            return;
        }
        else
        {
            return;
        }

        ApplyTranspose();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "semitones")
        {
            return mSemitones;
        }

        if (key == "minSemitones")
        {
            return mMinSemitones;
        }

        if (key == "maxSemitones")
        {
            return mMaxSemitones;
        }

        if (key == "stepMode")
        {
            return mStepMode ? 1.0 : 0.0;
        }

        if (key == "mix")
        {
            return mMix;
        }

        return 0.0;
    }

    [[nodiscard]] bool GetAutomationRange(const std::string& key, ParamRange& range) const override
    {
        if (key != "semitones")
        {
            return false;
        }

        range.minValue = LowerBound();
        range.maxValue = UpperBound();
        range.step = mStepMode ? 1.0 : 0.0;
        return true;
    }

    /// The shift actually applied, after the range and the snap.
    [[nodiscard]] double GetAppliedSemitones() const
    {
        return mAppliedSemitones;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "pitch_shift";
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
        return std::abs(mAppliedSemitones) < 1.0e-9;
    }

    // The bounds are read in either order, so a preset loading them one at a time never
    // clamps one against the other's previous value.
    [[nodiscard]] double LowerBound() const
    {
        return std::min(mMinSemitones, mMaxSemitones);
    }

    [[nodiscard]] double UpperBound() const
    {
        return std::max(mMinSemitones, mMaxSemitones);
    }

    void ApplyTranspose()
    {
        // Snap first and clamp after, so the range wins when a bound is not a whole semitone.
        const double requested = mStepMode ? std::round(mSemitones) : mSemitones;
        mAppliedSemitones = std::clamp(requested, LowerBound(), UpperBound());

        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }

        // Tonality limit is normalised to sample rate (Signalsmith API contract).
        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mStretch.setTransposeSemitones(static_cast<float>(mAppliedSemitones), tonalityLimit);
    }

    void EnsureDryDelayCapacity()
    {
        if (!mConfigured)
        {
            return;
        }

        const int latency = SignalsmithTotalLatencySamples(mStretch);
        const size_t needed = static_cast<size_t>(std::max(latency, 0) + std::max(mMaxBlockSize, 1) + 8);

        if (mDryDelayL.size() < needed)
        {
            mDryDelayL.assign(needed, 0.0f);
            mDryDelayR.assign(needed, 0.0f);
            mDryWritePos = 0;
        }
    }

    void PushAndReadDry(float inL, float inR, int latency, float& outL, float& outR)
    {
        if (mDryDelayL.empty() || latency <= 0)
        {
            outL = inL;
            outR = inR;
            return;
        }

        const size_t size = mDryDelayL.size();
        mDryDelayL[mDryWritePos] = inL;
        mDryDelayR[mDryWritePos] = inR;

        const size_t delay = static_cast<size_t>(std::min(latency, static_cast<int>(size) - 1));
        const size_t readPos = (mDryWritePos + size - delay) % size;
        outL = mDryDelayL[readPos];
        outR = mDryDelayR[readPos];

        mDryWritePos = (mDryWritePos + 1) % size;
    }

    static constexpr double kTonalityLimitHz = 8000.0;

    static constexpr double kHardMinSemitones = -12.0;
    static constexpr double kHardMaxSemitones = 12.0;

    double mSemitones = 0.0;
    double mMinSemitones = kHardMinSemitones;
    double mMaxSemitones = kHardMaxSemitones;
    bool mStepMode = true;
    double mAppliedSemitones = 0.0;
    double mMix = 1.0;
    bool mConfigured = false;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
    std::vector<float> mDryDelayL;
    std::vector<float> mDryDelayR;
    size_t mDryWritePos = 0;
};

inline void RegisterPitchShiftEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kPitchShift;
    info.aliases = {"pitch_shift"};
    info.displayName = "Pitch Shift";
    info.category = "pitch";
    info.description = "Pitch shift, free or in whole semitones, within a range an expression pedal sweeps";
    info.requiresResource = false;
    // semitones keeps its declared step of 1 for renderers that do not know about stepMode,
    // which is on by default. The params panel and automation take the live range and step
    // from the node instead.
    info.parameters = {{"semitones", "Semitones", 0.0, -12.0, 12.0, "st", "", false, 1.0},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                       {"stepMode", "Snap to Semitone", 1.0, 0.0, 1.0, "toggle"},
                       {"minSemitones", "Range Min", -12.0, -12.0, 12.0, "st", "", false, 1.0},
                       {"maxSemitones", "Range Max", 12.0, -12.0, 12.0, "st", "", false, 1.0}};
    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<PitchShiftEffect>(); });
}
} // namespace guitarfx
