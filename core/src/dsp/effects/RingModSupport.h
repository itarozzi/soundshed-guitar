#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/TempoSync.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

/**
 * What the ring modulator is, as opposed to how it runs (RingModEffect.h): its constants, its
 * parameter table, the band-limited carrier and the LFO shapes, and the factory presets.
 */
namespace guitarfx::ring_mod
{
constexpr double kPi = 3.14159265358979323846;

/// The carrier is held inside this range whatever the knob and the LFO ask for. Past a quarter
/// of the sample rate the band-limiting corrections at a square's two edges would overlap, and
/// above 8 kHz a guitar's sidebands are only hash.
constexpr double kMinCarrierHz = 0.1;
constexpr double kMaxCarrierHz = 8000.0;
constexpr double kMaxCarrierFraction = 0.25;

/// The LFO and the carrier frequency are worked out once per this many samples, and the
/// carrier's phase increment ramps linearly in between. Even at the LFO's 20 Hz maximum that
/// is 150 updates per LFO cycle at 48 kHz.
constexpr int kControlInterval = 16;

/// Frequency glides in the log domain, so a MIDI CC's 1/127 steps do not zipper the sidebands.
constexpr double kFrequencyGlideMs = 20.0;
/// Smoothing for LFO Depth, Tone, Stereo Spread, Level and Mix.
constexpr double kControlSmoothingMs = 10.0;
/// Rounds the corners of the Square and Random LFO shapes, so the carrier slides to its next
/// pitch in about a millisecond instead of stepping.
constexpr double kLfoSlewMs = 1.0;
/// A Waveform change crossfades from the old carrier to the new over this long.
constexpr double kWaveformFadeMs = 10.0;
/// Once Stereo Spread is back at zero, the right carrier's phase is pulled onto the left's with
/// this time constant: a brief detune of at most 10 Hz at 48 kHz rather than a jump.
constexpr double kPhaseLockMs = 50.0;
/// Within this many cycles of each other, the two phases are simply made equal.
constexpr double kPhaseLockSnap = 1.0e-7;

/// A DC offset on the input would otherwise come out as a tone at the carrier frequency.
constexpr double kDcBlockHz = 10.0;
/// Tone sweeps the output low-pass exponentially between these. The top is clamped to 0.49 of
/// the sample rate, which leaves the audio band flat at 44.1 kHz and above.
constexpr double kToneMinHz = 400.0;
constexpr double kToneMaxHz = 24000.0;
constexpr double kMaxToneFraction = 0.49;
constexpr float kButterworthDamping = 1.41421356f; ///< 1/Q

/// At full Stereo Spread the right channel's LFO runs half a cycle ahead of the left's, and its
/// carrier a quarter of a cycle ahead.
constexpr double kSpreadLfoOffset = 0.5;
constexpr double kSpreadCarrierOffset = 0.25;

/// Each carrier is scaled to an RMS of 1, so the ring-modulated signal has the input's RMS
/// whichever waveform is chosen. The price is crest factor: against the input, a sine carrier's
/// output can peak 3 dB higher and a triangle's 4.8 dB.
constexpr float kSineGain = 1.41421356f;
constexpr float kTriangleGain = 1.73205081f;
constexpr float kSquareGain = 1.0f;

enum Param : std::size_t
{
    kFrequency,
    kWaveform,
    kLfoDepth,
    kLfoRate,
    kLfoShape,
    kSyncMode,
    kSyncDivision,
    kTone,
    kSpread,
    kLevel,
    kMix,
    kParamCount
};

enum class Waveform : int
{
    Sine,
    Triangle,
    Square
};

enum class LfoShape : int
{
    Sine,
    Triangle,
    Square,
    Random
};

inline constexpr const char* kWaveformLabels[] = {"Sine", "Triangle", "Square"};
inline constexpr const char* kLfoShapeLabels[] = {"Sine", "Triangle", "Square", "Random"};

/// In `Param` order, which is also the order the UI lays the controls out in.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"frequency", "Frequency", 440.0, 1.0, 2000.0, "Hz", "Carrier", false, 0.0, {}},
    {"waveform", "Waveform", 0.0, 0.0, 2.0, "enum", "Carrier", false, 1.0, kWaveformLabels},
    {"lfoDepth", "LFO Depth", 0.0, 0.0, 3.0, "oct", "LFO", false, 0.0, {}},
    {"lfoRate", "LFO Rate", 1.0, 0.05, 20.0, "Hz", "LFO", false, 0.0, {}},
    {"lfoShape", "LFO Shape", 0.0, 0.0, 3.0, "enum", "LFO", false, 1.0, kLfoShapeLabels},
    {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "LFO", false, 1.0, tempo_sync::kSyncModeLabelNames},
    {"syncDivision", "Division", 4.0, 0.0, 14.0, "enum", "LFO", false, 1.0, tempo_sync::kDivisionLabelNames},
    {"tone", "Tone", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0, {}},
    {"spread", "Stereo Spread", 0.0, 0.0, 1.0, "amount", "Output", true, 0.0, {}},
    {"level", "Level", 0.0, -12.0, 12.0, "dB", "Output", false, 0.0, {}},
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.0, {}},
}};

static_assert(kParams[kWaveform].maxValue == static_cast<double>(std::size(kWaveformLabels) - 1));
static_assert(kParams[kLfoShape].maxValue == static_cast<double>(std::size(kLfoShapeLabels) - 1));
static_assert(kParams[kSyncDivision].maxValue == static_cast<double>(std::size(tempo_sync::kDivisionLabelNames) - 1));

/// Every parameter's value, indexed by `Param`, always normalised (NormaliseParamValue).
using ParamValues = std::array<double, kParamCount>;

inline constexpr ParamValues kDefaultValues = DefaultParamValues(kParams);

/// `phase` wrapped back into [0, 1) after a step of less than a cycle either way.
[[nodiscard]] inline double WrapPhase(double phase)
{
    if (phase >= 1.0)
    {
        return phase - 1.0;
    }

    return phase < 0.0 ? phase + 1.0 : phase;
}

/// Whether the waveform feature at phase `edge` lies within one sample of `phase`, and if so how
/// far away, in samples: in [0, 1) just after it, in (-1, 0) just before.
[[nodiscard]] inline bool NearEdge(double phase, double edge, double dt, double& distance)
{
    const double t = WrapPhase(phase - edge);

    if (t < dt)
    {
        distance = t / dt;
        return true;
    }

    if (t > 1.0 - dt)
    {
        distance = (t - 1.0) / dt;
        return true;
    }

    return false;
}

/// What a unit step needs added to become band-limited: the two-sample polynomial BLEP, which is
/// the step integrated from a triangular kernel one sample either side of it.
[[nodiscard]] inline double BlepResidual(double distance)
{
    return distance < 0.0 ? 0.5 * (1.0 + distance) * (1.0 + distance) : -0.5 * (1.0 - distance) * (1.0 - distance);
}

/// The same for a unit change of slope per sample (BLAMP): the BLEP residual integrated again.
[[nodiscard]] inline double BlampResidual(double distance)
{
    const double u = 1.0 - std::abs(distance);
    return u * u * u / 6.0;
}

/// One carrier sample at `phase`, moving `dt` cycles per sample, scaled to an RMS of 1. The
/// triangle and square are aligned with the sine, so a crossfade between them never cancels.
/// Their corners and edges are band-limited, so a high carrier does not fold its harmonics back
/// down as inharmonic tones.
[[nodiscard]] inline float CarrierSample(Waveform waveform, double phase, double dt)
{
    double distance = 0.0;

    switch (waveform)
    {
    case Waveform::Triangle: {
        double value = phase < 0.25 ? 4.0 * phase : (phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0);

        // The slope turns from +4 to -4 per cycle at the peak, and back at the trough.
        if (NearEdge(phase, 0.25, dt, distance))
        {
            value -= 8.0 * dt * BlampResidual(distance);
        }

        if (NearEdge(phase, 0.75, dt, distance))
        {
            value += 8.0 * dt * BlampResidual(distance);
        }

        return static_cast<float>(value) * kTriangleGain;
    }
    case Waveform::Square: {
        double value = phase < 0.5 ? 1.0 : -1.0;

        if (NearEdge(phase, 0.0, dt, distance))
        {
            value += 2.0 * BlepResidual(distance);
        }

        if (NearEdge(phase, 0.5, dt, distance))
        {
            value -= 2.0 * BlepResidual(distance);
        }

        return static_cast<float>(value) * kSquareGain;
    }
    case Waveform::Sine:
    default:
        return std::sin(static_cast<float>(2.0 * kPi * phase)) * kSineGain;
    }
}

/// Spreads a cycle number across 32 bits (Wellons' lowbias32), for the Random LFO's steps.
[[nodiscard]] inline std::uint32_t HashCycle(std::uint32_t cycle)
{
    std::uint32_t x = cycle * 0x9e3779b9u + 0x7f4a7c15u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// The LFO at `phase`, in [-1, 1]. Random holds one value per cycle, picked by the cycle's
/// number, so the right channel's offset LFO steps through the same values as the left's.
[[nodiscard]] inline double LfoValue(LfoShape shape, double phase, std::uint32_t cycle)
{
    switch (shape)
    {
    case LfoShape::Triangle:
        return phase < 0.25 ? 4.0 * phase : (phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0);
    case LfoShape::Square:
        return phase < 0.5 ? 1.0 : -1.0;
    case LfoShape::Random:
        return static_cast<double>(HashCycle(cycle) >> 8) * (2.0 / 16777216.0) - 1.0;
    case LfoShape::Sine:
    default:
        return std::sin(2.0 * kPi * phase);
    }
}

[[nodiscard]] inline double ToneCutoffHz(double tone)
{
    return kToneMinHz * std::pow(kToneMaxHz / kToneMinHz, tone);
}

[[nodiscard]] inline float FlushDenormal(float value)
{
    return (std::fabs(value) < 1.0e-25f) ? 0.0f : value;
}

/**
 * How a factory preset sets up the effect. A preset is a whole sound, so it sets every
 * parameter except the tempo division, and turns Sync off because each names its own LFO rate.
 * The first is the defaults, which is also what a new node starts with.
 */
struct Voicing
{
    const char* id;
    const char* displayName;
    double frequency;
    Waveform waveform;
    double lfoDepth;
    double lfoRate;
    LfoShape lfoShape;
    double tone;
    double spread;
    double mix;
};

constexpr const char* kDefaultPresetId = "classic-ring";

// clang-format off
inline constexpr std::array<Voicing, 6> kFactoryVoicings = {{
    //  id                  display name        freq   waveform            depth rate  LFO shape           tone  spread mix
    {kDefaultPresetId,      "Classic Ring",     440.0, Waveform::Sine,     0.0,  1.0,  LfoShape::Sine,     1.0,  0.0,   1.0},
    {"robot-voice",         "Robot Voice",      30.0,  Waveform::Sine,     0.0,  1.0,  LfoShape::Sine,     0.85, 0.0,   1.0},
    {"bell-tones",          "Bell Tones",       740.0, Waveform::Sine,     0.0,  1.0,  LfoShape::Sine,     0.8,  0.0,   0.7},
    {"sci-fi-sweep",        "Sci-Fi Sweep",     500.0, Waveform::Sine,     1.5,  0.25, LfoShape::Sine,     0.9,  0.0,   1.0},
    {"computer-chatter",    "Computer Chatter", 900.0, Waveform::Square,   1.0,  8.0,  LfoShape::Random,   0.6,  0.0,   0.9},
    {"stereo-warble",       "Stereo Warble",    280.0, Waveform::Triangle, 0.3,  3.0,  LfoShape::Triangle, 0.9,  1.0,   0.8},
}};
// clang-format on

[[nodiscard]] inline EffectPresetDefinition MakePreset(const Voicing& voicing)
{
    EffectPresetDefinition preset;
    preset.id = voicing.id;
    preset.displayName = voicing.displayName;
    preset.isFactory = true;
    preset.isDefault = (preset.id == kDefaultPresetId);
    preset.parameters = {{"frequency", voicing.frequency},
                         {"waveform", static_cast<double>(voicing.waveform)},
                         {"lfoDepth", voicing.lfoDepth},
                         {"lfoRate", voicing.lfoRate},
                         {"lfoShape", static_cast<double>(voicing.lfoShape)},
                         {"syncMode", 0.0},
                         {"tone", voicing.tone},
                         {"spread", voicing.spread},
                         {"level", 0.0},
                         {"mix", voicing.mix}};
    preset.parameterOrder = {"frequency", "waveform", "lfoDepth", "lfoRate", "lfoShape",
                             "syncMode",  "tone",     "spread",   "level",   "mix"};
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
} // namespace guitarfx::ring_mod
