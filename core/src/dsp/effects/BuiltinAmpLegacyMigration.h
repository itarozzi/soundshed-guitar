#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinAmpFilters.h"
#include "dsp/effects/BuiltinAmpVoicing.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iterator>
#include <map>
#include <string>

namespace guitarfx::amp_legacy
{
/**
 * Brings Heavy American nodes saved before the amp was rebuilt (1.5.0 and earlier) up to the amp
 * that replaced it, at the same loudness.
 *
 * The old amp had no level makeup. A guitar at the nominal level came out 12-18 dB above it at any
 * Gain past a quarter, so its presets set Output against that, often -15 to -24 dB. The new amp
 * holds the nominal level at Output 0 dB, and an old node would come out up to 21 dB quieter: on
 * its own, and into whatever follows it. So a stored amp node with no `character` key is given
 * Character's default explicitly and the Output that keeps it as loud as it was. Only an old preset
 * lacks that key: Character came with the rebuild, and every node written since carries it.
 *
 * That Output is the old one plus the difference between the two amps at the node's settings, as
 * heard (through a generic cab band and K-weighting) on the demo guitar at the nominal level, the
 * way the new amp's own makeup is measured:
 * - kOldLevelDb holds the old amp's heard gain by voice, Preamp Stages, Gain and Input Trim, with
 *   Power Drive and Sag at 0. The new amp holds the nominal level untrimmed, so its own is its
 *   kTrimGainDb, which shares the grid: the Gain steps are even on log(1 + 16 Gain), since the old
 *   amp's level rose 10 dB in the first sixteenth of its Gain.
 * - What the old Power Drive and Sag added comes from a sine through the old power stage (a tanh,
 *   the sag ahead of it holding its input down by its own peaks) standing in for the guitar there,
 *   peaking at kOldFeedDb + kOldFeedSlope x the old amp's heard gain. The new amp makes up both, at
 *   any Input Trim, so they need nothing more.
 * - The old voice blended its two channels; the new one snaps to the nearer. The old level is
 *   interpolated between the voices, the new one is the channel it snaps to.
 * - The tone stack's level reaches the two outputs differently: through the old amp's last
 *   stage, a tanh, and through the new one's later stages and makeup. A mid scoop on a driven
 *   amp cost the new one up to 2.5 dB more than the old. ToneStackDb is what the tone stack does
 *   to a flat signal, and kToneCutShare and kToneBoostShare how much more of that the new amp
 *   passes on, by voice, Preamp Stages and Gain.
 * - Presence, the speaker controls and Bias are left out: each moved the difference by under
 *   0.5 dB.
 *
 * `BuiltinAmpLegacyMigrationTests --calibrate` re-derives the tables and the fit, from the old amp
 * kept verbatim in core/tests/helpers/LegacyBuiltinAmp.h. Re-run it if the new amp's voicing or
 * makeup changes.
 */

/// Character's default, which the tables were measured at and a migrated node is given.
inline constexpr double kMigratedCharacter = builtin_amp::kParams[builtin_amp::kCharacter].defaultValue;

// The new amp's Input Trim grid (builtin_amp::kTrimGainDb), which the old amp's table shares.
inline constexpr std::size_t kGainSteps = builtin_amp::kTrimGainRows; ///< Gain 0 to 1, even on GainAxis
inline constexpr std::size_t kTrimSteps = builtin_amp::kTrimColumns;  ///< Input Trim -24 to +24 dB
inline constexpr int kStageCounts = builtin_amp::kMaxStages;
inline constexpr double kTrimMinDb = builtin_amp::kTrimMinDb;
inline constexpr double kTrimMaxDb = builtin_amp::kTrimMaxDb;
inline constexpr double kOutputMinDb = -24.0;
inline constexpr double kOutputMaxDb = 24.0;

[[nodiscard]] inline double GainAxis(double gain)
{
    return builtin_amp::TrimGainAxis(gain);
}

/// The Gain at table row `index`: 0, 0.027, 0.064, 0.118, 0.195, 0.30, 0.46, 0.68, 1.
[[nodiscard]] inline double GainAtRow(std::size_t index)
{
    return builtin_amp::TrimGainAtRow(index);
}

/// [voice][stages - 1][Gain row][Input Trim column] -> heard gain, dB.
using LevelGrid = float[2][kStageCounts][kGainSteps][kTrimSteps];

// clang-format off
// *INDENT-OFF*
inline constexpr LevelGrid kOldLevelDb = {
    { // Clean
        { // 1 stage
            {-25.75f, -19.76f, -13.78f, -7.86f, -2.16f, 2.96f, 7.03f, 9.89f, 11.81f}, // Gain 0.000
            {-23.38f, -17.39f, -11.43f, -5.57f, -0.04f, 4.71f, 8.30f, 10.74f, 12.39f}, // Gain 0.027
            {-20.49f, -14.51f, -8.58f, -2.84f, 2.38f, 6.60f, 9.60f, 11.62f, 12.98f}, // Gain 0.064
            {-17.07f, -11.11f, -5.26f, 0.24f, 4.94f, 8.46f, 10.85f, 12.46f, 13.54f}, // Gain 0.118
            {-13.13f, -7.23f, -1.57f, 3.46f, 7.40f, 10.14f, 11.98f, 13.22f, 14.03f}, // Gain 0.195
            {-8.77f, -3.02f, 2.22f, 6.48f, 9.52f, 11.56f, 12.94f, 13.85f, 14.40f}, // Gain 0.305
            {-4.12f, 1.26f, 5.74f, 9.02f, 11.22f, 12.71f, 13.71f, 14.32f, 14.65f}, // Gain 0.461
            {0.54f, 5.18f, 8.63f, 10.96f, 12.54f, 13.59f, 14.25f, 14.61f, 14.80f}, // Gain 0.683
            {4.78f, 8.35f, 10.77f, 12.41f, 13.51f, 14.20f, 14.59f, 14.79f, 14.88f} // Gain 1.000
        },
        { // 2 stages
            {-49.75f, -37.75f, -25.77f, -13.85f, -2.34f, 7.01f, 12.17f, 14.47f, 15.29f}, // Gain 0.000
            {-46.10f, -34.11f, -22.13f, -10.28f, 0.84f, 8.99f, 13.08f, 14.83f, 15.39f}, // Gain 0.027
            {-41.65f, -29.66f, -17.72f, -6.02f, 4.30f, 10.87f, 13.93f, 15.13f, 15.46f}, // Gain 0.064
            {-36.40f, -24.43f, -12.56f, -1.25f, 7.66f, 12.50f, 14.62f, 15.33f, 15.50f}, // Gain 0.118
            {-30.39f, -18.46f, -6.79f, 3.60f, 10.50f, 13.79f, 15.09f, 15.45f, 15.52f}, // Gain 0.195
            {-23.71f, -11.91f, -0.80f, 7.86f, 12.63f, 14.68f, 15.35f, 15.50f, 15.53f}, // Gain 0.305
            {-16.53f, -5.10f, 4.73f, 11.09f, 14.09f, 15.19f, 15.47f, 15.53f, 15.53f}, // Gain 0.461
            {-9.13f, 1.35f, 9.10f, 13.26f, 14.94f, 15.42f, 15.52f, 15.53f, 15.54f}, // Gain 0.683
            {-2.06f, 6.70f, 12.13f, 14.57f, 15.33f, 15.50f, 15.53f, 15.54f, 15.54f} // Gain 1.000
        },
        { // 3 stages
            {-73.75f, -55.75f, -37.77f, -19.84f, -2.46f, 10.30f, 15.10f, 16.14f, 16.25f}, // Gain 0.000
            {-68.81f, -50.82f, -32.85f, -14.98f, 1.78f, 12.20f, 15.60f, 16.20f, 16.25f}, // Gain 0.027
            {-62.81f, -44.83f, -26.88f, -9.18f, 6.19f, 13.85f, 15.95f, 16.24f, 16.26f}, // Gain 0.064
            {-55.74f, -37.77f, -19.89f, -2.66f, 10.10f, 15.07f, 16.14f, 16.25f, 16.26f}, // Gain 0.118
            {-47.65f, -29.72f, -12.04f, 3.90f, 13.02f, 15.80f, 16.22f, 16.26f, 16.26f}, // Gain 0.195
            {-38.68f, -20.87f, -3.78f, 9.36f, 14.89f, 16.12f, 16.25f, 16.26f, 16.26f}, // Gain 0.305
            {-29.01f, -11.56f, 3.96f, 13.04f, 15.83f, 16.23f, 16.26f, 16.26f, 16.26f}, // Gain 0.461
            {-18.96f, -2.47f, 9.91f, 15.10f, 16.16f, 16.25f, 16.26f, 16.26f, 16.26f}, // Gain 0.683
            {-9.10f, 5.35f, 13.61f, 15.95f, 16.24f, 16.26f, 16.26f, 16.26f, 16.26f} // Gain 1.000
        },
        { // 4 stages
            {-97.75f, -73.75f, -49.77f, -25.84f, -2.56f, 12.48f, 15.98f, 16.25f, 16.26f}, // Gain 0.000
            {-91.53f, -67.54f, -43.56f, -19.70f, 2.67f, 14.08f, 16.14f, 16.25f, 16.26f}, // Gain 0.027
            {-83.98f, -59.99f, -36.04f, -12.34f, 7.81f, 15.26f, 16.22f, 16.26f, 16.26f}, // Gain 0.064
            {-75.08f, -51.11f, -27.23f, -4.04f, 11.87f, 15.92f, 16.25f, 16.26f, 16.26f}, // Gain 0.118
            {-64.92f, -40.99f, -17.30f, 4.21f, 14.48f, 16.18f, 16.26f, 16.26f, 16.26f}, // Gain 0.195
            {-53.65f, -29.84f, -6.75f, 10.59f, 15.76f, 16.24f, 16.26f, 16.26f, 16.26f}, // Gain 0.305
            {-41.49f, -18.03f, 3.24f, 14.25f, 16.17f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.461
            {-28.81f, -6.30f, 10.62f, 15.79f, 16.25f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.683
            {-16.18f, 4.08f, 14.53f, 16.19f, 16.26f, 16.26f, 16.26f, 16.26f, 16.26f} // Gain 1.000
        }
    },
    { // Drive
        { // 1 stage
            {-24.84f, -18.84f, -12.87f, -6.96f, -1.28f, 3.79f, 7.80f, 10.60f, 12.48f}, // Gain 0.000
            {-16.73f, -10.77f, -4.92f, 0.60f, 5.34f, 8.92f, 11.35f, 12.99f, 14.09f}, // Gain 0.027
            {-10.24f, -4.40f, 1.07f, 5.72f, 9.18f, 11.53f, 13.11f, 14.17f, 14.83f}, // Gain 0.064
            {-4.37f, 1.09f, 5.74f, 9.19f, 11.54f, 13.12f, 14.17f, 14.83f, 15.20f}, // Gain 0.118
            {0.98f, 5.64f, 9.13f, 11.49f, 13.09f, 14.16f, 14.82f, 15.19f, 15.38f}, // Gain 0.195
            {5.53f, 9.05f, 11.44f, 13.05f, 14.13f, 14.81f, 15.19f, 15.38f, 15.47f}, // Gain 0.305
            {8.99f, 11.40f, 13.02f, 14.11f, 14.80f, 15.18f, 15.38f, 15.47f, 15.51f}, // Gain 0.461
            {11.37f, 13.01f, 14.10f, 14.79f, 15.18f, 15.38f, 15.47f, 15.51f, 15.53f}, // Gain 0.683
            {13.00f, 14.10f, 14.79f, 15.18f, 15.37f, 15.47f, 15.51f, 15.53f, 15.53f} // Gain 1.000
        },
        { // 2 stages
            {-48.84f, -36.84f, -24.85f, -12.94f, -1.49f, 7.58f, 12.43f, 14.58f, 15.32f}, // Gain 0.000
            {-39.44f, -27.47f, -15.57f, -4.07f, 5.68f, 11.57f, 14.25f, 15.23f, 15.48f}, // Gain 0.027
            {-31.37f, -19.48f, -7.90f, 2.48f, 9.83f, 13.56f, 15.02f, 15.44f, 15.52f}, // Gain 0.064
            {-23.63f, -12.01f, -1.27f, 7.28f, 12.42f, 14.67f, 15.35f, 15.51f, 15.53f}, // Gain 0.118
            {-16.04f, -5.13f, 4.15f, 10.67f, 14.06f, 15.21f, 15.48f, 15.53f, 15.53f}, // Gain 0.195
            {-8.88f, 0.76f, 8.31f, 13.02f, 14.96f, 15.43f, 15.52f, 15.53f, 15.54f}, // Gain 0.305
            {-2.64f, 5.50f, 11.41f, 14.47f, 15.35f, 15.51f, 15.53f, 15.54f, 15.54f}, // Gain 0.461
            {2.49f, 9.25f, 13.55f, 15.18f, 15.48f, 15.53f, 15.53f, 15.54f, 15.54f}, // Gain 0.683
            {6.69f, 12.09f, 14.77f, 15.44f, 15.52f, 15.53f, 15.54f, 15.54f, 15.54f} // Gain 1.000
        },
        { // 3 stages
            {-72.84f, -54.84f, -36.85f, -18.93f, -1.63f, 10.68f, 15.21f, 16.15f, 16.25f}, // Gain 0.000
            {-62.16f, -44.18f, -26.28f, -8.75f, 6.25f, 13.87f, 15.96f, 16.24f, 16.26f}, // Gain 0.027
            {-52.54f, -34.64f, -17.04f, -0.67f, 10.88f, 15.34f, 16.18f, 16.25f, 16.26f}, // Gain 0.064
            {-42.97f, -25.33f, -8.53f, 5.71f, 13.72f, 15.97f, 16.24f, 16.26f, 16.26f}, // Gain 0.118
            {-33.30f, -16.35f, -1.00f, 10.44f, 15.30f, 16.19f, 16.26f, 16.26f, 16.26f}, // Gain 0.195
            {-23.83f, -8.10f, 5.34f, 13.62f, 15.98f, 16.25f, 16.26f, 16.26f, 16.26f}, // Gain 0.305
            {-15.07f, -0.82f, 10.35f, 15.33f, 16.20f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.461
            {-7.24f, 5.50f, 13.70f, 16.01f, 16.25f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.683
            {-0.22f, 10.57f, 15.40f, 16.21f, 16.26f, 16.26f, 16.26f, 16.26f, 16.26f} // Gain 1.000
        },
        { // 4 stages
            {-96.84f, -72.84f, -48.85f, -24.93f, -1.76f, 12.74f, 16.01f, 16.25f, 16.26f}, // Gain 0.000
            {-84.87f, -60.90f, -36.99f, -13.46f, 6.80f, 15.07f, 16.21f, 16.26f, 16.26f}, // Gain 0.027
            {-73.70f, -49.80f, -26.20f, -3.83f, 11.75f, 15.93f, 16.25f, 16.26f, 16.26f}, // Gain 0.064
            {-62.31f, -38.67f, -15.86f, 4.21f, 14.54f, 16.19f, 16.26f, 16.26f, 16.26f}, // Gain 0.118
            {-50.56f, -27.61f, -6.23f, 10.30f, 15.79f, 16.25f, 16.26f, 16.26f, 16.26f}, // Gain 0.195
            {-38.80f, -17.06f, 2.37f, 14.07f, 16.17f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.305
            {-27.55f, -7.27f, 9.41f, 15.70f, 16.25f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.461
            {-17.08f, 1.68f, 13.87f, 16.16f, 16.26f, 16.26f, 16.26f, 16.26f, 16.26f}, // Gain 0.683
            {-7.29f, 9.18f, 15.67f, 16.25f, 16.26f, 16.26f, 16.26f, 16.26f, 16.26f} // Gain 1.000
        }
    }
};
// *INDENT-ON*
// clang-format on

inline constexpr double kOldFeedDb = -12.40;
inline constexpr double kOldFeedSlope = 0.89;

/// [voice][stages - 1][Gain row] -> the share of the tone stack's level that the new amp passes on
/// beyond the old one's, measured at Input Trim 0 with a mid scoop (Middle 0, Contour 0.6) for a
/// cut and Middle 1 for a boost: the two differ, since a boost drives a saturated stage further in
/// and a cut takes it out.
using ShareGrid = float[2][kStageCounts][kGainSteps];

// clang-format off
// *INDENT-OFF*
inline constexpr ShareGrid kToneCutShare = {
    { // Clean: a row per stage count, Gain across
        {0.01f, 0.03f, 0.05f, 0.08f, 0.12f, 0.15f, 0.19f, 0.23f, 0.28f},
        {0.05f, 0.07f, 0.11f, 0.16f, 0.21f, 0.27f, 0.33f, 0.37f, 0.38f},
        {0.04f, 0.09f, 0.19f, 0.36f, 0.52f, 0.64f, 0.71f, 0.71f, 0.67f},
        {0.07f, 0.15f, 0.34f, 0.59f, 0.76f, 0.83f, 0.80f, 0.68f, 0.53f}
    },
    { // Drive: a row per stage count, Gain across
        {0.02f, 0.09f, 0.16f, 0.20f, 0.25f, 0.29f, 0.31f, 0.27f, 0.18f},
        {0.05f, 0.12f, 0.19f, 0.26f, 0.33f, 0.36f, 0.34f, 0.24f, 0.17f},
        {-0.01f, 0.12f, 0.28f, 0.42f, 0.53f, 0.58f, 0.51f, 0.23f, -0.03f},
        {-0.04f, 0.16f, 0.38f, 0.56f, 0.67f, 0.67f, 0.49f, 0.15f, -0.02f}
    }
};
inline constexpr ShareGrid kToneBoostShare = {
    { // Clean: a row per stage count, Gain across
        {0.05f, 0.08f, 0.12f, 0.18f, 0.26f, 0.32f, 0.38f, 0.43f, 0.48f},
        {0.06f, 0.11f, 0.18f, 0.28f, 0.38f, 0.47f, 0.54f, 0.59f, 0.62f},
        {-0.07f, 0.01f, 0.14f, 0.29f, 0.40f, 0.43f, 0.38f, 0.29f, 0.19f},
        {-0.12f, 0.00f, 0.18f, 0.32f, 0.38f, 0.36f, 0.29f, 0.20f, 0.12f}
    },
    { // Drive: a row per stage count, Gain across
        {0.06f, 0.20f, 0.33f, 0.42f, 0.48f, 0.53f, 0.57f, 0.60f, 0.59f},
        {0.07f, 0.22f, 0.35f, 0.46f, 0.55f, 0.60f, 0.64f, 0.63f, 0.62f},
        {-0.24f, -0.02f, 0.16f, 0.28f, 0.31f, 0.25f, 0.12f, -0.01f, 0.00f},
        {-0.31f, -0.04f, 0.16f, 0.25f, 0.24f, 0.17f, 0.06f, 0.00f, 0.00f}
    }
};
// *INDENT-ON*
// clang-format on

/// `grid`'s heard gain for one voice, interpolated bilinearly across Gain and Input Trim.
[[nodiscard]] inline double GridLevelDb(const LevelGrid& grid, int voice, int stages, double gain,
                                        double trimDb) noexcept
{
    const double row = GainAxis(gain) * static_cast<double>(kGainSteps - 1);
    const double column = (std::clamp(trimDb, kTrimMinDb, kTrimMaxDb) - kTrimMinDb) / (kTrimMaxDb - kTrimMinDb) *
                          static_cast<double>(kTrimSteps - 1);
    const auto r = std::min(static_cast<std::size_t>(row), kGainSteps - 2);
    const auto c = std::min(static_cast<std::size_t>(column), kTrimSteps - 2);
    const double rf = row - static_cast<double>(r);
    const double cf = column - static_cast<double>(c);
    const auto& cells = grid[voice][stages - 1];
    const auto along = [&](std::size_t at) { return cells[at][c] + cf * (cells[at][c + 1] - cells[at][c]); };
    const double low = along(r);
    return low + rf * (along(r + 1) - low);
}

/// What the old Power Drive and Sag added, in dB, to an old amp whose heard gain was `oldDb`
/// without them. The calibration passes the feed it is trying.
[[nodiscard]] inline double OldPowerStageDb(double oldDb, double drive, double sag, double feedDb = kOldFeedDb,
                                            double feedSlope = kOldFeedSlope) noexcept
{
    constexpr int kPoints = 16;
    constexpr double kPi = 3.14159265358979323846;
    const double amplitude = std::pow(10.0, (feedDb + feedSlope * oldDb) / 20.0);
    const double held = (1.0 + 6.0 * drive) / (1.0 + sag * amplitude);
    double driven = 0.0, plain = 0.0;

    for (int i = 0; i < kPoints; ++i)
    {
        const double x = amplitude * std::sin(2.0 * kPi * (i + 0.5) / kPoints);
        driven += std::tanh(held * x) * std::tanh(held * x);
        plain += std::tanh(x) * std::tanh(x);
    }

    return 10.0 * std::log10(driven / plain);
}

/// The stored settings the level depends on, as the old amp read them.
struct Settings
{
    double voice = 0.0;
    double gain = 0.45;
    int stages = 2;
    double trimDb = 0.0;
    double drive = 0.0;
    double sag = 0.0;
    double bass = 0.5;
    double middle = 0.5;
    double treble = 0.5;
    double contour = 0.2;
};

/// What the tone stack (Bass, Middle, Treble and Contour, set as both amps set them) does to the
/// level of a flat signal against its defaults, in dB: its power gain averaged over 24 frequencies
/// spaced evenly in pitch from 100 Hz to 4 kHz, where a guitar is heard.
[[nodiscard]] inline double ToneStackDb(const Settings& settings) noexcept
{
    using namespace builtin_amp;
    constexpr double kRate = 48000.0;
    constexpr int kPoints = 24;
    const BiquadCoefficients set[] = {DesignLowShelf(120.0, 0.8, (settings.bass - 0.5) * 18.0, kRate),
                                      DesignPeaking(750.0, 0.9, (settings.middle - 0.5) * 18.0, kRate),
                                      DesignPeaking(600.0, 0.7, -12.0 * settings.contour, kRate),
                                      DesignHighShelf(3500.0, 0.9, (settings.treble - 0.5) * 18.0, kRate)};
    // At their defaults only Contour does anything.
    const BiquadCoefficients plain = DesignPeaking(600.0, 0.7, -12.0 * kParams[kContour].defaultValue, kRate);
    double sum = 0.0;

    for (int i = 0; i < kPoints; ++i)
    {
        const double hz = 100.0 * std::pow(40.0, (i + 0.5) / kPoints);
        const auto z1 = std::polar(1.0, -2.0 * builtin_amp::kPi * hz / kRate);
        std::complex<double> response = 1.0;

        for (const auto& section : set)
        {
            response *= biquad::Response(section, z1);
        }

        sum += std::norm(response) / std::norm(biquad::Response(plain, z1));
    }

    return 10.0 * std::log10(sum / kPoints);
}

/// How much more of ToneStackDb reaches the new amp's output than reached the old one's, at
/// these settings, the voices blended as for the old level.
[[nodiscard]] inline double ToneShare(const Settings& settings, double toneDb) noexcept
{
    const ShareGrid& grid = toneDb < 0.0 ? kToneCutShare : kToneBoostShare;
    const double row = GainAxis(settings.gain) * static_cast<double>(kGainSteps - 1);
    const auto r = std::min(static_cast<std::size_t>(row), kGainSteps - 2);
    const double rf = row - static_cast<double>(r);
    const auto at = [&](int voice) {
        const auto& cells = grid[voice][settings.stages - 1];
        return cells[r] + rf * (cells[r + 1] - cells[r]);
    };
    const double clean = at(0);
    return clean + settings.voice * (at(1) - clean);
}

/// How much the new amp's Output has to rise, in dB, to sound as loud as the old amp at these
/// settings.
[[nodiscard]] inline double OutputChangeDb(const Settings& settings) noexcept
{
    const auto at = [&settings](const LevelGrid& grid, int voice) {
        return GridLevelDb(grid, voice, settings.stages, settings.gain, settings.trimDb);
    };
    const double oldClean = at(kOldLevelDb, 0);
    const double oldDb = oldClean + settings.voice * (at(kOldLevelDb, 1) - oldClean);
    const int newVoice = settings.voice >= 0.5 ? 1 : 0;
    const double newDb = builtin_amp::TrimGainDb(newVoice, settings.stages, static_cast<float>(settings.gain),
                                                 static_cast<float>(settings.trimDb));
    const double toneDb = ToneStackDb(settings);
    return oldDb + OldPowerStageDb(oldDb, settings.drive, settings.sag) - newDb - ToneShare(settings, toneDb) * toneDb;
}

[[nodiscard]] inline bool IsBuiltinAmp(const std::string& type) noexcept
{
    return type == EffectGuids::kAmpBuiltin || type == "amp_builtin";
}

/// The Input Trim a stored node plays at. Older presets also carry stage1Gain..stage6Gain, which
/// both amps take as Input Trim too, applied in the map's order, so the last of them present wins.
[[nodiscard]] inline double StoredTrimDb(const std::map<std::string, double>& params) noexcept
{
    double trim = 0.0;

    for (const auto& [key, value] : params)
    {
        const auto legacy =
            std::find_if(std::begin(builtin_amp::kLegacyStageGainKeys), std::end(builtin_amp::kLegacyStageGainKeys),
                         [&key](const char* name) { return key == name; });

        if (key == "stageGain" || legacy != std::end(builtin_amp::kLegacyStageGainKeys))
        {
            trim = value;
        }
    }

    return trim;
}

/// Rewrites a stored Heavy American node's params for the current amp if it predates the
/// rebuild: Character's default, set explicitly, and the Output that keeps it as loud as it was.
/// Returns false, leaving `params` untouched, for any other node, including an amp node that
/// already has a Character.
inline bool MigrateParams(const std::string& type, std::map<std::string, double>& params)
{
    if (!IsBuiltinAmp(type) || params.find("character") != params.end())
    {
        return false;
    }

    const auto stored = [&params](const char* key, double fallback) {
        const auto it = params.find(key);
        return it != params.end() ? it->second : fallback;
    };

    // The old amp clamped its parameters to these ranges and rounded the stage count, so an
    // out-of-range value sounded as the clamped one did. Its defaults were the new amp's.
    Settings settings;
    settings.voice = std::clamp(stored("voice", 0.0), 0.0, 1.0);
    settings.gain = std::clamp(stored("gain", 0.45), 0.0, 1.0);
    settings.stages = std::clamp(static_cast<int>(std::lround(stored("stageCount", 2.0))), 1, kStageCounts);
    settings.trimDb = std::clamp(StoredTrimDb(params), kTrimMinDb, kTrimMaxDb);
    settings.drive = std::clamp(stored("powerDrive", 0.0), 0.0, 1.0);
    settings.sag = std::clamp(stored("sag", 0.0), 0.0, 1.0);
    settings.bass = std::clamp(stored("bass", 0.5), 0.0, 1.0);
    settings.middle = std::clamp(stored("middle", 0.5), 0.0, 1.0);
    settings.treble = std::clamp(stored("treble", 0.5), 0.0, 1.0);
    settings.contour = std::clamp(stored("contour", 0.2), 0.0, 1.0);
    const double outputDb = std::clamp(stored("output", 0.0), kOutputMinDb, kOutputMaxDb);

    params["character"] = kMigratedCharacter;
    params["output"] = std::clamp(outputDb + OutputChangeDb(settings), kOutputMinDb, kOutputMaxDb);
    return true;
}
} // namespace guitarfx::amp_legacy
