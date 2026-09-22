#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/DrivePedal.h"
#include "dsp/effects/DriveStages.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace guitarfx
{
namespace distortion
{
enum Param : std::size_t
{
    kModel,
    kDrive,
    kTone,
    kTight,
    kClipping,
    kLow,
    kMid,
    kMidFreq,
    kHigh,
    kLevel,
    kMix,
    kParamCount
};

/// Index order is stored in presets: append new models, never reorder.
enum class Model
{
    Rat,
    Ds1,
    DistortionPlus,
    MetalZone,
    Count
};

inline constexpr const char* kModelLabels[] = {"RAT", "DS-1", "Distortion+", "Metal Zone"};
static_assert(std::size(kModelLabels) == static_cast<std::size_t>(Model::Count));

inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"model", "Model", 0.0, 0.0, 3.0, "enum", "Pedal", false, 1.0, kModelLabels},
    {"drive", "Drive", 0.6, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"tone", "Tone", 0.5, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"tight", "Tight", 0.5, 0.0, 1.0, "amount", "Voicing", false, 0.0},
    {"clipping", "Clipping", 0.0, 0.0, 6.0, "enum", "Voicing", false, 1.0, drive::kClipChoiceLabels},
    {"low", "Low", 0.0, -15.0, 15.0, "dB", "EQ", false, 0.0},
    {"mid", "Mid", 0.0, -15.0, 15.0, "dB", "EQ", false, 0.0},
    LogTaper({"midFreq", "Mid Freq", 800.0, 200.0, 5000.0, "Hz", "EQ", false, 0.0}),
    {"high", "High", 0.0, -15.0, 15.0, "dB", "EQ", false, 0.0},
    {"level", "Level", 0.0, -24.0, 24.0, "dB", "Output", false, 0.0},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0},
}};

/// The EQ bands every model shares, the Metal Zone's controls: a low shelf, a sweepable mid
/// bell and a high shelf, flat at 0 dB.
inline constexpr double kLowShelfHz = 100.0;
inline constexpr double kHighShelfHz = 5000.0;
inline constexpr double kMidQ = 0.9;

inline constexpr double kInputHighPassHz = 20.0;
inline constexpr double kDcBlockHz = 8.0;

/// Unity-gain bandwidth of the op-amps: LM308 (RAT, with its 30 pF compensation) and 741
/// (Distortion+) alike. Closed-loop bandwidth is this over the gain, which is what keeps a
/// RAT at full distortion from turning to fizz.
///
/// The LM308's slew rate is left out on purpose. After the diodes it only shapes edges above
/// 50 kHz, and a sample-by-sample slew limiter starts each ramp on the sample grid, which
/// aliases as badly as the naive clipper it would sit beside (measured: -52 dB of inharmonic
/// lines at 1.3 kHz with it, -97 dB without).
inline constexpr double kOpAmpGbwHz = 1.0e6;

/**
 * Every model is a non-inverting op-amp stage whose gain is set by RC legs to ground, the
 * shape that decides where the gain goes in frequency, followed by clipping diodes to ground
 * and the pedal's tone circuit:
 *
 *     booster -> x + Rd (legs) -> feedback cap -> op-amp bandwidth -> rails -> diodes
 *
 * - RAT: two legs, 47R + 2.2 uF (1.5 kHz) and 560R + 4.7 uF (60 Hz), under a 100k Distortion
 *   pot: up to 45 dB through the mids and 67 dB above 1.5 kHz, but the LM308's bandwidth
 *   pulls the top back down. 1N914s to ground, then the Filter: 1.5k plus 100k against
 *   3.3 nF, 32 kHz down to 475 Hz.
 * - DS-1: a transistor booster ahead of the op-amp, silicon diodes to ground, and a passive
 *   tone stack that blends a 234 Hz low-pass with a 1.06 kHz high-pass: scooped at noon.
 * - Distortion+: one leg, 4.7k + 47 nF (720 Hz), under a 1M pot; a 741 whose bandwidth
 *   falls with gain; germanium diodes to ground and a 16 kHz roll-off. No tone control.
 * - Metal Zone: a mid-forward pre-emphasis, a first stage clipping softly in the op-amp's
 *   feedback, a second driving hard into diodes, and a steep roll-off after. The shared
 *   Low / Mid / Mid Freq / High EQ is its EQ section.
 *
 * Tight moves the bass corner feeding the gain two octaves either way (up is tighter).
 * Where a pedal has no tone control, Tone is a tilt that is flat at noon.
 */
enum class ToneStyle
{
    LowPass, ///< the RAT's Filter
    Blend,   ///< the DS-1's low-pass/high-pass blend
    Tilt     ///< flat at noon
};

struct Voicing
{
    double boostDb;         ///< fixed booster ahead of the op-amp (0 dB: none)
    double boostPerDriveDb; ///< and how much of Drive it takes (the Metal Zone's first stage)
    drive::ClipCurve boostClip;
    double drivePotOhms; ///< the gain pot, audio taper
    double leg1Ohms;
    double leg1Farads;
    double leg2Ohms; ///< 0: no second leg
    double leg2Farads;
    double feedbackFarads; ///< across the gain pot (0: none)
    bool limitBandwidth;   ///< apply kOpAmpGbwHz
    double railVolts;
    drive::ClipCurve stockClip;
    double clipLevelExponent; ///< see ClipLevelCompensation
    double stage2Gain;        ///< Metal Zone's second stage (0: none)
    double postLowPassHz;     ///< at the oversampled rate, after the diodes
    double preEmphasisDb;     ///< host-rate bell ahead of the gain (Metal Zone)
    double preEmphasisHz;
    ToneStyle tone;
    double toneLowHz;
    double toneHighHz;
    bool steepRollOff; ///< a second-order low-pass after the tone (Metal Zone)
    drive::TrimTable trimDb;
};

// clang-format off
// *INDENT-OFF*
inline constexpr std::array<Voicing, static_cast<std::size_t>(Model::Count)> kVoicings = {{
    // RAT
    {0.0,  0.0,  {10.0, 10.0, drive::Knee::Soft, drive::Knee::Soft}, 100.0e3, 47.0,  2.2e-6,  560.0, 4.7e-6, 100.0e-12, true,  4.2,
     {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.97, 0.0, 60000.0, 0.0, 1000.0, ToneStyle::LowPass, 0.0, 0.0, false, {5.6, -2.4, -3.3, -3.6, -3.8, -3.9, -4.1, -4.2, -4.3}},
    // DS-1
    {14.0, 0.0,  {3.0, 3.0, drive::Knee::Soft, drive::Knee::Soft},   100.0e3, 4.7e3, 0.47e-6, 0.0,   0.0,    250.0e-12, false, 4.2,
     {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.84, 0.0, 60000.0, 0.0, 1000.0, ToneStyle::Blend, 234.0, 1063.0, false, {4.0, 3.6, 3.0, 2.5, 1.9, 1.4, 1.0, 0.7, 0.6}},
    // Distortion+
    {0.0,  0.0,  {10.0, 10.0, drive::Knee::Soft, drive::Knee::Soft}, 1.0e6,   4.7e3, 47.0e-9, 0.0,   0.0,    0.0,       true,  3.5,
     {0.35, 0.35, drive::Knee::Gradual, drive::Knee::Gradual}, 0.88, 0.0, 16000.0, 0.0, 1000.0, ToneStyle::Tilt, 1000.0, 1000.0, false, {5.2, 2.0, 0.1, -0.9, -1.6, -2.0, -2.2, -2.3, -2.3}},
    // Metal Zone
    {10.0, 30.0, {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.0, 1.0e3, 0.53e-6, 0.0,   0.0,    0.0,       false, 4.2,
     {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.06, 8.0, 9000.0, 9.0, 1200.0, ToneStyle::Tilt, 1000.0, 1000.0, true, {-5.9, -5.9, -5.9, -5.9, -5.9, -5.9, -5.9, -5.9, -5.9}},
}};
// *INDENT-ON*
// clang-format on

/// The fixed op-amp gain of the Metal Zone's second stage, through its RC leg.
inline constexpr double kMetalZoneStageOhms = 10.0e3;

struct Traits
{
    static constexpr const char* kTypeName = "distortion";
    static constexpr const auto& kParams = distortion::kParams;
    static constexpr std::size_t kLevel = distortion::kLevel;
    static constexpr std::size_t kMix = distortion::kMix;

    struct Coefficients
    {
        double inputHighPass = 0.0;
        BiquadCoefficients preEmphasis;
        bool hasBoost = false;
        double boost = 1.0;
        drive::ClipCurve boostClip{10.0, 10.0, drive::Knee::Soft, drive::Knee::Soft};
        double leg1 = 0.0;
        double leg1Gain = 0.0;
        double leg2 = 0.0;
        double leg2Gain = 0.0;
        double feedbackLowPass = 1.0;
        double bandwidth = 1.0;
        drive::ClipCurve rails{4.2, 4.2, drive::Knee::Hard, drive::Knee::Hard};
        drive::ClipCurve clip;
        double stage2Gain = 0.0;
        drive::ClipCurve stage2Clip{0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft};
        double postLowPass = 1.0;
        double toneLow = 0.0;
        double toneLowGain = 1.0;
        double toneHigh = 0.0;
        double toneHighGain = 0.0;
        BiquadCoefficients rollOff;
        BiquadCoefficients eqLow;
        BiquadCoefficients eqMid;
        BiquadCoefficients eqHigh;
        double dcBlock = 0.0;
        double trim = 1.0;
    };

    struct State
    {
        drive::OnePole input;
        biquad::State preEmphasis;
        drive::OnePole leg1;
        drive::OnePole leg2;
        drive::OnePole feedback;
        drive::OnePole bandwidth;
        drive::OnePole postLowPass;
        drive::OnePole toneLow;
        drive::OnePole toneHigh;
        biquad::State rollOff;
        biquad::State eqLow;
        biquad::State eqMid;
        biquad::State eqHigh;
        drive::OnePole dcBlock;
        drive::AntialiasMemory boostClip;
        drive::AntialiasMemory rails;
        drive::AntialiasMemory clip;
        drive::AntialiasMemory stage2Clip;

        void Reset() noexcept
        {
            *this = State{};
        }
    };

    static void Design(Coefficients& c, const std::array<double, kParamCount>& values, double rate, double osRate)
    {
        const auto modelIndex = static_cast<std::size_t>(
            std::clamp(static_cast<int>(values[kModel]), 0, static_cast<int>(Model::Count) - 1));
        const Voicing& v = kVoicings[modelIndex];
        const double amount = values[kDrive];
        const double tone = values[kTone];
        const double tight = drive::KnobOctaves(values[kTight], 2.0);

        c.inputHighPass = drive::OnePoleCoefficient(kInputHighPassHz, rate);
        c.preEmphasis = v.preEmphasisDb != 0.0 ? biquad::Peaking(v.preEmphasisHz, 0.6, v.preEmphasisDb, rate)
                                               : BiquadCoefficients{};
        c.hasBoost = v.boostDb != 0.0 || v.boostPerDriveDb != 0.0;
        c.boost = drive::DbToGain(v.boostDb + v.boostPerDriveDb * amount);
        c.boostClip = v.boostClip;

        // The gain pot sets how hard the legs pull; the Metal Zone's second stage is fixed.
        const double feedbackOhms =
            v.drivePotOhms > 0.0 ? drive::AudioTaperOhms(v.drivePotOhms, amount) : kMetalZoneStageOhms;
        c.leg1 = drive::OnePoleCoefficient(drive::RcHz(v.leg1Ohms, v.leg1Farads) * tight, osRate);
        c.leg1Gain = feedbackOhms / v.leg1Ohms;
        c.leg2 =
            v.leg2Ohms > 0.0 ? drive::OnePoleCoefficient(drive::RcHz(v.leg2Ohms, v.leg2Farads) * tight, osRate) : 0.0;
        c.leg2Gain = v.leg2Ohms > 0.0 ? feedbackOhms / v.leg2Ohms : 0.0;
        c.feedbackLowPass = v.feedbackFarads > 0.0
                                ? drive::OnePoleCoefficient(drive::RcHz(feedbackOhms, v.feedbackFarads), osRate)
                                : drive::OnePoleCoefficient(osRate, osRate);

        // Closed-loop bandwidth: the unity-gain bandwidth over the highest gain the legs reach.
        const double legConductance = 1.0 / v.leg1Ohms + (v.leg2Ohms > 0.0 ? 1.0 / v.leg2Ohms : 0.0);
        const double peakGain = 1.0 + feedbackOhms * legConductance;
        c.bandwidth = drive::OnePoleCoefficient(v.limitBandwidth ? kOpAmpGbwHz / peakGain : osRate, osRate);
        c.rails = {v.railVolts, v.railVolts, drive::Knee::Hard, drive::Knee::Hard};

        const auto choice = static_cast<drive::ClipChoice>(
            std::clamp(static_cast<int>(values[kClipping]), 0, static_cast<int>(drive::ClipChoice::Count) - 1));
        c.clip = drive::ClipFor(choice, v.stockClip);
        c.stage2Gain = v.stage2Gain > 0.0 ? drive::DbToGain(v.stage2Gain) : 0.0;
        c.postLowPass = drive::OnePoleCoefficient(v.postLowPassHz, osRate);

        switch (v.tone)
        {
        case ToneStyle::LowPass: {
            // The Filter pot is linear; squaring the rotation spreads the bright end,
            // where it is played, across more of the knob.
            const double filterOhms = 100.0e3 * (1.0 - tone) * (1.0 - tone);
            c.toneLow = drive::OnePoleCoefficient(drive::RcHz(1.5e3 + filterOhms, 3.3e-9), rate);
            c.toneLowGain = 1.0;
            c.toneHigh = c.toneLow;
            c.toneHighGain = 0.0;
            break;
        }

        case ToneStyle::Blend:
            c.toneLow = drive::OnePoleCoefficient(v.toneLowHz, rate);
            c.toneLowGain = 1.0 - tone;
            c.toneHigh = drive::OnePoleCoefficient(v.toneHighHz, rate);
            c.toneHighGain = tone;
            break;

        case ToneStyle::Tilt:
            c.toneLow = drive::OnePoleCoefficient(v.toneLowHz, rate);
            c.toneLowGain = drive::DbToGain(-(tone - 0.5) * 12.0);
            c.toneHigh = c.toneLow;
            c.toneHighGain = drive::DbToGain((tone - 0.5) * 12.0);
            break;
        }

        c.rollOff = v.steepRollOff ? biquad::LowPass(6500.0, biquad::kButterworthQ, rate) : BiquadCoefficients{};
        c.eqLow = biquad::LowShelf(kLowShelfHz, biquad::kButterworthQ, values[kLow], rate);
        c.eqMid = biquad::Peaking(values[kMidFreq], kMidQ, values[kMid], rate);
        c.eqHigh = biquad::HighShelf(kHighShelfHz, biquad::kButterworthQ, values[kHigh], rate);
        c.dcBlock = drive::OnePoleCoefficient(kDcBlockHz, rate);
        c.trim = drive::DbToGain(drive::InterpolateTrimDb(v.trimDb, amount)) *
                 drive::ClipLevelCompensation(v.stockClip, c.clip, v.clipLevelExponent);
    }

    static double Pre(const Coefficients& c, State& s, double volts) noexcept
    {
        return s.preEmphasis.Process(c.preEmphasis, s.input.HighPass(c.inputHighPass, volts));
    }

    static double Shape(const Coefficients& c, State& s, double x) noexcept
    {
        const double boosted = c.hasBoost ? c.boostClip.Antialiased(x * c.boost, s.boostClip) : x;
        double legs = c.leg1Gain * s.leg1.HighPass(c.leg1, boosted);

        if (c.leg2Gain > 0.0)
        {
            legs += c.leg2Gain * s.leg2.HighPass(c.leg2, boosted);
        }

        double o = boosted + s.feedback.LowPass(c.feedbackLowPass, legs);
        o = c.rails.Antialiased(s.bandwidth.LowPass(c.bandwidth, o), s.rails);
        o = c.clip.Antialiased(o, s.clip);

        if (c.stage2Gain > 0.0)
        {
            o = c.stage2Clip.Antialiased(o * c.stage2Gain, s.stage2Clip);
        }

        return s.postLowPass.LowPass(c.postLowPass, o);
    }

    static double Post(const Coefficients& c, State& s, double y) noexcept
    {
        const double low = s.toneLow.LowPass(c.toneLow, y);
        const double high = y - s.toneHigh.LowPass(c.toneHigh, y);
        double toned = low * c.toneLowGain + high * c.toneHighGain;
        toned = s.rollOff.Process(c.rollOff, toned);
        toned = s.eqHigh.Process(c.eqHigh, s.eqMid.Process(c.eqMid, s.eqLow.Process(c.eqLow, toned)));
        return s.dcBlock.HighPass(c.dcBlock, toned) * c.trim;
    }
};
} // namespace distortion

/**
 * Distortion pedal with four classic circuits behind its Model switch. See distortion::Voicing
 * for what each one models.
 *
 * Drive, Tone and Level are the pedal's knobs (on the RAT, Tone is the Filter, bright
 * clockwise). Tight sets how much bass reaches the gain stage, noon being stock. Clipping
 * swaps the diodes to ground: silicon, asymmetric, LEDs (the RAT "Turbo"), germanium (the
 * "You Dirty Rat"), MOSFETs, or none. Low, Mid, Mid Freq and High follow the pedal and are
 * flat at 0 dB. Level is calibrated so the default drive is about as loud as bypass at the
 * nominal operating level.
 */
class DistortionEffect : public drive::DrivePedal<distortion::Traits>
{
};

inline void RegisterDistortionEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDistortion;
    info.aliases = {"distortion"};
    info.displayName = "Distortion";
    info.category = "drive";
    info.description = "Classic distortions: RAT, DS-1, Distortion+ and Metal Zone";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(distortion::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<DistortionEffect>(); });
}
} // namespace guitarfx
