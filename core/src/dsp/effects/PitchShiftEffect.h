#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/SignalsmithSupport.h"
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
 *
 * The bypass keeps feeding the dry history even though Stretch is idle, so that
 * re-engaging can seek Stretch onto real audio instead of replaying whatever it
 * was left holding. See SignalsmithSupport.h for the configuration policy and
 * the measurements behind both.
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

        ConfigureSignalsmithLive(mStretch, 2, sampleRate);
        // Mark configured before ApplyTranspose so preloaded semitones (SetParam
        // before Prepare) are applied to the stretch engine. Matches TransposeEffect.
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

        // Clamp to allocated buffer size to prevent out-of-bounds writes
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
    bool mNeedsEngage = true;

    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    SignalsmithDryHistory mDry;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
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
