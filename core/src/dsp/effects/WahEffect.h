#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace guitarfx
{
namespace wah
{
constexpr double kPi = 3.14159265358979323846;

/// Centre frequencies are clamped to this fraction of the sample rate. The filter is stable
/// at any frequency, but past Nyquist the sweep would fold back down instead of rising.
constexpr double kMaxFilterFraction = 0.45;
constexpr double kMinFilterHz = 20.0;

/// Peak gain of a Q-1 resonance. The peak scales with sqrt(Q); see WahEffect on the gain law.
constexpr double kPeakGainScale = 2.0;

/// log2 units per dB.
constexpr double kLog2PerDb = 1.0 / (20.0 * 0.30102999566398120);

constexpr double kLowEndCutoffHz = 300.0;
constexpr double kTrebleShelfHz = 2500.0;

/// Damping added per unit of squared resonant output at Saturation 1.
constexpr float kSaturationScale = 2.0f;
/// Caps the damping term, so an absurd input cannot push the coefficients to extremes.
constexpr float kMaxSaturationDrive = 16.0f;

/// Smoothing for Level, Mix, Low End and Treble, which are knobs rather than the pedal.
constexpr double kControlSmoothingMs = 10.0;

/// Auto-Engage: a position at or below this counts as the heel...
constexpr double kHeelThreshold = 0.04;
/// ...the wah switches off once the pedal has rested there this long, so a rhythmic heel-toe
/// rock passes through without cutting out...
constexpr double kAutoOffHoldMs = 400.0;
/// ...and it fades between wah and dry over this long.
constexpr double kEngageFadeMs = 25.0;

/// A smoothed value this close to its target snaps onto it. In log2 units it is 0.1 cent.
constexpr double kSettleThreshold = 1.0e-4;

enum Param : std::size_t
{
    kPosition,
    kResponse,
    kAutoEngage,
    kHeelFreq,
    kToeFreq,
    kTaper,
    kQ,
    kToeQScale,
    kToeGain,
    kLowEnd,
    kTreble,
    kSaturation,
    kLevel,
    kMix,
    kParamCount
};

struct ParamSpec
{
    const char* id;
    const char* displayName;
    double defaultValue;
    double minValue;
    double maxValue;
    const char* unit;
    const char* group;
    bool advanced;
    double step;
};

/// The one place parameter ranges live: registration and SetParam's clamping both read it.
/// Entries are in `Param` order. The voicing defaults are the Cry Baby GCB-95 preset's, so a
/// wah built straight from the registry sounds like a new node does.
inline constexpr std::array<ParamSpec, kParamCount> kParams = {{
    {"position", "Pedal Position", 0.5, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"response", "Response", 12.0, 1.0, 150.0, "ms", "Pedal", true, 0.0},
    {"autoEngage", "Auto-Engage", 0.0, 0.0, 1.0, "toggle", "Pedal", false, 1.0},
    {"heelFreq", "Heel Freq", 440.0, 150.0, 1000.0, "Hz", "Voicing", false, 0.0},
    {"toeFreq", "Toe Freq", 2000.0, 600.0, 5000.0, "Hz", "Voicing", false, 0.0},
    {"taper", "Taper", -0.25, -1.0, 1.0, "amount", "Voicing", true, 0.0},
    {"q", "Q", 8.0, 0.5, 20.0, "", "Voicing", false, 0.0},
    {"toeQScale", "Toe Q Scale", 0.25, 0.1, 2.0, "x", "Voicing", true, 0.0},
    {"toeGain", "Toe Gain", 0.0, -12.0, 12.0, "dB", "Voicing", true, 0.0},
    {"lowEnd", "Low End", 0.15, 0.0, 1.0, "amount", "Voicing", false, 0.0},
    {"treble", "Treble", 0.0, -12.0, 12.0, "dB", "Voicing", false, 0.0},
    {"saturation", "Saturation", 0.2, 0.0, 1.0, "amount", "Voicing", true, 0.0},
    {"level", "Level", 0.0, -12.0, 18.0, "dB", "Output", false, 0.0},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0},
}};

[[nodiscard]] inline std::size_t FindParam(const std::string& key)
{
    for (std::size_t index = 0; index < kParamCount; ++index)
    {
        if (key == kParams[index].id)
        {
            return index;
        }
    }

    return kParamCount;
}

[[nodiscard]] inline float FlushDenormal(float value)
{
    return (std::fabs(value) < 1.0e-25f) ? 0.0f : value;
}

/// Bends pedal travel the way a pot taper does: 0 is even, positive packs the sweep toward the
/// heel, negative toward the toe.
[[nodiscard]] inline double WarpPosition(double position, double taper)
{
    return std::pow(std::clamp(position, 0.0, 1.0), std::pow(3.0, -taper));
}
} // namespace wah

/**
 * Conventional, pedal-controlled wah.
 *
 * The auto-wah drives its filter from the playing envelope. This one is driven by
 * `position`, meant to be mapped to an expression pedal, a MIDI CC or host automation. The
 * other parameters describe the pedal itself, so a factory preset is a model of a particular
 * wah rather than a setting of one.
 *
 * Per channel:
 *
 *   in -+- resonant bandpass (TPT state-variable filter) - x peak gain -+
 *       +- one-pole lowpass: non-resonant low-end bleed -- x Low End ---+- + - treble shelf - x Level - mix
 *
 * - The centre frequency moves exponentially from Heel Freq to Toe Freq, so equal pedal travel
 *   gives equal musical intervals. Taper bends that, as a pot's taper does.
 * - Q moves geometrically from Q at the heel to Q x Toe Q Scale at the toe. Inductor wahs are
 *   narrowest at the heel and open up toward the toe: Julius Smith's fit to a measured GCB-95
 *   runs from Q 8 to Q 2.
 * - The peak gain is 2 sqrt(Q), tilted by Toe Gain along the travel. A unity-peak bandpass
 *   passes pink-noise power in proportion to 1/Q whatever its frequency, so a sqrt(Q) peak holds
 *   pink noise, which is close to a guitar's long-term spectrum, at a steady level as the pedal
 *   sweeps and as Q is turned. Where Q falls toward the toe, the heel peak is the taller one:
 *   6 dB on the GCB-95's curve, in line with Holters and Zolzer's measured 6-8 dB.
 * - Saturation raises the filter's damping with the resonant stage's own output. A hot signal
 *   then flattens and widens the peak and adds odd harmonics, as a saturating inductor or a
 *   clipping transistor stage does, instead of the peak growing without limit.
 *
 * The state-variable filter is the trapezoidal (topology-preserving) form, which stays stable
 * and free of the artefacts a direct-form biquad produces when its frequency changes every
 * sample, as a pedal's constantly does.
 *
 * Pedal movement is smoothed over `response` ms in the log-frequency domain. A MIDI CC moves in
 * 1/127 steps, which would otherwise be audible as zipper noise, and an optical wah's
 * light-dependent resistor lags the pedal in much the same way.
 */
class WahEffect : public EffectProcessor
{
  public:
    WahEffect()
    {
        for (std::size_t index = 0; index < wah::kParamCount; ++index)
        {
            mValues[index] = wah::kParams[index].defaultValue;
        }

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
        UpdateRateCoefficients();
        Reset();
    }

    void Reset() override
    {
        mChannels = {};
        ComputeTargets();

        mPitch = mTargetPitch;
        mLogQ = mTargetLogQ;
        mLogGain = mTargetLogGain;
        mGliding = false;
        UpdateFilterCoefficients();

        mLevel = mTargetLevel;
        mMix = mTargetMix;
        mLowEnd = mTargetLowEnd;
        mTreble = mTargetTreble;

        // A wah loaded with its pedal already parked at the heel starts switched off, rather
        // than sounding for the hold time and then fading out.
        const bool parkedAtHeel = AutoEngageEnabled() && mValues[wah::kPosition] <= wah::kHeelThreshold;
        mHeelSamples = parkedAtHeel ? AutoOffHoldSamples() : 0;
        mEngage = EngageTarget();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        ComputeTargets();
        UpdateAutoEngage(numSamples);

        const double pedalSmoothing = SmoothingCoefficient(mValues[wah::kResponse]);
        const float engageTarget = EngageTarget();
        mGliding = mGliding || std::abs(mTargetPitch - mPitch) > wah::kSettleThreshold ||
                   std::abs(mTargetLogQ - mLogQ) > wah::kSettleThreshold ||
                   std::abs(mTargetLogGain - mLogGain) > wah::kSettleThreshold;

        for (int i = 0; i < numSamples; ++i)
        {
            if (mGliding)
            {
                GlidePedal(pedalSmoothing);
            }

            mLevel = Approach(mLevel, mTargetLevel, mControlSmoothing);
            mMix = Approach(mMix, mTargetMix, mControlSmoothing);
            mLowEnd = Approach(mLowEnd, mTargetLowEnd, mControlSmoothing);
            mTreble = Approach(mTreble, mTargetTreble, mControlSmoothing);
            mEngage = Approach(mEngage, engageTarget, mEngageSmoothing);

            const float wetAmount = mMix * mEngage;
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : inL;
            const float outL = ProcessSample(mChannels[0], inL, wetAmount);
            const float outR = ProcessSample(mChannels[1], inR, wetAmount);

            if (outputs[0])
            {
                outputs[0][i] = outL;
            }

            if (outputs[1])
            {
                outputs[1][i] = outR;
            }
        }

        // A non-finite input poisons the filter state for good. Clear it so the wah recovers
        // on the next block instead of emitting NaN until the preset is reloaded.
        for (auto& channel : mChannels)
        {
            if (!IsFinite(channel.ic1) || !IsFinite(channel.ic2) || !IsFinite(channel.low) ||
                !IsFinite(channel.treble) || !IsFinite(channel.resonant))
            {
                channel = {};
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        const std::size_t index = wah::FindParam(key);

        if (index == wah::kParamCount || !IsFinite(value))
        {
            return;
        }

        const auto& spec = wah::kParams[index];
        mValues[index] = std::clamp(value, spec.minValue, spec.maxValue);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = wah::FindParam(key); index != wah::kParamCount)
        {
            return mValues[index];
        }

        // Read-only state, for tests and any future pedal display.
        if (key == "currentFrequency")
        {
            return std::pow(2.0, mPitch);
        }

        if (key == "currentQ")
        {
            return std::pow(2.0, mLogQ);
        }

        if (key == "engaged")
        {
            return mEngage;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "wah";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    struct ChannelState
    {
        float ic1 = 0.0f; ///< state-variable filter integrator states
        float ic2 = 0.0f;
        float resonant = 0.0f; ///< last resonant-stage output, which drives Saturation
        float low = 0.0f;      ///< Low End one-pole state
        float treble = 0.0f;   ///< Treble shelf one-pole state
    };

    [[nodiscard]] static float Approach(float current, float target, float coefficient)
    {
        const float next = current + coefficient * (target - current);
        return (std::abs(target - next) < 1.0e-6f) ? target : next;
    }

    [[nodiscard]] double SmoothingCoefficient(double milliseconds) const
    {
        return 1.0 - std::exp(-1.0 / (std::max(milliseconds, 0.01) * 0.001 * mSampleRate));
    }

    /// G for a trapezoidal one-pole at `frequencyHz`.
    [[nodiscard]] float OnePoleCoefficient(double frequencyHz) const
    {
        const double cutoff = std::min(frequencyHz, mSampleRate * wah::kMaxFilterFraction);
        const double g = std::tan(wah::kPi * cutoff / mSampleRate);
        return static_cast<float>(g / (1.0 + g));
    }

    void UpdateRateCoefficients()
    {
        mControlSmoothing = static_cast<float>(SmoothingCoefficient(wah::kControlSmoothingMs));
        mEngageSmoothing = static_cast<float>(SmoothingCoefficient(wah::kEngageFadeMs));
        mLowEndCoefficient = OnePoleCoefficient(wah::kLowEndCutoffHz);
        mTrebleCoefficient = OnePoleCoefficient(wah::kTrebleShelfHz);
    }

    [[nodiscard]] bool AutoEngageEnabled() const
    {
        return mValues[wah::kAutoEngage] >= 0.5;
    }

    [[nodiscard]] int AutoOffHoldSamples() const
    {
        return static_cast<int>(wah::kAutoOffHoldMs * 0.001 * mSampleRate);
    }

    [[nodiscard]] float EngageTarget() const
    {
        return (AutoEngageEnabled() && mHeelSamples >= AutoOffHoldSamples()) ? 0.0f : 1.0f;
    }

    /// Counts at block resolution: the hold is a few hundred milliseconds, so a block's worth
    /// of slack is inaudible.
    void UpdateAutoEngage(int numSamples)
    {
        if (!AutoEngageEnabled() || mValues[wah::kPosition] > wah::kHeelThreshold)
        {
            mHeelSamples = 0;
        }
        else if (mHeelSamples < AutoOffHoldSamples())
        {
            mHeelSamples += numSamples;
        }
    }

    /// Everything the parameters ask for, read once per block. SetParam lands between blocks,
    /// so nothing finer is lost.
    void ComputeTargets()
    {
        const double warped = wah::WarpPosition(mValues[wah::kPosition], mValues[wah::kTaper]);
        const double heelPitch = std::log2(mValues[wah::kHeelFreq]);
        const double toePitch = std::log2(mValues[wah::kToeFreq]);
        const double maxPitch = std::log2(std::max(wah::kMinFilterHz, mSampleRate * wah::kMaxFilterFraction));

        mTargetPitch = std::clamp(heelPitch + warped * (toePitch - heelPitch), std::log2(wah::kMinFilterHz), maxPitch);
        mTargetLogQ = std::log2(mValues[wah::kQ]) + warped * std::log2(mValues[wah::kToeQScale]);
        mTargetLogGain =
            std::log2(wah::kPeakGainScale) + 0.5 * mTargetLogQ + warped * mValues[wah::kToeGain] * wah::kLog2PerDb;

        mTargetLevel = static_cast<float>(std::pow(10.0, mValues[wah::kLevel] / 20.0));
        mTargetMix = static_cast<float>(mValues[wah::kMix]);
        mTargetLowEnd = static_cast<float>(mValues[wah::kLowEnd]);
        mTargetTreble = static_cast<float>(std::pow(10.0, mValues[wah::kTreble] / 20.0));
        mSaturation = static_cast<float>(mValues[wah::kSaturation]) * wah::kSaturationScale;
    }

    void GlidePedal(double coefficient)
    {
        mPitch += coefficient * (mTargetPitch - mPitch);
        mLogQ += coefficient * (mTargetLogQ - mLogQ);
        mLogGain += coefficient * (mTargetLogGain - mLogGain);

        if (std::abs(mTargetPitch - mPitch) < wah::kSettleThreshold &&
            std::abs(mTargetLogQ - mLogQ) < wah::kSettleThreshold &&
            std::abs(mTargetLogGain - mLogGain) < wah::kSettleThreshold)
        {
            mPitch = mTargetPitch;
            mLogQ = mTargetLogQ;
            mLogGain = mTargetLogGain;
            mGliding = false;
        }

        UpdateFilterCoefficients();
    }

    void UpdateFilterCoefficients()
    {
        mG = static_cast<float>(std::tan(wah::kPi * std::pow(2.0, mPitch) / mSampleRate));
        mDamping = static_cast<float>(std::pow(2.0, -mLogQ));
        mPeakGain = static_cast<float>(std::pow(2.0, mLogGain));
    }

    [[nodiscard]] float ProcessSample(ChannelState& state, float input, float wetAmount) const
    {
        // Damping rises with the square of the resonant stage's last output.
        const float drive = std::min(state.resonant * state.resonant, wah::kMaxSaturationDrive);
        const float damping = mDamping * (1.0f + mSaturation * drive);

        const float a1 = 1.0f / (1.0f + mG * (mG + damping));
        const float a2 = mG * a1;
        const float a3 = mG * a2;
        const float v3 = input - state.ic2;
        const float v1 = a1 * state.ic1 + a2 * v3;
        const float v2 = state.ic2 + a2 * state.ic1 + a3 * v3;
        state.ic1 = wah::FlushDenormal(2.0f * v1 - state.ic1);
        state.ic2 = wah::FlushDenormal(2.0f * v2 - state.ic2);

        // The band output scaled by the nominal damping has unity peak gain; with saturation
        // raising the actual damping, that peak drops below unity.
        state.resonant = wah::FlushDenormal(mPeakGain * mDamping * v1);

        const float lowV = (input - state.low) * mLowEndCoefficient;
        const float low = lowV + state.low;
        state.low = wah::FlushDenormal(low + lowV);

        const float preShelf = state.resonant + mLowEnd * low;
        const float trebleV = (preShelf - state.treble) * mTrebleCoefficient;
        const float belowShelf = trebleV + state.treble;
        state.treble = wah::FlushDenormal(belowShelf + trebleV);

        const float wet = (belowShelf + mTreble * (preShelf - belowShelf)) * mLevel;
        return input + wetAmount * (wet - input);
    }

    std::array<double, wah::kParamCount> mValues{};
    std::array<ChannelState, 2> mChannels{};

    double mTargetPitch = 0.0; ///< log2 of the centre frequency
    double mTargetLogQ = 0.0;
    double mTargetLogGain = 0.0;
    double mPitch = 0.0;
    double mLogQ = 0.0;
    double mLogGain = 0.0;
    bool mGliding = false;

    float mG = 0.0f;
    float mDamping = 1.0f;
    float mPeakGain = 1.0f;
    float mSaturation = 0.0f;

    float mTargetLevel = 1.0f;
    float mTargetMix = 1.0f;
    float mTargetLowEnd = 0.0f;
    float mTargetTreble = 1.0f;
    float mLevel = 1.0f;
    float mMix = 1.0f;
    float mLowEnd = 0.0f;
    float mTreble = 1.0f;
    float mEngage = 1.0f;
    int mHeelSamples = 0;

    float mControlSmoothing = 1.0f;
    float mEngageSmoothing = 1.0f;
    float mLowEndCoefficient = 0.0f;
    float mTrebleCoefficient = 0.0f;
};

namespace wah
{
/**
 * How a factory preset voices the pedal.
 *
 * A factory preset models a pedal, so it sets every voicing parameter and deliberately leaves
 * the two performance parameters alone. Pedal Position belongs to whatever controller is
 * mapped to it and Auto-Engage to how the player switches the wah, so loading a voicing must
 * neither move the pedal nor switch the wah off under the player's foot.
 *
 * Sweep ranges come from the manufacturers' published specifications where they exist (every
 * Dunlop model here), from circuit analysis and measurement for the GCB-95 and Vox, and from
 * reviews and descriptions otherwise. Q, taper and the tone controls are voiced to the
 * character those sources describe. docs/fx-library.md lists the basis of each.
 */
struct Voicing
{
    const char* id;
    const char* displayName;
    double heelFreq;
    double toeFreq;
    double taper;
    double q;
    double toeQScale;
    double toeGain;
    double lowEnd;
    double treble;
    double saturation;
    double level;
    double response;
};

constexpr const char* kDefaultPresetId = "cry-baby-gcb95";

// clang-format off
inline constexpr std::array<Voicing, 22> kFactoryVoicings = {{
    //  id                              display name                        heel   toe     taper  Q     toeQ  toeGain low   treble sat   level resp
    {kDefaultPresetId,                  "Cry Baby GCB-95",                  440.0, 2000.0, -0.25, 8.0,  0.25, 0.0,    0.15, 0.0,   0.20, 0.0,  12.0},
    {"vox-v847",                        "Vox V847",                         450.0, 1600.0, -0.30, 6.0,  0.35, 0.0,    0.20, -1.0,  0.20, -1.0, 12.0},
    {"vox-clyde-mccoy-67",              "Vox Clyde McCoy '67",              420.0, 1700.0, -0.35, 11.0, 0.30, 0.0,    0.30, -1.0,  0.25, -1.0, 12.0},
    {"thomas-organ-cry-baby-68",        "Thomas Organ Cry Baby '68",        300.0, 1450.0, -0.20, 5.0,  0.40, 0.0,    0.30, -2.0,  0.30, -1.0, 12.0},
    {"cry-baby-535q",                   "Cry Baby 535Q",                    440.0, 2200.0, -0.20, 9.0,  0.30, 0.0,    0.15, 0.0,   0.15, 6.0,  12.0},
    {"cry-baby-95q",                    "Cry Baby 95Q",                     390.0, 2000.0, -0.20, 7.0,  0.30, 0.0,    0.20, 0.0,   0.15, 3.0,  12.0},
    {"jimi-hendrix-jh1d",               "Jimi Hendrix JH1D",                300.0, 1450.0, 0.20,  6.0,  0.33, 0.5,    0.30, -1.0,  0.25, 0.0,  12.0},
    {"evh95",                           "EVH95 Eddie Van Halen",            340.0, 2100.0, 0.00,  11.0, 0.30, 1.5,    0.20, 1.0,   0.20, 1.0,  12.0},
    {"slash-sw95",                      "Slash SW95",                       320.0, 1700.0, -0.10, 6.0,  0.35, 0.5,    0.30, 2.0,   0.45, 4.0,  12.0},
    {"dimebag-db01",                    "Dimebag Cry Baby From Hell",       295.0, 1400.0, -0.15, 10.0, 0.35, 0.0,    0.35, 1.0,   0.30, 6.0,  12.0},
    {"kirk-hammett-kh95",               "Kirk Hammett KH95",                340.0, 1600.0, -0.20, 9.0,  0.30, 4.5,    0.20, 0.0,   0.20, 1.0,  12.0},
    {"cry-baby-105q-bass",              "Cry Baby 105Q Bass",               180.0, 1800.0, -0.20, 5.0,  0.40, 7.5,    0.60, 0.0,   0.10, 2.0,  12.0},
    {"morley-power-wah",                "Morley Power Wah",                 250.0, 3200.0, 0.25,  3.0,  0.70, 0.0,    0.45, 1.0,   0.05, 0.0,  45.0},
    {"ibanez-weeping-demon",            "Ibanez Weeping Demon",             380.0, 2300.0, 0.00,  6.0,  0.80, 0.0,    0.70, 0.0,   0.05, 0.0,  25.0},
    {"fulltone-clyde-deluxe-jimi",      "Fulltone Clyde Deluxe · Jimi",     400.0, 1650.0, -0.35, 9.0,  0.30, 0.0,    0.30, -1.0,  0.30, 2.0,  12.0},
    {"fulltone-clyde-deluxe-wacked",    "Fulltone Clyde Deluxe · Wacked",   280.0, 1900.0, -0.30, 7.0,  0.35, 0.0,    0.70, 0.0,   0.35, 3.0,  12.0},
    {"real-mccoy-rmc3",                 "Real McCoy RMC3",                  420.0, 1800.0, -0.30, 8.0,  0.30, 0.0,    0.35, 0.0,   0.30, 0.0,  12.0},
    {"xotic-xw1",                       "Xotic XW-1",                       400.0, 1900.0, -0.30, 9.0,  0.30, 0.0,    0.40, 2.0,   0.25, 2.0,  12.0},
    {"budda-bud-wah",                   "Budda Bud-Wah",                    400.0, 1700.0, -0.20, 6.0,  0.35, 0.0,    0.35, -3.0,  0.35, 0.0,  12.0},
    {"colorsound-wah",                  "Colorsound Wah",                   300.0, 2400.0, 0.00,  6.0,  0.35, 0.0,    0.65, 0.0,   0.30, 1.0,  12.0},
    {"maestro-boomerang",               "Maestro Boomerang",                350.0, 1500.0, -0.50, 5.0,  0.40, 0.0,    0.40, -2.0,  0.25, 0.0,  12.0},
    {"mission-rewah-pro",               "Mission ReWah Pro",                330.0, 2500.0, -0.10, 7.0,  0.35, 0.0,    0.50, 1.0,   0.05, 0.0,  12.0},
}};
// clang-format on

[[nodiscard]] inline EffectPresetDefinition MakePreset(const Voicing& voicing)
{
    EffectPresetDefinition preset;
    preset.id = voicing.id;
    preset.displayName = voicing.displayName;
    preset.isFactory = true;
    preset.isDefault = (preset.id == kDefaultPresetId);
    preset.parameters = {{"heelFreq", voicing.heelFreq},     {"toeFreq", voicing.toeFreq},
                         {"taper", voicing.taper},           {"q", voicing.q},
                         {"toeQScale", voicing.toeQScale},   {"toeGain", voicing.toeGain},
                         {"lowEnd", voicing.lowEnd},         {"treble", voicing.treble},
                         {"saturation", voicing.saturation}, {"level", voicing.level},
                         {"response", voicing.response},     {"mix", 1.0}};
    preset.parameterOrder = {"heelFreq", "toeFreq", "taper",      "q",     "toeQScale", "toeGain",
                             "lowEnd",   "treble",  "saturation", "level", "response",  "mix"};
    return preset;
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    std::vector<EffectPresetDefinition> presets;
    presets.reserve(kFactoryVoicings.size());

    for (const auto& voicing : kFactoryVoicings)
    {
        presets.push_back(MakePreset(voicing));
    }

    return presets;
}
} // namespace wah

inline void RegisterWahEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kWah;
    info.aliases = {"wah"};
    info.displayName = "Wah";
    info.category = "modulation";
    info.description = "Pedal-controlled wah. Map Pedal Position to an expression pedal or MIDI CC; "
                       "factory presets voice classic and boutique wahs.";
    info.requiresResource = false;
    info.presets = wah::FactoryPresets();

    for (const auto& spec : wah::kParams)
    {
        info.parameters.push_back({spec.id,
                                   spec.displayName,
                                   spec.defaultValue,
                                   spec.minValue,
                                   spec.maxValue,
                                   spec.unit,
                                   spec.group,
                                   spec.advanced,
                                   spec.step,
                                   {}});
    }

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<WahEffect>(); });
}
} // namespace guitarfx
