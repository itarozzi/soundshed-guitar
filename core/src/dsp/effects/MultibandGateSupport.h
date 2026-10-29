#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/LinkwitzRileyCrossover.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/**
 * The Multi-band Gate's parts that are not the effect itself (MultibandGateEffect.h): its
 * parameter table, the six-band crossover and the per-band gain curve.
 */
namespace guitarfx::multiband_gate
{
inline constexpr int kBands = 6;
inline constexpr int kSplits = kBands - 1;

/// Where the bands meet. Lows (hum, rumble), low mids (a guitar's body), mids, upper mids,
/// presence, and air, which is most of the hiss. Fixed, so a learned threshold keeps meaning
/// the same band.
inline constexpr std::array<double, kSplits> kSplitHz = {150.0, 400.0, 1000.0, 2500.0, 6000.0};

/// How long each band's detector averages over. Long enough that a note's ripple stays within
/// about a dB (a low E repeats every 12 ms, a drop-tuned low string every 25), short enough that
/// a band closes promptly once the playing in it stops.
inline constexpr std::array<double, kBands> kDetectorMs = {20.0, 8.0, 4.0, 3.0, 3.0, 3.0};

/// Below a band's threshold the gain falls this many dB for each dB the level does (1:5
/// downward expansion), through a soft knee this wide, so a band fades rather than switches.
inline constexpr double kSlope = 4.0;
inline constexpr double kKneeDb = 6.0;

/// Learn puts each threshold this far above the noise it heard: with the slope and knee above,
/// enough to take about 20 dB off the noise floor itself and more off the quieter noise under it.
inline constexpr double kLearnMarginDb = 8.0;

/// The gain curve is worked out every this many samples; the applied gain ramps every sample.
inline constexpr int kControlInterval = 16;

enum Param : std::size_t
{
    kThreshold1,
    kThreshold2,
    kThreshold3,
    kThreshold4,
    kThreshold5,
    kThreshold6,
    kOffset,
    kAttack,
    kRelease,
    kRange,
    kParamCount,
};

inline constexpr double kMinThresholdDb = -96.0;
inline constexpr double kMaxThresholdDb = 0.0;
inline constexpr double kDefaultThresholdDb = -65.0;

// clang-format off
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"threshold1", "Lows", kDefaultThresholdDb, kMinThresholdDb, kMaxThresholdDb, "dB", "Thresholds"},
    {"threshold2", "Low Mids", kDefaultThresholdDb, kMinThresholdDb, kMaxThresholdDb, "dB", "Thresholds"},
    {"threshold3", "Mids", kDefaultThresholdDb, kMinThresholdDb, kMaxThresholdDb, "dB", "Thresholds"},
    {"threshold4", "Upper Mids", kDefaultThresholdDb, kMinThresholdDb, kMaxThresholdDb, "dB", "Thresholds"},
    {"threshold5", "Presence", kDefaultThresholdDb, kMinThresholdDb, kMaxThresholdDb, "dB", "Thresholds"},
    {"threshold6", "Air", kDefaultThresholdDb, kMinThresholdDb, kMaxThresholdDb, "dB", "Thresholds"},
    // Moves all six together, for a quick "a little more" or "a little less" after Learn.
    {"offset", "Offset", 0.0, -24.0, 24.0, "dB", ""},
    {"attack", "Attack", 1.0, 0.1, 50.0, "ms", ""},
    LogTaper({"release", "Release", 120.0, 5.0, 1000.0, "ms", ""}),
    // The deepest a band is cut. Not silence: a closed band is turned down, never off.
    {"range", "Range", -40.0, -80.0, 0.0, "dB", ""},
}};
// clang-format on

/// The six-band split (dsp/LinkwitzRileyCrossover.h) at the fixed kSplitHz.
using CrossoverDesign = lr_crossover::Design<kBands>;
using Crossover = lr_crossover::Crossover<kBands>;

/**
 * A band's gain, in dB, for its level `belowDb` under its threshold (0 or less: at or above
 * it). Zero above the threshold; through the knee a parabola that leaves it with no corner;
 * beyond the knee a straight line of kSlope; never deeper than `rangeDb`.
 */
[[nodiscard]] inline double GainDb(double belowDb, double rangeDb)
{
    if (belowDb <= 0.0)
    {
        return 0.0;
    }

    const double cut =
        belowDb < kKneeDb ? kSlope * belowDb * belowDb / (2.0 * kKneeDb) : kSlope * (belowDb - 0.5 * kKneeDb);
    return std::max(-cut, rangeDb);
}
} // namespace guitarfx::multiband_gate
