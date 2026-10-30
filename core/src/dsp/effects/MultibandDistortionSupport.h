#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/LinkwitzRileyCrossover.h"
#include "dsp/effects/DriveStages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/**
 * The Multi-band Distortion's parts that are not the effect itself (MultibandDistortionEffect.h):
 * its parameter table, its clipping curves and the table that keeps Drive level-matched.
 */
namespace guitarfx::multiband_distortion
{
inline constexpr int kBands = 4;
inline constexpr int kSplits = kBands - 1;

using CrossoverDesign = lr_crossover::Design<kBands>;
using Crossover = lr_crossover::Crossover<kBands>;

/// Each band's clipper bends this far above the nominal operating level (an RMS level), so at
/// Drive 0 a normal note's peaks sit 9 dB under the knee and pass all but clean.
inline constexpr double kKneeAboveNominalDb = 12.0;

inline constexpr double kMaxDriveDb = 48.0;

/// Gains and levels are worked out every this many samples; the applied ones ramp between.
inline constexpr int kControlInterval = 16;

/// Drive, Level, Mix and Output glide over this, so a knob turn or a preset change does not
/// step the level mid-note.
inline constexpr double kSmoothingMs = 20.0;

/// How long a band takes to cross from one Character to the next.
inline constexpr double kCharacterFadeMs = 10.0;

/// The band meters' peak level falls back over this.
inline constexpr double kPeakReleaseMs = 300.0;

enum class Character
{
    Tape,
    Tube,
    Diode,
    Hard,
    Fuzz,
    Count
};

inline constexpr int kCharacterCount = static_cast<int>(Character::Count);
inline constexpr const char* kCharacterLabels[] = {"Tape", "Tube", "Diode", "Hard", "Fuzz"};
static_assert(std::size(kCharacterLabels) == static_cast<std::size_t>(kCharacterCount));

/// Each Character's curve, on signals scaled so the knee is at 1. All are clean with slope 1
/// for small signals; a lopsided one only adds even harmonics once it bends.
inline constexpr std::array<drive::ClipCurve, kCharacterCount> kCurves = {{
    {1.0, 1.0, drive::Knee::Gradual, drive::Knee::Gradual}, // Tape: the softest bend
    {1.0, 0.7, drive::Knee::Soft, drive::Knee::Soft},       // Tube: warm, even harmonics too
    {1.0, 1.0, drive::Knee::Soft, drive::Knee::Soft},       // Diode
    {1.0, 1.0, drive::Knee::Hard, drive::Knee::Hard},       // Hard: an op-amp's rails
    {1.0, 0.5, drive::Knee::Hard, drive::Knee::Hard},       // Fuzz: hard and lopsided
}};

enum Param : std::size_t
{
    kCrossover1,
    kCrossover2,
    kCrossover3,
    kDrive1,
    kCharacter1,
    kLevel1,
    kDrive2,
    kCharacter2,
    kLevel2,
    kDrive3,
    kCharacter3,
    kLevel3,
    kDrive4,
    kCharacter4,
    kLevel4,
    kMix,
    kOutput,
    kParamCount,
};

/// Band `band`'s drive, character and level: three params a band, in that order.
[[nodiscard]] constexpr Param BandParam(int band, int offset) noexcept
{
    return static_cast<Param>(kDrive1 + band * 3 + offset);
}

inline constexpr double kDefaultCharacter = static_cast<double>(Character::Tube);

// clang-format off
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    // The Multi-band Compressor's crossovers, ranges and defaults, so the two line up.
    LogTaper({"crossover1", "Low / Low Mid", 200.0, 40.0, 1000.0, "Hz", "Crossovers"}),
    LogTaper({"crossover2", "Low Mid / High Mid", 800.0, 150.0, 5000.0, "Hz", "Crossovers"}),
    LogTaper({"crossover3", "High Mid / High", 3000.0, 600.0, 12000.0, "Hz", "Crossovers"}),
    {"drive1", "Drive", 18.0, 0.0, kMaxDriveDb, "dB", "Low"},
    {"character1", "Character", kDefaultCharacter, 0.0, 4.0, "enum", "Low", false, 1.0, kCharacterLabels},
    {"level1", "Level", 0.0, -24.0, 12.0, "dB", "Low"},
    {"drive2", "Drive", 28.0, 0.0, kMaxDriveDb, "dB", "Low Mid"},
    {"character2", "Character", kDefaultCharacter, 0.0, 4.0, "enum", "Low Mid", false, 1.0, kCharacterLabels},
    {"level2", "Level", 0.0, -24.0, 12.0, "dB", "Low Mid"},
    {"drive3", "Drive", 36.0, 0.0, kMaxDriveDb, "dB", "High Mid"},
    {"character3", "Character", kDefaultCharacter, 0.0, 4.0, "enum", "High Mid", false, 1.0, kCharacterLabels},
    {"level3", "Level", 0.0, -24.0, 12.0, "dB", "High Mid"},
    {"drive4", "Drive", 30.0, 0.0, kMaxDriveDb, "dB", "High"},
    {"character4", "Character", kDefaultCharacter, 0.0, 4.0, "enum", "High", false, 1.0, kCharacterLabels},
    {"level4", "Level", 0.0, -24.0, 12.0, "dB", "High"},
    // Each band's driven signal against the same band clean, so the two share the crossover's
    // and the oversampler's phase and a part-way Mix does not comb.
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", ""},
    {"output", "Output", 0.0, -18.0, 18.0, "dB", ""},
}};
// clang-format on

/**
 * How much each Character loses of a tone at the nominal level, per dB of Drive: the RMS of a
 * sine through the curve, set against the sine's own, with the DC a lopsided curve makes left
 * out (the effect blocks it). Multiplying the clipper's output by this keeps a nominal note as
 * loud as it is at Drive 0 whatever the Drive, so Drive changes how dirty a band is, not how
 * loud.
 */
class LevelMatch
{
  public:
    static constexpr int kPoints = static_cast<int>(kMaxDriveDb) + 1; ///< one a dB

    LevelMatch()
    {
        constexpr int kSamples = 256;
        constexpr double kKneeOverNominal = 3.9810717055349722; // kKneeAboveNominalDb as a ratio

        for (int character = 0; character < kCharacterCount; ++character)
        {
            for (int point = 0; point < kPoints; ++point)
            {
                // A sine whose RMS is the nominal level, in the curve's units, driven.
                const double peak = std::sqrt(2.0) / kKneeOverNominal * std::pow(10.0, point / 20.0);
                double sum = 0.0;
                double sumSquares = 0.0;

                for (int i = 0; i < kSamples; ++i)
                {
                    const double shaped = kCurves[static_cast<std::size_t>(character)].Apply(
                        peak * std::sin(2.0 * drive::kPi * i / kSamples));
                    sum += shaped;
                    sumSquares += shaped * shaped;
                }

                const double mean = sum / kSamples;
                const double rms = std::sqrt(std::max(sumSquares / kSamples - mean * mean, 1.0e-30));
                mGain[static_cast<std::size_t>(character)][static_cast<std::size_t>(point)] =
                    (peak / std::sqrt(2.0)) / rms;
            }

            // Relative to Drive 0, so a band at 0 dB passes a quiet signal at unity, and its
            // whole Drive range stays as loud as it is there.
            auto& table = mGain[static_cast<std::size_t>(character)];
            const double atZero = table[0];

            for (auto& gain : table)
            {
                gain /= atZero;
            }
        }
    }

    /// The make-up gain for `character` at `driveDb`, interpolated between whole dB.
    [[nodiscard]] double Gain(int character, double driveDb) const noexcept
    {
        const auto& table = mGain[static_cast<std::size_t>(std::clamp(character, 0, kCharacterCount - 1))];
        const double position = std::clamp(driveDb, 0.0, kMaxDriveDb);
        const auto index = std::min(static_cast<std::size_t>(position), static_cast<std::size_t>(kPoints - 2));
        const double fraction = position - static_cast<double>(index);
        return table[index] + fraction * (table[index + 1] - table[index]);
    }

  private:
    std::array<std::array<double, kPoints>, kCharacterCount> mGain{};
};

/// Built once, the first time an effect is made (on the message thread).
[[nodiscard]] inline const LevelMatch& SharedLevelMatch()
{
    static const LevelMatch table;
    return table;
}
} // namespace guitarfx::multiband_distortion
