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
namespace fuzz
{
enum Param : std::size_t
{
    kModel,
    kDrive,
    kTone,
    kBias,
    kBass,
    kLevel,
    kMix,
    kParamCount
};

/// Index order is stored in presets: append new models, never reorder.
enum class Model
{
    FuzzFace,
    BigMuff,
    ToneBender,
    FuzzTone,
    SuperFuzz,
    Count
};

inline constexpr const char* kModelLabels[] = {"Fuzz Face", "Big Muff", "Tone Bender", "Fuzz-Tone", "Super-Fuzz"};
static_assert(std::size(kModelLabels) == static_cast<std::size_t>(Model::Count));

inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"model", "Model", 0.0, 0.0, 4.0, "enum", "Pedal", false, 1.0, kModelLabels},
    {"drive", "Fuzz", 0.7, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"tone", "Tone", 0.5, 0.0, 1.0, "amount", "Pedal", false, 0.0},
    {"bias", "Bias", 0.5, 0.0, 1.0, "amount", "Voicing", false, 0.0},
    {"bass", "Bass", 0.5, 0.0, 1.0, "amount", "Voicing", false, 0.0},
    {"level", "Level", 0.0, -24.0, 24.0, "dB", "Output", false, 0.0},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0},
}};

inline constexpr std::size_t kMaxStages = 3;

/// One gain stage: a coupling capacitor, gain, a transistor or diode curve around its bias
/// point, and the stage's own high-frequency roll-off. A frequency of 0 leaves a filter out.
struct StageVoicing
{
    double couplingHz;
    double fixedGainDb;
    bool gainFromDrive; ///< this stage's gain is the Fuzz knob, fixedGainDb its minimum
    double driveRangeDb;
    drive::ClipCurve curve;
    double bias; ///< stock operating point, in volts from centre
    double lowPassHz;
};

enum class ToneStyle
{
    Tilt,  ///< flat at noon: the pedals that had no tone control
    Blend, ///< the Big Muff's low-pass/high-pass blend, scooped at noon
    Scoop  ///< the Super-Fuzz's mid cut, from none to full
};

/**
 * - Fuzz Face: two germanium transistors, direct-coupled with shunt feedback. Round on one
 *   side and clipped on the other, so strong even harmonics; full bass, and the low input
 *   impedance loads the pickup, softening its top. Biased right, it cleans up as the signal
 *   falls, the way it does on the guitar's volume knob.
 * - Big Muff: an input booster, then two clipping stages with diodes in their feedback,
 *   each high-passed by its coupling capacitor and rolled off by its Miller capacitance, then
 *   the famous tone stack: 39k/10 nF low-pass (408 Hz) blended with 4 nF/22k high-pass. At
 *   noon it scoops the mids by about 9 dB. Sustain rather than splatter.
 * - Tone Bender MkII: a third germanium stage ahead of a Fuzz-Face-style pair: more gain,
 *   smaller coupling capacitors for a tighter low end, more compression.
 * - Fuzz-Tone: three germanium transistors on a 1.5 V supply, biased close to cutoff. Only
 *   the peaks get through, gated and buzzy, with a brassy mid lift and little bass.
 * - Super-Fuzz: a preamp, then a full-wave rectifier (the octave-up), then a hard-clipping
 *   stage. Its Tone switch cuts the mids; here Tone sets how much, from none to full.
 *
 * The drive stage is marked by `biasStage`; the Bias knob moves its operating point
 * `biasRange` volts either way from stock. Toward one knee, small signals stop passing:
 * starved, gated and sputtery, like a dying battery. Toward the other, the stage runs hot
 * and compressed. Bass moves the input's low cut two octaves either way.
 */
struct Voicing
{
    std::array<StageVoicing, kMaxStages> stages;
    std::size_t stageCount;
    std::size_t biasStage;
    double biasRange;
    double inputHighPassHz;
    double pickupLoadHz; ///< 0: none
    double octave;       ///< fraction of the rectified octave-up after the first stage
    ToneStyle tone;
    double toneLowHz;
    double toneHighHz;
    double peakHz; ///< fixed post bell (the Fuzz-Tone's brass); 0: none
    double peakDb;
    double dcBlockHz;
    drive::TrimTable trimDb;
};

// clang-format off
// *INDENT-OFF*
inline constexpr std::array<Voicing, static_cast<std::size_t>(Model::Count)> kVoicings = {{
    // Fuzz Face
    {{{{5.0,   7.0,  true,  16.0, {1.5, 1.5, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   0.0},
       {5.0,   7.0,  true,  16.0, {1.0, 0.7, drive::Knee::Gradual, drive::Knee::Hard}, 0.0,   9000.0},
       {0.0,   0.0,  false, 0.0,  {1.0, 1.0, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   0.0}}},
     2, 1, 1.1, 40.0, 6000.0, 0.0, ToneStyle::Tilt, 800.0, 800.0, 0.0, 0.0, 30.0, {-5.6, -6.8, -7.7, -8.4, -8.8, -9.2, -9.4, -9.6, -9.7}},
    // Big Muff
    {{{{30.0,  -12.0, true, 36.0, {2.5, 2.5, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   0.0},
       {150.0, 24.0, false, 0.0,  {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   5000.0},
       {150.0, 24.0, false, 0.0,  {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   5000.0}}},
     3, 0, 2.0, 60.0, 0.0, 0.0, ToneStyle::Blend, 408.0, 1800.0, 0.0, 0.0, 20.0, {3.8, 3.6, 3.5, 3.4, 3.4, 3.4, 3.4, 3.4, 3.4}},
    // Tone Bender MkII
    {{{{10.0,  20.0, false, 0.0,  {1.2, 0.9, drive::Knee::Gradual, drive::Knee::Soft}, -0.1,  0.0},
       {80.0,  6.0,  true,  28.0, {0.9, 0.6, drive::Knee::Gradual, drive::Knee::Hard}, 0.0,   7000.0},
       {0.0,   0.0,  false, 0.0,  {1.0, 1.0, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   0.0}}},
     2, 1, 0.9, 100.0, 8000.0, 0.0, ToneStyle::Tilt, 800.0, 800.0, 0.0, 0.0, 30.0, {-6.8, -7.3, -7.7, -8.0, -8.2, -8.3, -8.4, -8.5, -8.5}},
    // Fuzz-Tone
    {{{{5.0,   10.0, true,  24.0, {0.5, 0.35, drive::Knee::Soft, drive::Knee::Hard}, -0.4, 10000.0},
       {200.0, 24.0, false, 0.0,  {0.5, 0.35, drive::Knee::Soft, drive::Knee::Hard}, -0.4, 6000.0},
       {0.0,   0.0,  false, 0.0,  {1.0, 1.0, drive::Knee::Soft, drive::Knee::Soft},  0.0,  0.0}}},
     2, 0, 0.35, 250.0, 0.0, 0.0, ToneStyle::Tilt, 1000.0, 1000.0, 1300.0, 7.0, 60.0, {-6.4, -6.6, -6.8, -6.9, -7.0, -7.0, -7.1, -7.1, -7.1}},
    // Super-Fuzz
    {{{{10.0,  14.0, true,  26.0, {1.0, 1.0, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   7000.0},
       {60.0,  20.0, false, 0.0,  {0.6, 0.6, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   6000.0},
       {0.0,   0.0,  false, 0.0,  {1.0, 1.0, drive::Knee::Soft, drive::Knee::Soft}, 0.0,   0.0}}},
     2, 0, 0.3, 80.0, 0.0, 0.75, ToneStyle::Scoop, 800.0, 800.0, 0.0, 0.0, 20.0, {-1.7, -1.7, -1.7, -1.8, -1.8, -1.9, -2.0, -2.0, -2.0}},
}};
// *INDENT-ON*
// clang-format on

/// Softens the rectifier's corner at zero, as the germanium diodes' own knee does, so the
/// octave does not spray harmonics past what the oversampling can hold.
inline constexpr double kRectifierKneeVolts = 0.05;

/// Deepest the Super-Fuzz scoop goes, at Tone fully up.
inline constexpr double kScoopDepthDb = 24.0;

struct Traits
{
    static constexpr const char* kTypeName = "fuzz";
    static constexpr const auto& kParams = fuzz::kParams;
    static constexpr std::size_t kLevel = fuzz::kLevel;
    static constexpr std::size_t kMix = fuzz::kMix;

    struct StageCoefficients
    {
        double coupling = 0.0;
        bool hasCoupling = false;
        double gain = 1.0;
        drive::ClipCurve curve;
        double bias = 0.0;
        double lowPass = 1.0;
        bool hasLowPass = false;
    };

    struct Coefficients
    {
        double inputHighPass = 0.0;
        double pickupLoad = 1.0;
        bool hasPickupLoad = false;
        std::array<StageCoefficients, kMaxStages> stages;
        std::size_t stageCount = 1;
        double octave = 0.0;
        double toneLow = 0.0;
        double toneLowGain = 1.0;
        double toneHigh = 0.0;
        double toneHighGain = 0.0;
        BiquadCoefficients peak;
        double dcBlock = 0.0;
        double trim = 1.0;
    };

    struct State
    {
        drive::OnePole input;
        drive::OnePole pickupLoad;
        std::array<drive::OnePole, kMaxStages> coupling;
        std::array<drive::OnePole, kMaxStages> lowPass;
        drive::OnePole toneLow;
        drive::OnePole toneHigh;
        biquad::State peak;
        drive::OnePole dcBlock;
        std::array<drive::AntialiasMemory, kMaxStages> curve;
        drive::AntialiasMemory rectifier;

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

        c.inputHighPass = drive::OnePoleCoefficient(v.inputHighPassHz * drive::KnobOctaves(values[kBass], -2.0), rate);
        c.hasPickupLoad = v.pickupLoadHz > 0.0;
        c.pickupLoad = drive::OnePoleCoefficient(c.hasPickupLoad ? v.pickupLoadHz : rate, rate);
        c.stageCount = v.stageCount;
        c.octave = v.octave;

        for (std::size_t index = 0; index < kMaxStages; ++index)
        {
            const StageVoicing& stage = v.stages[index];
            StageCoefficients& out = c.stages[index];
            out.hasCoupling = stage.couplingHz > 0.0;
            out.coupling = drive::OnePoleCoefficient(out.hasCoupling ? stage.couplingHz : osRate, osRate);
            out.gain = drive::DbToGain(stage.fixedGainDb + (stage.gainFromDrive ? stage.driveRangeDb * amount : 0.0));
            out.curve = stage.curve;
            out.bias = stage.bias + (index == v.biasStage ? (values[kBias] - 0.5) * 2.0 * v.biasRange : 0.0);
            out.hasLowPass = stage.lowPassHz > 0.0;
            out.lowPass = drive::OnePoleCoefficient(out.hasLowPass ? stage.lowPassHz : osRate, osRate);
        }

        c.peak = v.peakHz > 0.0 ? biquad::Peaking(v.peakHz, 1.2, v.peakDb, rate) : BiquadCoefficients{};

        switch (v.tone)
        {
        case ToneStyle::Tilt:
            c.toneLow = drive::OnePoleCoefficient(v.toneLowHz, rate);
            c.toneLowGain = drive::DbToGain(-(tone - 0.5) * 12.0);
            c.toneHigh = c.toneLow;
            c.toneHighGain = drive::DbToGain((tone - 0.5) * 12.0);
            break;

        case ToneStyle::Blend:
            c.toneLow = drive::OnePoleCoefficient(v.toneLowHz, rate);
            c.toneLowGain = 1.0 - tone;
            c.toneHigh = drive::OnePoleCoefficient(v.toneHighHz, rate);
            c.toneHighGain = tone;
            break;

        case ToneStyle::Scoop:
            c.toneLow = drive::OnePoleCoefficient(v.toneLowHz, rate);
            c.toneLowGain = 1.0;
            c.toneHigh = c.toneLow;
            c.toneHighGain = 1.0;
            c.peak = biquad::Peaking(v.toneLowHz, 0.7, -kScoopDepthDb * tone, rate);
            break;
        }

        c.dcBlock = drive::OnePoleCoefficient(v.dcBlockHz, rate);
        c.trim = drive::DbToGain(drive::InterpolateTrimDb(v.trimDb, amount));
    }

    static double Pre(const Coefficients& c, State& s, double volts) noexcept
    {
        const double x = s.input.HighPass(c.inputHighPass, volts);
        return c.hasPickupLoad ? s.pickupLoad.LowPass(c.pickupLoad, x) : x;
    }

    static double Shape(const Coefficients& c, State& s, double x) noexcept
    {
        for (std::size_t index = 0; index < c.stageCount; ++index)
        {
            const StageCoefficients& stage = c.stages[index];

            if (stage.hasCoupling)
            {
                x = s.coupling[index].HighPass(stage.coupling, x);
            }

            x = stage.curve.Antialiased(x * stage.gain, s.curve[index], stage.bias);

            if (stage.hasLowPass)
            {
                x = s.lowPass[index].LowPass(stage.lowPass, x);
            }

            if (index == 0 && c.octave > 0.0)
            {
                x += (drive::RectifyAntialiased(x, kRectifierKneeVolts, s.rectifier) - x) * c.octave;
            }
        }

        return x;
    }

    static double Post(const Coefficients& c, State& s, double y) noexcept
    {
        const double low = s.toneLow.LowPass(c.toneLow, y);
        const double high = y - s.toneHigh.LowPass(c.toneHigh, y);
        const double toned = s.peak.Process(c.peak, low * c.toneLowGain + high * c.toneHighGain);
        return s.dcBlock.HighPass(c.dcBlock, toned) * c.trim;
    }
};
} // namespace fuzz

/**
 * Fuzz pedal with five classic circuits behind its Model switch. See fuzz::Voicing for what
 * each one models.
 *
 * Fuzz, Tone and Level are the pedal's knobs; on the pedals that had no tone control, Tone is
 * a tilt that is flat at noon. Bias moves the drive transistor's operating point (noon is
 * stock): down starves it into gated sputter, up runs it hot and compressed. Bass sets how
 * much low end reaches the circuit. The output is AC-coupled like the hardware's, so the
 * asymmetry that gives a fuzz its even harmonics never leaves the pedal as DC. Level is
 * calibrated so the default fuzz is about as loud as bypass at the nominal operating level.
 */
class FuzzEffect : public drive::DrivePedal<fuzz::Traits>
{
};

inline void RegisterFuzzEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kFuzz;
    info.aliases = {"fuzz"};
    info.displayName = "Fuzz";
    info.category = "drive";
    info.description = "Classic fuzzes: Fuzz Face, Big Muff, Tone Bender, Fuzz-Tone and Super-Fuzz";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(fuzz::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<FuzzEffect>(); });
}
} // namespace guitarfx
