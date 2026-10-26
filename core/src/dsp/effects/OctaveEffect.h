#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/SpliceTransposer.h"
#include "dsp/effects/SignalsmithSupport.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
/**
 * Octave effect with octave up/down blend, on either of two engines.
 *
 * Engines (`engine`), as Pitch Shift and Transpose have them:
 *   - High Quality (0): two Signalsmith Stretch instances, one per octave. 80 ms of latency. See
 *     SignalsmithSupport.h for the configuration policy and the measurements behind it.
 *   - Low Latency (1, the default): two SpliceTransposers, the engine Pitch Shift's Low Latency,
 *     Transpose and the Harmonizer use. Picks arrive a few milliseconds late, and it reports its
 *     tap's mean delay, 16 ms (docs/transpose-engine.md). A node stored before the switch has no
 *     `engine` key and loads on High Quality, the only engine it had (PitchEngineLegacyMigration.h).
 *
 * Transparent (mix 0, or neither octave dialled in) bypasses both engines and reports 0 latency.
 * Both engines' input histories are recorded on every sample, idle or not, so re-engaging starts
 * them on real audio rather than replaying whatever they were left holding.
 *
 * Every change of path crossfades over kPathFadeSeconds: into and out of the bypass, and from one
 * engine to the other, which both run until the fade ends. The processing path runs the dry
 * signal late by the engine's latency, so a hard switch (Mix leaving 0 on a pedal, say) jumped
 * the audio 80 ms in time with a click.
 */
class OctaveEffect : public EffectProcessor
{
  public:
    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    static constexpr double kPathFadeSeconds = 0.010;

    enum class Engine
    {
        HighQuality = 0,
        LowLatency = 1
    };

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        const size_t bufferSize = static_cast<size_t>(std::max(1, maxBlockSize));
        mWetUpL.assign(bufferSize, 0.0f);
        mWetUpR.assign(bufferSize, 0.0f);
        mWetDownL.assign(bufferSize, 0.0f);
        mWetDownR.assign(bufferSize, 0.0f);
        mZero.assign(bufferSize, 0.0f);

        ConfigureSignalsmithLive(mUpStretch, 2, sampleRate);
        ConfigureSignalsmithLive(mDownStretch, 2, sampleRate);
        // Fixed shifts, set before Prepare so its Reset starts each tap at its own.
        mLiveUp.SetSemitones(12.0, false);
        mLiveDown.SetSemitones(-12.0, false);
        mLiveUp.Prepare(sampleRate);
        mLiveDown.Prepare(sampleRate);
        mConfigured = true;
        ApplyStretchSettings();
        UpdateToneCoefficient();
        mDry.Prepare(SignalsmithTotalLatencySamples(mUpStretch, mDownStretch),
                     std::max(mUpStretch.seekLength(), mDownStretch.seekLength()), maxBlockSize);
        mPathFadeStep = 1.0f / std::max(1.0f, static_cast<float>(sampleRate * kPathFadeSeconds));
        Reset();
    }

    void Reset() override
    {
        if (mConfigured)
        {
            mUpStretch.reset();
            mDownStretch.reset();
            mLiveUp.Reset();
            mLiveDown.Reset();
        }

        mToneStateL = 0.0f;
        mToneStateR = 0.0f;
        mLiveToneStateL = 0.0f;
        mLiveToneStateR = 0.0f;
        mDry.Reset();
        // Nothing has been fed yet, so an engine that starts next re-seeks onto whatever history
        // has accumulated by then.
        mNeedsEngage = true;
        mLiveNeedsEngage = true;
        // No fade from whatever path was playing before: there is nothing to fade from.
        mDryGain = TargetGain(Path::Dry);
        mHighQualityGain = TargetGain(Path::HighQuality);
        mLowLatencyGain = TargetGain(Path::LowLatency);
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

        if (!mConfigured)
        {
            CopyDry(inputs, outputs, numSamples);
            return;
        }

        if (static_cast<size_t>(numSamples) > mWetUpL.size())
        {
            mWetUpL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetUpR.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetDownL.resize(static_cast<size_t>(numSamples), 0.0f);
            mWetDownR.resize(static_cast<size_t>(numSamples), 0.0f);
            mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }

        float* leftInput = inputs[0] ? inputs[0] : mZero.data();
        float* rightInput = inputs[1] ? inputs[1] : leftInput;

        const float dryTarget = TargetGain(Path::Dry);
        const float highQualityTarget = TargetGain(Path::HighQuality);
        const float lowLatencyTarget = TargetGain(Path::LowLatency);
        const bool runHighQuality = mHighQualityGain > 0.0f || highQualityTarget > 0.0f;
        const bool runLowLatency = mLowLatencyGain > 0.0f || lowLatencyTarget > 0.0f;

        if (!runHighQuality && !runLowLatency)
        {
            // Both engines are idle but their histories are not: they are what an engine
            // starting later begins on, so keep recording the input.
            for (int i = 0; i < numSamples; ++i)
            {
                mDry.Push(leftInput[i], rightInput[i]);
                mLiveUp.Write(leftInput[i], rightInput[i]);
                mLiveDown.Write(leftInput[i], rightInput[i]);
            }

            CopyDry(inputs, outputs, numSamples);
            mNeedsEngage = true;
            mLiveNeedsEngage = true;
            return;
        }

        if (runHighQuality)
        {
            if (mNeedsEngage)
            {
                EngageSignalsmith(mUpStretch, mDry);
                EngageSignalsmith(mDownStretch, mDry);
                mNeedsEngage = false;
            }

            float* inputPtrs[2] = {leftInput, rightInput};
            float* upPtrs[2] = {mWetUpL.data(), mWetUpR.data()};
            float* downPtrs[2] = {mWetDownL.data(), mWetDownR.data()};

            mUpStretch.process(inputPtrs, numSamples, upPtrs, numSamples);
            mDownStretch.process(inputPtrs, numSamples, downPtrs, numSamples);
        }
        else
        {
            mNeedsEngage = true;
        }

        if (runLowLatency && mLiveNeedsEngage)
        {
            mLiveUp.Engage();
            mLiveDown.Engage();
            mLiveNeedsEngage = false;
        }
        else if (!runLowLatency)
        {
            mLiveNeedsEngage = true;
        }

        const float dryMix = 1.0f - mMix;
        const float wetMix = mMix;

        const int highQualityLatency = SignalsmithTotalLatencySamples(mUpStretch, mDownStretch);
        // Both taps share the window, so one figure aligns the dry signal with either.
        const int lowLatencyLatency = mLiveUp.NominalLatencySamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = leftInput[i];
            const float inR = rightInput[i];

            // Unconditional: the histories have to stay current for a later Mix turn-down or
            // engine start, not just for this block's blend.
            float dryL = inL;
            float dryR = inR;
            mDry.PushAndRead(inL, inR, highQualityLatency, dryL, dryR);
            mLiveUp.Write(inL, inR);
            mLiveDown.Write(inL, inR);

            // The bypass, faded against the processing paths while one comes or goes.
            float sumL = mDryGain * inL;
            float sumR = mDryGain * inR;
            float gainSum = mDryGain;

            if (runHighQuality)
            {
                float wetL =
                    mWetUpL[static_cast<size_t>(i)] * mOctaveUp + mWetDownL[static_cast<size_t>(i)] * mOctaveDown;
                float wetR =
                    mWetUpR[static_cast<size_t>(i)] * mOctaveUp + mWetDownR[static_cast<size_t>(i)] * mOctaveDown;
                wetL = ApplyTone(wetL, mToneStateL);
                wetR = ApplyTone(wetR, mToneStateR);
                sumL += mHighQualityGain * (dryL * dryMix + wetL * wetMix);
                sumR += mHighQualityGain * (dryR * dryMix + wetR * wetMix);
                gainSum += mHighQualityGain;
            }

            if (runLowLatency)
            {
                float upL = 0.0f;
                float upR = 0.0f;
                float downL = 0.0f;
                float downR = 0.0f;
                mLiveUp.Process(upL, upR);
                mLiveDown.Process(downL, downR);
                float alignedL = 0.0f;
                float alignedR = 0.0f;
                mLiveUp.ReadDelayed(lowLatencyLatency, alignedL, alignedR);
                // Its own tone state: both engines run through an engine crossfade.
                const float wetL = ApplyTone(upL * mOctaveUp + downL * mOctaveDown, mLiveToneStateL);
                const float wetR = ApplyTone(upR * mOctaveUp + downR * mOctaveDown, mLiveToneStateR);
                sumL += mLowLatencyGain * (alignedL * dryMix + wetL * wetMix);
                sumR += mLowLatencyGain * (alignedR * dryMix + wetR * wetMix);
                gainSum += mLowLatencyGain;
            }

            // Divided by the gains' sum, so a fade that changes direction halfway never dips.
            const float scale = gainSum > 1.0e-6f ? 1.0f / gainSum : 0.0f;

            if (outputs[0])
            {
                outputs[0][i] = sumL * scale;
            }

            if (outputs[1])
            {
                outputs[1][i] = sumR * scale;
            }

            mDryGain = StepToward(mDryGain, dryTarget);
            mHighQualityGain = StepToward(mHighQualityGain, highQualityTarget);
            mLowLatencyGain = StepToward(mLowLatencyGain, lowLatencyTarget);
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "octaveUp")
        {
            mOctaveUp = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "octaveDown")
        {
            mOctaveDown = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "tone")
        {
            mTone = static_cast<float>(std::clamp(value, 0.0, 1.0));
            UpdateToneCoefficient();
        }
        else if (key == "mix")
        {
            mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
        }
        else if (key == "engine" && IsFinite(value))
        {
            // Only a target: the paths crossfade in Process, so this is safe on the audio thread.
            mEngine = value >= 0.5 ? Engine::LowLatency : Engine::HighQuality;
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "octaveUp")
        {
            return mOctaveUp;
        }

        if (key == "octaveDown")
        {
            return mOctaveDown;
        }

        if (key == "tone")
        {
            return mTone;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "engine")
        {
            return mEngine == Engine::LowLatency ? 1.0 : 0.0;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "octave";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        // Transparent dry path reports 0; wet path reports the engine's latency (for High Quality,
        // Signalsmith's input + output halves) for host delay compensation.
        if (!mConfigured || (IsTransparent() && mHighQualityGain <= 0.0f && mLowLatencyGain <= 0.0f))
        {
            return 0;
        }

        return mEngine == Engine::LowLatency ? mLiveUp.NominalLatencySamples()
                                             : SignalsmithTotalLatencySamples(mUpStretch, mDownStretch);
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kTonalityLimitHz = 16000.0;

    enum class Path
    {
        Dry,
        HighQuality,
        LowLatency
    };

    [[nodiscard]] float TargetGain(Path path) const
    {
        if (IsTransparent())
        {
            return path == Path::Dry ? 1.0f : 0.0f;
        }

        const Path engine = mEngine == Engine::LowLatency ? Path::LowLatency : Path::HighQuality;
        return path == engine ? 1.0f : 0.0f;
    }

    [[nodiscard]] float StepToward(float gain, float target) const
    {
        return gain < target ? std::min(target, gain + mPathFadeStep) : std::max(target, gain - mPathFadeStep);
    }

    [[nodiscard]] bool IsTransparent() const
    {
        return mMix <= 0.0f || (mOctaveUp <= 0.0f && mOctaveDown <= 0.0f);
    }

    void CopyDry(float** inputs, float** outputs, int numSamples)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            if (!outputs[ch])
            {
                continue;
            }

            if (inputs[ch])
            {
                std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
            }
            else if (ch == 1 && inputs[0])
            {
                std::copy(inputs[0], inputs[0] + numSamples, outputs[ch]);
            }
            else
            {
                std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
            }
        }
    }

    void ApplyStretchSettings()
    {
        if (!mConfigured || mSampleRate <= 0.0)
        {
            return;
        }

        const float tonalityLimit = static_cast<float>(kTonalityLimitHz / mSampleRate);
        mUpStretch.setTransposeSemitones(12.0f, tonalityLimit);
        mDownStretch.setTransposeSemitones(-12.0f, tonalityLimit);
    }

    void UpdateToneCoefficient()
    {
        const float minHz = 400.0f;
        const float maxHz = 4000.0f;
        const float cutoff = minHz + (maxHz - minHz) * mTone;
        const float x = static_cast<float>(2.0 * kPi * cutoff / std::max(1.0, mSampleRate));
        mToneCoef = 1.0f - std::exp(-x);
    }

    float ApplyTone(float input, float& state)
    {
        state += mToneCoef * (input - state);
        return state;
    }

    double mSampleRate = 48000.0;
    int mMaxBlockSize = 0;
    bool mConfigured = false;
    bool mNeedsEngage = true;
    bool mLiveNeedsEngage = true;
    Engine mEngine = Engine::LowLatency;
    float mPathFadeStep = 0.0f; ///< how far each path's gain moves per sample, over kPathFadeSeconds
    float mDryGain = 0.0f;
    float mHighQualityGain = 0.0f;
    float mLowLatencyGain = 1.0f;

    float mOctaveUp = 0.6f;
    float mOctaveDown = 0.6f;
    float mTone = 0.5f;
    float mMix = 1.0f;

    float mToneCoef = 0.0f;
    float mToneStateL = 0.0f;
    float mToneStateR = 0.0f;
    float mLiveToneStateL = 0.0f;
    float mLiveToneStateR = 0.0f;

    signalsmith::stretch::SignalsmithStretch<float> mUpStretch;
    signalsmith::stretch::SignalsmithStretch<float> mDownStretch;
    SignalsmithDryHistory mDry;
    SpliceTransposer mLiveUp;
    SpliceTransposer mLiveDown;
    std::vector<float> mWetUpL;
    std::vector<float> mWetUpR;
    std::vector<float> mWetDownL;
    std::vector<float> mWetDownR;
    std::vector<float> mZero;
};

inline void RegisterOctaveEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kOctave;
    info.aliases = {"octave"};
    info.displayName = "Octave";
    info.category = "pitch";
    info.description = "Octave up/down blend; the Low Latency engine keeps every attack on time";
    info.requiresResource = false;
    info.parameters = {{"octaveUp", "Oct Up", 0.6, 0.0, 1.0, "amount"},
                       {"octaveDown", "Oct Down", 0.6, 0.0, 1.0, "amount"},
                       {"tone", "Tone", 0.5, 0.0, 1.0, "amount"},
                       {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                       {"engine", "Engine", 1.0, 0.0, 1.0, "enum", "", false, 1.0, {"High Quality", "Low Latency"}}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<OctaveEffect>(); });
}
} // namespace guitarfx
