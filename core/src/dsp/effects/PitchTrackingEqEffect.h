#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/PitchTrackingEqSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace guitarfx
{
/**
 * Pitch-tracking EQ: an EQ whose bands follow the note being played.
 *
 * A fixed EQ band sits on a different harmonic of every note. Here each band is placed at a
 * multiple of the fundamental PitchTracker finds, so it lifts or cuts the same harmonic, and the
 * tone keeps the same character, anywhere on the neck.
 *
 *   in -+- band 1 - band 2 - band 3 - low-pass - x Level - out
 *       |     ^        ^        ^         ^
 *       +- pitch tracker ------------------+  (every band at Harmonic x f0)
 *
 * - Three peaking bands. Harmonic sets where (1 the fundamental, 2 the octave, 0.5 an octave
 *   below), Gain how much, and Width how wide, in harmonics: a band's bandwidth is Width x f0,
 *   so its Q is Harmonic / Width and a width under 1 touches one harmonic at any pitch.
 * - The optional low-pass is key-tracked, as a synth filter is: its cutoff is Cutoff x f0, so
 *   low notes are not fizzy and high ones not dull. Butterworth, 12 dB per octave.
 * - Until a first note is found the EQ is flat; it then fades in over kEngageMs. The pitch glides
 *   to each new note over Glide, in octaves. Through silence and chords, which the tracker cannot
 *   read, the bands stay on the last note it held.
 * - A band above kMaxFraction of the sample rate fades out over the half-octave below it.
 *
 * The tracker is monophonic and needs a few periods of a note before it reads it, so the attack
 * of each new note goes through with the previous note's EQ (around 20 ms for a low E).
 */
class PitchTrackingEqEffect : public EffectProcessor
{
  public:
    PitchTrackingEqEffect()
    {
        mTracker.Prepare(mSampleRate);
        UpdateRateCoefficients();
        Reset();
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mTracker.Prepare(sampleRate);
        UpdateRateCoefficients();
        Reset();
    }

    void Reset() override
    {
        mTracker.Reset();
        mStates = {};

        for (auto& ramp : mRamps)
        {
            ramp = {};
        }

        mPitch = 0.0;
        mEngage = 0.0;

        // The first block snaps every smoothed value onto its target, so parameters set before
        // any audio (a preset loading) start there rather than gliding in from the defaults.
        mStarted = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* inL = inputs[0] ? inputs[0] : inputs[1];
        const float* inR = inputs[1] ? inputs[1] : nullptr;

        if (!inL)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (outputs[ch])
                {
                    std::fill_n(outputs[ch], numSamples, 0.0f);
                }
            }

            return;
        }

        Render(inL, inR, outputs[0], outputs[1], numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(pitch_eq::kParams, key);

        if (index == pitch_eq::kParamCount)
        {
            return;
        }

        mValues[index] = NormaliseParamValue(pitch_eq::kParams[index], value);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(pitch_eq::kParams, key); index != pitch_eq::kParamCount)
        {
            return mValues[index];
        }

        // Read-only state, for tests and any future display.
        if (key == "trackedFrequency")
        {
            return mTracker.FrequencyHz();
        }

        if (key == "fundamental")
        {
            return mEngage > 0.0 ? std::exp2(mPitch) : 0.0;
        }

        if (key == "bandFrequency1" || key == "bandFrequency2" || key == "bandFrequency3")
        {
            const auto band = static_cast<std::size_t>(key.back() - '1');
            return mEngage > 0.0 ? BandHz(mSmoothed[pitch_eq::kHarmonic1 + band * pitch_eq::kBandStride]) : 0.0;
        }

        if (key == "lowPassFrequency")
        {
            return mEngage > 0.0 && mLowPassOn ? BandHz(mSmoothed[pitch_eq::kCutoff]) : 0.0;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "eq_pitch_tracking";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "eq";
    }

  private:
    /// The three bands and the low-pass, in the order they run.
    static constexpr std::size_t kSections = pitch_eq::kBandCount + 1;
    static constexpr std::size_t kLowPassSection = pitch_eq::kBandCount;

    using ChannelState = std::array<biquad::State, kSections>;

    /// The knobs that glide, by Param index; the enums and Glide itself are read as they are.
    static constexpr std::array<std::size_t, 10> kSmoothedParams = {
        pitch_eq::kHarmonic1, pitch_eq::kGain1,     pitch_eq::kWidth1, pitch_eq::kHarmonic2, pitch_eq::kGain2,
        pitch_eq::kWidth2,    pitch_eq::kHarmonic3, pitch_eq::kGain3,  pitch_eq::kWidth3,    pitch_eq::kCutoff};

    [[nodiscard]] static double Glide(double current, double target, double coefficient)
    {
        const double next = current + coefficient * (target - current);
        return (std::abs(target - next) < 1.0e-6) ? target : next;
    }

    /// The one-pole coefficient that covers `samples` samples of a `milliseconds` time constant.
    [[nodiscard]] double SmoothingCoefficient(double milliseconds, int samples) const
    {
        return 1.0 - std::exp(-static_cast<double>(samples) / (milliseconds * 0.001 * mSampleRate));
    }

    /// `cached` is the coefficient for a full control interval; a short last one is worked out.
    [[nodiscard]] double TickCoefficient(double cached, double milliseconds, int samples) const
    {
        return samples == pitch_eq::kControlInterval ? cached : SmoothingCoefficient(milliseconds, samples);
    }

    void UpdateRateCoefficients()
    {
        mControlGlide = SmoothingCoefficient(pitch_eq::kControlSmoothingMs, pitch_eq::kControlInterval);
        mEngageGlide = SmoothingCoefficient(pitch_eq::kEngageMs, pitch_eq::kControlInterval);
        mMaxHz = pitch_eq::kMaxFraction * mSampleRate;
    }

    /// `multiple` of the fundamental the EQ is following, in Hz.
    [[nodiscard]] double BandHz(double multiple) const
    {
        return multiple * std::exp2(mPitch);
    }

    void ComputeTargets()
    {
        mLowPassOn = mValues[pitch_eq::kLowPass] >= 0.5;
        mLevel = static_cast<float>(std::pow(10.0, mValues[pitch_eq::kLevel] / 20.0));
        mGlideMs = mValues[pitch_eq::kGlide];
        mPitchGlide = mGlideMs > 0.0 ? SmoothingCoefficient(mGlideMs, pitch_eq::kControlInterval) : 1.0;
        mTracker.SetLowestFrequency(pitch_tracker::LowestNoteHz(static_cast<int>(mValues[pitch_eq::kLowestNote])));
    }

    /// The section `band` should run now, from the smoothed knobs, the pitch and the engage fade.
    [[nodiscard]] BiquadCoefficients DesignBand(std::size_t band) const
    {
        using namespace pitch_eq;

        const std::size_t base = kHarmonic1 + band * kBandStride;
        const double harmonic = mSmoothed[base + kHarmonicOffset];
        const double hz = BandHz(harmonic);
        double gain = mSmoothed[base + kGainOffset] * mEngage;

        // Fade out over the half-octave below the ceiling, and hold the design there.
        gain *= std::clamp(std::log2(mMaxHz / std::max(hz, 1.0)) / kTopFadeOctaves, 0.0, 1.0);

        if (std::abs(gain) < 1.0e-4)
        {
            return {};
        }

        const double q = std::clamp(harmonic / mSmoothed[base + kWidthOffset], kMinQ, kMaxQ);
        return biquad::Peaking(std::clamp(hz, kMinBandHz, mMaxHz), q, gain, mSampleRate);
    }

    [[nodiscard]] BiquadCoefficients DesignLowPass() const
    {
        if (!mLowPassOn || mEngage <= 0.0)
        {
            return {};
        }

        const double hz = std::clamp(BandHz(mSmoothed[pitch_eq::kCutoff]), pitch_eq::kMinBandHz, mMaxHz);
        const BiquadCoefficients design = biquad::LowPass(hz, biquad::kButterworthQ, mSampleRate);

        // Between the identity and the design: every point on that line is a stable section.
        return BiquadCoefficients{} + (design - BiquadCoefficients{}) * mEngage;
    }

    /// The control-rate work for the next `samples` samples: the knobs' glides, the pitch, the
    /// engage fade, and each section's coefficient ramp.
    void UpdateControl(int samples)
    {
        const double controlGlide = TickCoefficient(mControlGlide, pitch_eq::kControlSmoothingMs, samples);

        for (const std::size_t index : kSmoothedParams)
        {
            mSmoothed[index] = Glide(mSmoothed[index], mValues[index], controlGlide);
        }

        if (const double tracked = mTracker.FrequencyHz(); tracked > 0.0)
        {
            const double target = std::log2(tracked);

            if (mEngage <= 0.0)
            {
                mPitch = target; // the first note: start on it rather than gliding in from nowhere
            }
            else
            {
                const double glide = mGlideMs > 0.0 ? TickCoefficient(mPitchGlide, mGlideMs, samples) : 1.0;
                mPitch = Glide(mPitch, target, glide);
            }

            mEngage = Glide(mEngage, 1.0, TickCoefficient(mEngageGlide, pitch_eq::kEngageMs, samples));
        }

        for (std::size_t band = 0; band < pitch_eq::kBandCount; ++band)
        {
            mRamps[band].target = DesignBand(band);
        }

        mRamps[kLowPassSection].target = DesignLowPass();

        for (auto& ramp : mRamps)
        {
            ramp.Begin(samples);
        }
    }

    void SnapToTargets()
    {
        for (const std::size_t index : kSmoothedParams)
        {
            mSmoothed[index] = mValues[index];
        }
    }

    [[nodiscard]] float ProcessSample(ChannelState& state, float input) const
    {
        double y = input;

        for (std::size_t s = 0; s < kSections; ++s)
        {
            y = state[s].Process(mRamps[s].current, y);
        }

        return static_cast<float>(y) * mLevel;
    }

    /// `inR` null runs the left channel alone.
    void Render(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
    {
        ComputeTargets();

        if (!mStarted)
        {
            SnapToTargets();
            mStarted = true;
        }

        const bool stereo = inR != nullptr;

        for (int start = 0; start < numSamples; start += pitch_eq::kControlInterval)
        {
            const int end = std::min(numSamples, start + pitch_eq::kControlInterval);
            UpdateControl(end - start);

            for (int i = start; i < end; ++i)
            {
                for (auto& ramp : mRamps)
                {
                    ramp.Advance();
                }

                // Read both inputs before writing: a host may process in place.
                const float left = inL[i];
                const float right = stereo ? inR[i] : left;
                mTracker.Push(stereo ? 0.5f * (left + right) : left);
                const float wetL = ProcessSample(mStates[0], left);

                if (outL)
                {
                    outL[i] = wetL;
                }

                if (stereo)
                {
                    const float wetR = ProcessSample(mStates[1], right);

                    if (outR)
                    {
                        outR[i] = wetR;
                    }
                }
                else if (outR)
                {
                    outR[i] = wetL;
                }
            }

            // Land exactly on the designs, however the steps rounded.
            for (auto& ramp : mRamps)
            {
                ramp.Finish();
            }
        }

        // A non-finite input poisons the filters for good. Clear them so the effect recovers
        // on the next block instead of emitting NaN until the preset is reloaded.
        for (auto& channel : mStates)
        {
            for (auto& section : channel)
            {
                if (!IsFinite(section.s1) || !IsFinite(section.s2))
                {
                    channel = {};
                    break;
                }
            }
        }
    }

    pitch_eq::ParamValues mValues = pitch_eq::kDefaultValues;
    /// The knobs in kSmoothedParams as they glide; the other entries are unused.
    pitch_eq::ParamValues mSmoothed = pitch_eq::kDefaultValues;

    std::array<ChannelState, 2> mStates{};
    std::array<biquad::Ramp, kSections> mRamps{};

    double mPitch = 0.0;  ///< log2 of the fundamental the bands follow
    double mEngage = 0.0; ///< 0 until a first note, then fades to 1
    bool mLowPassOn = false;
    float mLevel = 1.0f;
    bool mStarted = false;

    double mGlideMs = 0.0;
    double mPitchGlide = 1.0;
    double mControlGlide = 1.0;
    double mEngageGlide = 1.0;
    double mMaxHz = 20000.0;

    PitchTracker mTracker;
};

inline void RegisterPitchTrackingEqEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kEqPitchTracking;
    info.aliases = {"eq_pitch_tracking"};
    info.displayName = "Pitch Tracking EQ";
    info.category = "eq";
    info.description = "An EQ whose bands follow the note you play: each sits on a harmonic of it, so the same "
                       "harmonics are lifted or cut anywhere on the neck. A key-tracked low-pass keeps brightness "
                       "even. Experimental; single notes track best.";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(pitch_eq::kParams);
    info.presets = pitch_eq::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<PitchTrackingEqEffect>(); });
}
} // namespace guitarfx
