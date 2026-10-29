#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectRegistry.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/FactoryPresetSupport.h"

#include <array>
#include <cstddef>
#include <vector>

/**
 * What the pitch-tracking EQ is, as opposed to how it runs (PitchTrackingEqEffect.h): its
 * constants, its parameter table and its factory presets.
 */
namespace guitarfx::pitch_eq
{
constexpr int kBandCount = 3;

/// The bands' frequencies follow the tracked pitch once per this many samples, and their
/// coefficients ramp linearly in between.
constexpr int kControlInterval = 32;

/// Smoothing for the knobs (gain, harmonic, width, cutoff) once audio is running.
constexpr double kControlSmoothingMs = 10.0;
/// The EQ fades in over this long when the first note is found, rather than switching on.
constexpr double kEngageMs = 15.0;

/// A band or the low-pass is never placed below this or above kMaxFraction of the sample rate.
constexpr double kMinBandHz = 20.0;
constexpr double kMaxFraction = 0.45;
/// A band's gain fades to nothing over the last half-octave below that ceiling, so a high
/// harmonic of a high note slides out instead of piling up at the top.
constexpr double kTopFadeOctaves = 0.5;
/// The Q a band's width works out to is held inside this range.
constexpr double kMinQ = 0.2;
constexpr double kMaxQ = 80.0;

enum Param : std::size_t
{
    kHarmonic1,
    kGain1,
    kWidth1,
    kHarmonic2,
    kGain2,
    kWidth2,
    kHarmonic3,
    kGain3,
    kWidth3,
    kLowPass,
    kCutoff,
    kGlide,
    kLowestNote,
    kLevel,
    kParamCount
};

/// The parameters of band `band` (0-based) are these offsets from kHarmonic1.
constexpr std::size_t kBandStride = kHarmonic2 - kHarmonic1;
constexpr std::size_t kHarmonicOffset = 0;
constexpr std::size_t kGainOffset = kGain1 - kHarmonic1;
constexpr std::size_t kWidthOffset = kWidth1 - kHarmonic1;

inline constexpr const char* kLowPassLabels[] = {"Off", "On"};
inline constexpr int kLowPassOn[] = {1};

// clang-format off
/// In `Param` order, which is also the order the UI lays the controls out in.
///
/// Harmonic places a band at that multiple of the note's fundamental: 1 is the fundamental, 2 its
/// octave, 3 the octave and a fifth, 0.5 an octave below. Width is the band's width in harmonics
/// (the spacing between two neighbouring ones, which is the fundamental), so a width under 1
/// touches one harmonic whichever note is played.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"harmonic1", "Harmonic", 2.0, 0.5, 16.0, "x", "Band 1", false, 0.5, {}},
    {"gain1", "Gain", 4.0, -18.0, 18.0, "dB", "Band 1", false, 0.0, {}},
    LogTaper({"width1", "Width", 0.5, 0.1, 2.0, "harmonics", "Band 1", false, 0.0, {}}),
    {"harmonic2", "Harmonic", 4.0, 0.5, 16.0, "x", "Band 2", false, 0.5, {}},
    {"gain2", "Gain", 3.0, -18.0, 18.0, "dB", "Band 2", false, 0.0, {}},
    LogTaper({"width2", "Width", 0.5, 0.1, 2.0, "harmonics", "Band 2", false, 0.0, {}}),
    {"harmonic3", "Harmonic", 6.0, 0.5, 16.0, "x", "Band 3", false, 0.5, {}},
    {"gain3", "Gain", 2.0, -18.0, 18.0, "dB", "Band 3", false, 0.0, {}},
    LogTaper({"width3", "Width", 0.5, 0.1, 2.0, "harmonics", "Band 3", false, 0.0, {}}),
    {"lowPass", "Low-pass", 0.0, 0.0, 1.0, "enum", "Low-pass", false, 1.0, kLowPassLabels},
    LogTaper({"cutoff", "Cutoff", 8.0, 1.0, 32.0, "x", "Low-pass", false, 0.0, {}, {}, "lowPass", kLowPassOn}),
    {"glide", "Glide", 20.0, 0.0, 200.0, "ms", "Tracking", true, 0.0, {}},
    {"lowestNote", "Lowest Note", 0.0, 0.0, 3.0, "enum", "Tracking", true, 1.0, pitch_tracker::kLowestNoteLabels},
    {"level", "Level", 0.0, -12.0, 12.0, "dB", "Output", false, 0.0, {}},
}};
// clang-format on

using ParamValues = std::array<double, kParamCount>;

inline constexpr ParamValues kDefaultValues = DefaultParamValues(kParams);

/// The factory presets, each from the defaults (the first, Even Warmth) with its own changes.
/// Level and Lowest Note are the player's, so no preset sets them.
[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {"level", "lowestNote"});
    constexpr double kOn = 1.0;

    return {
        // The even harmonics up: the octave, two octaves and two octaves and a fifth.
        b.Defaults("even-warmth", "Even Warmth"),
        // The odd ones up instead, for a reedier, more forward tone.
        b.Make("odd-bite", "Odd Bite", {{"harmonic1", 3.0}, {"gain1", 5.0}, {"harmonic2", 5.0}, {"harmonic3", 7.0}}),
        // The even harmonics cut, which leaves a hollow, clarinet-like tone.
        b.Make("hollow", "Hollow",
               {{"gain1", -9.0}, {"width1", 0.4}, {"gain2", -7.0}, {"width2", 0.4}, {"gain3", -5.0}, {"width3", 0.4}}),
        // The fundamental lifted and the upper harmonics eased back, for a lead that sings.
        b.Make("singing-fundamental", "Singing Fundamental",
               {{"harmonic1", 1.0},
                {"gain1", 6.0},
                {"width1", 0.4},
                {"harmonic2", 2.0},
                {"gain2", 2.0},
                {"harmonic3", 8.0},
                {"gain3", -3.0},
                {"width3", 1.0}}),
        // High harmonics notched and a key-tracked low-pass above them, for a fizzy drive.
        b.Make("tame-fizz", "Tame Fizz",
               {{"harmonic1", 7.0},
                {"gain1", -5.0},
                {"width1", 0.6},
                {"harmonic2", 9.0},
                {"gain2", -5.0},
                {"width2", 0.6},
                {"harmonic3", 11.0},
                {"gain3", -5.0},
                {"width3", 0.6},
                {"lowPass", kOn},
                {"cutoff", 12.0}}),
        // Only the key-tracked low-pass: the same brightness, relative to the note, up the neck.
        b.Make("even-brightness", "Even Brightness",
               {{"gain1", 0.0}, {"gain2", 0.0}, {"gain3", 0.0}, {"lowPass", kOn}, {"cutoff", 6.0}}),
    };
}
} // namespace guitarfx::pitch_eq
