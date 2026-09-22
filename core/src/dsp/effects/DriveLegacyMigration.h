#pragma once

#include "dsp/EffectGuids.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>

namespace guitarfx::drive_legacy
{
/**
 * Brings presets saved before the drive pedals had a Model switch up to the pedals that
 * replaced them, at the same loudness.
 *
 * The old Overdrive, Distortion and Fuzz came out 13-25 dB above bypass with Level at 0 dB,
 * so their presets set Level against that. The new pedals are calibrated to bypass
 * loudness, and a node from an old preset would lose that level: quieter on its own, and
 * pushing the amp after it that much less. So when a stored drive node has no `model` key,
 * which only an old preset lacks (a node created since carries every parameter), it is
 * given the first model of its family explicitly and the Level that makes it as loud as it
 * used to be.
 *
 * That Level comes from a table, not a formula: the old pedals' loudness moved with Tone as
 * well as Drive, and the old output limiter compressed everything above Level 0 dB. For
 * each pedal, each point below is the new Level (dB) at which the new pedal's first model,
 * at that Drive and Tone, is as loud as the old pedal at that Drive, Tone and Level. Both
 * were measured K-weighted on the plucked-string guitar phrase of DrivePedalTests at the
 * nominal operating level, and the new Level found by bisection. Between points it is
 * interpolated; `DriveLegacyMigrationTests --calibrate` prints the tables.
 *
 * The old Distortion is why the new pedals' Level reaches +24 dB: it came out 16 dB above
 * bypass at Level 0, and a dark old Tone lands on the RAT's much darker Filter, so old
 * presets with a few dB of Level need +20 dB or more. Only settings that held the old pedal
 * at full scale in its output limiter (old Level near +12 dB with high Drive) still reach the
 * +24 dB ceiling; there the new pedal meets the same limiter, and falls short by at most
 * about 2 dB (measured: 1.8 dB at full Drive, Tone 0.25, Level +12 on the Distortion).
 *
 * Mix is left alone: Level acts on the wet signal in both pedals, so matching the wet
 * loudness matches the blend.
 */
enum class Family
{
    None,
    Overdrive,
    Distortion,
    Fuzz
};

/// The family of a node's effect type, by canonical id or legacy alias; the registry need
/// not be populated.
[[nodiscard]] inline Family FamilyOf(const std::string& type) noexcept
{
    if (type == EffectGuids::kOverdrive || type == "overdrive")
    {
        return Family::Overdrive;
    }

    if (type == EffectGuids::kDistortion || type == "distortion")
    {
        return Family::Distortion;
    }

    if (type == EffectGuids::kFuzz || type == "fuzz")
    {
        return Family::Fuzz;
    }

    return Family::None;
}

inline constexpr std::size_t kDriveSteps = 9; ///< old Drive 0 to 1, closer together near 0
inline constexpr std::size_t kToneSteps = 5;  ///< old Tone 0 to 1 in quarters
inline constexpr std::size_t kLevelSteps = 5; ///< old Level -12 to +12 dB in 6 dB steps
inline constexpr double kOldLevelMinDb = -12.0;
inline constexpr double kOldLevelMaxDb = 12.0;
inline constexpr double kNewLevelMinDb = -24.0;
inline constexpr double kNewLevelMaxDb = 24.0;

/// [drive][tone][level] -> new Level, dB.
using LevelTable = std::array<std::array<std::array<double, kLevelSteps>, kToneSteps>, kDriveSteps>;

/// The old pedals' gain was linear in Drive (1 + k Drive, k from 9 to 23), so their loudness
/// moved fastest at the bottom of the knob: the old Fuzz rose 10 dB in its first eighth. The
/// table's drive points are therefore even on log(1 + 16 Drive), not on Drive.
inline constexpr double kDriveAxisSpread = 16.0;

[[nodiscard]] inline double DriveAxis(double drive) noexcept
{
    return std::log1p(kDriveAxisSpread * std::clamp(drive, 0.0, 1.0)) / std::log1p(kDriveAxisSpread);
}

/// The old Drive at table row `index`: 0, 0.027, 0.064, 0.118, 0.195, 0.30, 0.46, 0.68, 1.
[[nodiscard]] inline double DriveAtRow(std::size_t index) noexcept
{
    const double axis = static_cast<double>(index) / static_cast<double>(kDriveSteps - 1);
    return std::expm1(axis * std::log1p(kDriveAxisSpread)) / kDriveAxisSpread;
}

// clang-format off
// *INDENT-OFF*
/// Old Overdrive onto the TS-808.
inline constexpr LevelTable kOverdriveLevels = {{
    {{{-17.3, -11.3, -5.3, 0.7, 6.7}, {-12.5, -6.5, -0.5, 5.5, 11.0}, {-11.5, -5.5, 0.5, 6.4, 11.4}, {-14.3, -8.3, -2.3, 3.5, 8.1}, {-17.8, -11.8, -5.8, -0.1, 4.5}}}, // drive 0.000
    {{{-15.6, -9.6, -3.6, 2.4, 8.2}, {-10.9, -4.9, 1.1, 7.1, 12.5}, {-9.9, -3.9, 2.1, 7.9, 12.6}, {-12.7, -6.7, -0.7, 5.0, 9.3}, {-16.2, -10.2, -4.2, 1.4, 5.6}}}, // drive 0.027
    {{{-13.8, -7.8, -1.8, 4.2, 9.8}, {-9.1, -3.1, 2.9, 8.9, 14.0}, {-8.1, -2.1, 3.9, 9.5, 14.0}, {-11.0, -5.0, 1.0, 6.5, 10.5}, {-14.4, -8.4, -2.4, 2.9, 6.7}}}, // drive 0.064
    {{{-11.9, -5.9, 0.1, 6.1, 11.5}, {-7.2, -1.2, 4.8, 10.7, 15.7}, {-6.3, -0.3, 5.7, 11.2, 15.5}, {-9.1, -3.1, 2.9, 8.0, 11.7}, {-12.6, -6.6, -0.6, 4.4, 7.8}}}, // drive 0.118
    {{{-10.0, -4.0, 2.0, 8.0, 13.0}, {-5.4, 0.6, 6.6, 12.5, 17.4}, {-4.5, 1.5, 7.5, 12.8, 17.1}, {-7.3, -1.3, 4.7, 9.5, 12.8}, {-10.8, -4.8, 1.2, 5.8, 8.8}}}, // drive 0.195
    {{{-8.2, -2.2, 3.8, 9.7, 14.5}, {-3.6, 2.4, 8.4, 14.2, 19.2}, {-2.7, 3.3, 9.3, 14.5, 18.8}, {-5.6, 0.4, 6.4, 10.9, 13.9}, {-9.1, -3.1, 2.9, 7.1, 9.8}}}, // drive 0.305
    {{{-6.6, -0.6, 5.4, 11.2, 15.8}, {-2.0, 4.0, 10.0, 16.0, 21.3}, {-1.2, 4.8, 10.9, 16.1, 20.7}, {-4.0, 2.0, 8.0, 12.1, 14.9}, {-7.5, -1.5, 4.4, 8.2, 10.6}}}, // drive 0.461
    {{{-5.1, 0.9, 6.9, 12.5, 17.0}, {-0.6, 5.4, 11.4, 17.8, 23.7}, {0.2, 6.2, 12.4, 17.8, 22.8}, {-2.6, 3.4, 9.4, 13.2, 15.8}, {-6.1, -0.1, 5.8, 9.3, 11.4}}}, // drive 0.683
    {{{-4.0, 2.0, 8.0, 13.6, 18.3}, {0.5, 6.5, 12.7, 19.8, 24.0}, {1.4, 7.4, 13.9, 19.8, 24.0}, {-1.4, 4.6, 10.8, 14.3, 16.8}, {-4.9, 1.1, 7.2, 10.3, 12.4}}}, // drive 1.000
}};

/// Old Distortion onto the RAT.
inline constexpr LevelTable kDistortionLevels = {{
    {{{-13.5, -7.5, -1.5, 4.5, 10.4}, {-11.8, -5.8, 0.2, 6.2, 11.9}, {-12.4, -6.4, -0.4, 5.5, 10.8}, {-14.5, -8.5, -2.5, 3.4, 8.4}, {-15.2, -9.2, -3.2, 2.7, 7.7}}}, // drive 0.000
    {{{-10.4, -4.4, 1.6, 7.6, 13.1}, {-9.5, -3.5, 2.5, 8.3, 13.3}, {-11.1, -5.1, 0.9, 6.4, 10.6}, {-13.9, -7.9, -1.9, 3.5, 7.3}, {-14.7, -8.7, -2.7, 2.6, 6.5}}}, // drive 0.027
    {{{-6.4, -0.4, 5.6, 11.5, 16.5}, {-5.7, 0.3, 6.3, 11.7, 16.1}, {-7.5, -1.5, 4.5, 9.2, 12.8}, {-10.3, -4.3, 1.7, 6.3, 9.1}, {-11.1, -5.1, 0.9, 5.4, 8.3}}}, // drive 0.064
    {{{-1.2, 4.8, 10.8, 16.4, 21.0}, {-0.7, 5.3, 11.3, 16.4, 20.7}, {-2.5, 3.5, 9.5, 13.4, 16.8}, {-5.2, 0.8, 6.8, 10.4, 12.5}, {-6.1, -0.1, 5.9, 9.5, 11.6}}}, // drive 0.118
    {{{1.3, 7.3, 13.3, 18.8, 23.5}, {1.8, 7.8, 13.8, 19.0, 23.7}, {0.0, 6.0, 12.0, 15.4, 19.1}, {-2.8, 3.2, 9.2, 12.0, 13.6}, {-3.7, 2.3, 8.3, 11.1, 12.8}}}, // drive 0.195
    {{{3.1, 9.1, 15.1, 20.8, 24.0}, {3.6, 9.6, 15.7, 21.9, 24.0}, {1.8, 7.8, 13.9, 17.7, 21.7}, {-0.9, 5.1, 11.0, 13.2, 14.6}, {-1.8, 4.2, 10.1, 12.3, 13.7}}}, // drive 0.305
    {{{4.3, 10.3, 16.4, 22.6, 24.0}, {4.9, 10.9, 17.5, 24.0, 24.0}, {3.2, 9.2, 15.7, 20.4, 24.0}, {0.5, 6.5, 12.5, 14.3, 15.8}, {-0.4, 5.6, 11.6, 13.4, 14.7}}}, // drive 0.461
    {{{5.1, 11.1, 17.2, 24.0, 24.0}, {5.8, 11.8, 19.1, 24.0, 24.0}, {4.3, 10.3, 18.2, 24.0, 24.0}, {1.8, 7.8, 13.9, 15.8, 18.8}, {1.0, 7.0, 13.1, 14.9, 16.5}}}, // drive 0.683
    {{{4.9, 10.9, 17.2, 24.0, 24.0}, {6.1, 12.1, 21.1, 24.0, 24.0}, {5.2, 11.2, 23.8, 24.0, 24.0}, {3.1, 9.1, 15.9, 23.0, 24.0}, {2.3, 8.3, 15.1, 19.8, 24.0}}}, // drive 1.000
}};

/// Old Fuzz onto the Fuzz Face.
inline constexpr LevelTable kFuzzLevels = {{
    {{{-22.2, -16.2, -10.2, -4.2, 1.8}, {-15.7, -9.7, -3.7, 2.3, 8.1}, {-13.7, -7.7, -1.7, 4.3, 9.6}, {-15.4, -9.4, -3.4, 2.6, 7.8}, {-17.9, -11.9, -5.9, 0.1, 5.2}}}, // drive 0.000
    {{{-18.7, -12.6, -6.6, -0.6, 5.1}, {-12.0, -6.0, 0.0, 6.0, 10.7}, {-10.0, -4.0, 2.0, 7.8, 11.7}, {-11.7, -5.7, 0.3, 6.1, 10.0}, {-14.2, -8.2, -2.2, 3.5, 7.4}}}, // drive 0.027
    {{{-15.3, -9.3, -3.3, 2.7, 7.6}, {-8.6, -2.6, 3.4, 9.0, 12.8}, {-6.6, -0.6, 5.4, 10.4, 13.5}, {-8.3, -2.3, 3.7, 8.6, 11.7}, {-10.8, -4.8, 1.2, 6.0, 9.3}}}, // drive 0.064
    {{{-12.5, -6.5, -0.5, 5.3, 9.5}, {-5.8, 0.2, 6.2, 11.3, 14.6}, {-3.8, 2.2, 8.2, 12.3, 15.2}, {-5.5, 0.5, 6.5, 10.5, 13.3}, {-8.1, -2.1, 3.9, 8.0, 11.0}}}, // drive 0.118
    {{{-10.5, -4.5, 1.5, 7.1, 10.9}, {-3.8, 2.2, 8.2, 13.0, 16.1}, {-1.9, 4.1, 10.1, 13.9, 16.6}, {-3.6, 2.4, 8.3, 12.1, 14.8}, {-6.2, -0.2, 5.7, 9.6, 12.7}}}, // drive 0.195
    {{{-9.2, -3.2, 2.8, 8.2, 11.8}, {-2.5, 3.5, 9.4, 14.3, 17.4}, {-0.7, 5.3, 11.3, 15.2, 17.7}, {-2.4, 3.6, 9.5, 13.3, 16.0}, {-5.0, 1.0, 7.0, 10.9, 14.2}}}, // drive 0.305
    {{{-8.6, -2.6, 3.4, 8.8, 12.4}, {-1.9, 4.1, 10.1, 15.1, 18.2}, {-0.1, 5.9, 11.9, 16.0, 18.4}, {-1.9, 4.1, 10.1, 13.9, 16.8}, {-4.5, 1.5, 7.6, 11.6, 15.1}}}, // drive 0.461
    {{{-9.2, -3.2, 2.8, 8.3, 12.3}, {-2.5, 3.5, 9.5, 14.5, 17.8}, {-0.7, 5.3, 11.2, 15.4, 18.1}, {-2.5, 3.5, 9.4, 13.2, 16.0}, {-5.1, 0.9, 6.8, 10.8, 14.1}}}, // drive 0.683
    {{{-13.3, -7.3, -1.3, 3.8, 6.4}, {-6.4, -0.4, 5.6, 8.7, 10.4}, {-4.4, 1.6, 7.4, 9.4, 11.1}, {-6.2, -0.2, 5.6, 7.5, 9.2}, {-8.8, -2.8, 2.9, 4.8, 6.7}}}, // drive 1.000
}};
// *INDENT-ON*
// clang-format on

/// The old pedals' Drive defaults, for a node that never stored one. Tone was 0.5, Level
/// 0 dB and Mix 1 on all three.
[[nodiscard]] inline double OldDefaultDrive(Family family) noexcept
{
    return family == Family::Overdrive ? 0.5 : (family == Family::Distortion ? 0.6 : 0.7);
}

/// Where `value` falls on `steps` points evenly spanning [low, high]: the lower point's index
/// and how far toward the next.
struct GridPosition
{
    std::size_t index;
    double fraction;
};

[[nodiscard]] inline GridPosition Locate(double value, double low, double high, std::size_t steps) noexcept
{
    const double position = (std::clamp(value, low, high) - low) / (high - low) * static_cast<double>(steps - 1);
    const auto index = std::min(static_cast<std::size_t>(position), steps - 2);
    return {index, position - static_cast<double>(index)};
}

/// The new Level matching the old pedal at `drive`, `tone` and `levelDb`, interpolated
/// trilinearly.
[[nodiscard]] inline double MatchedLevelDb(const LevelTable& table, double drive, double tone, double levelDb) noexcept
{
    const auto d = Locate(DriveAxis(drive), 0.0, 1.0, kDriveSteps);
    const auto t = Locate(tone, 0.0, 1.0, kToneSteps);
    const auto l = Locate(levelDb, kOldLevelMinDb, kOldLevelMaxDb, kLevelSteps);
    double result = 0.0;

    for (std::size_t corner = 0; corner < 8; ++corner)
    {
        const std::size_t di = (corner & 1) != 0 ? 1 : 0;
        const std::size_t ti = (corner & 2) != 0 ? 1 : 0;
        const std::size_t li = (corner & 4) != 0 ? 1 : 0;
        const double weight = (di != 0 ? d.fraction : 1.0 - d.fraction) * (ti != 0 ? t.fraction : 1.0 - t.fraction) *
                              (li != 0 ? l.fraction : 1.0 - l.fraction);
        result += weight * table[d.index + di][t.index + ti][l.index + li];
    }

    return result;
}

/// Rewrites a stored drive node's params for the current pedal if it predates the Model
/// switch: the first model of its family, set explicitly, and the Level that keeps it as loud
/// as it was. Returns false, leaving `params` untouched, for any other node, including a
/// drive node that already has a model.
inline bool MigrateParams(const std::string& type, std::map<std::string, double>& params)
{
    const Family family = FamilyOf(type);

    if (family == Family::None || params.find("model") != params.end())
    {
        return false;
    }

    const auto stored = [&params](const char* key, double fallback) {
        const auto it = params.find(key);
        return it != params.end() ? it->second : fallback;
    };

    // The old pedals clamped their parameters to these ranges, so an out-of-range value
    // sounded as the clamped one did.
    const double drive = std::clamp(stored("drive", OldDefaultDrive(family)), 0.0, 1.0);
    const double tone = std::clamp(stored("tone", 0.5), 0.0, 1.0);
    const double levelDb = std::clamp(stored("level", 0.0), kOldLevelMinDb, kOldLevelMaxDb);
    const LevelTable& table = family == Family::Overdrive    ? kOverdriveLevels
                              : family == Family::Distortion ? kDistortionLevels
                                                             : kFuzzLevels;

    params["model"] = 0.0;
    params["drive"] = drive;
    params["tone"] = tone;
    params["level"] = std::clamp(MatchedLevelDb(table, drive, tone, levelDb), kNewLevelMinDb, kNewLevelMaxDb);
    return true;
}
} // namespace guitarfx::drive_legacy
