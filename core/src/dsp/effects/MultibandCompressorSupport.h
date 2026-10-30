#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/LinkwitzRileyCrossover.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/**
 * The Multi-band Compressor's parts that are not the effect itself (MultibandCompressorEffect.h):
 * its parameter table, how the crossovers are kept apart, and the per-band gain curve.
 */
namespace guitarfx::multiband_compressor
{
inline constexpr int kBands = 4;
inline constexpr int kSplits = kBands - 1;

using CrossoverDesign = lr_crossover::Design<kBands>;
using Crossover = lr_crossover::Crossover<kBands>;

using lr_crossover::kMinSplitRatio;

/// How long each band's detector averages over, low band first: long enough that a low note's
/// ripple does not read as level, short enough that the pick attack in the top bands is seen.
inline constexpr std::array<double, kBands> kDetectorMs = {8.0, 4.0, 2.0, 1.0};

/// Attack and Release are stretched in the lower bands: a fast release on a low note pumps, and
/// follows the waveform closely enough to distort it.
inline constexpr std::array<double, kBands> kTimeScale = {2.0, 1.5, 1.0, 1.0};

/// The gain curve is worked out every this many samples; the applied gain ramps every sample.
inline constexpr int kControlInterval = 16;

enum Param : std::size_t
{
    kCrossover1,
    kCrossover2,
    kCrossover3,
    kThreshold1,
    kRatio1,
    kKnee1,
    kGain1,
    kOn1,
    kSolo1,
    kThreshold2,
    kRatio2,
    kKnee2,
    kGain2,
    kOn2,
    kSolo2,
    kThreshold3,
    kRatio3,
    kKnee3,
    kGain3,
    kOn3,
    kSolo3,
    kThreshold4,
    kRatio4,
    kKnee4,
    kGain4,
    kOn4,
    kSolo4,
    kAttack,
    kRelease,
    kMix,
    kOutput,
    kParamCount,
};

/// What each band has, in this order, kBandParamCount params a band.
enum BandOffset : int
{
    kBandThreshold,
    kBandRatio,
    kBandKnee,
    kBandGain,
    kBandOn,
    kBandSolo,
    kBandParamCount,
};

/// Band `band`'s param `offset`.
[[nodiscard]] constexpr Param BandParam(int band, BandOffset offset) noexcept
{
    return static_cast<Param>(kThreshold1 + band * kBandParamCount + offset);
}

static_assert(BandParam(kBands - 1, kBandSolo) == kSolo4);

inline constexpr double kDefaultThresholdDb = -24.0;
inline constexpr double kDefaultRatio = 3.0;
inline constexpr double kDefaultKneeDb = 6.0;
inline constexpr double kMaxKneeDb = 24.0;

// clang-format off
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    LogTaper({"crossover1", "Low / Low Mid", 200.0, 40.0, 1000.0, "Hz", "Crossovers"}),
    LogTaper({"crossover2", "Low Mid / High Mid", 800.0, 150.0, 5000.0, "Hz", "Crossovers"}),
    LogTaper({"crossover3", "High Mid / High", 3000.0, 600.0, 12000.0, "Hz", "Crossovers"}),
    // A band turned off passes through untouched. While any band is soloed, only the soloed
    // bands are heard.
    {"threshold1", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "Low"},
    LogTaper({"ratio1", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "Low"}),
    {"knee1", "Knee", kDefaultKneeDb, 0.0, kMaxKneeDb, "dB", "Low"},
    {"gain1", "Makeup", 0.0, -12.0, 12.0, "dB", "Low"},
    {"on1", "On", 1.0, 0.0, 1.0, "toggle", "Low", false, 1.0},
    {"solo1", "Solo", 0.0, 0.0, 1.0, "toggle", "Low", false, 1.0},
    {"threshold2", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "Low Mid"},
    LogTaper({"ratio2", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "Low Mid"}),
    {"knee2", "Knee", kDefaultKneeDb, 0.0, kMaxKneeDb, "dB", "Low Mid"},
    {"gain2", "Makeup", 0.0, -12.0, 12.0, "dB", "Low Mid"},
    {"on2", "On", 1.0, 0.0, 1.0, "toggle", "Low Mid", false, 1.0},
    {"solo2", "Solo", 0.0, 0.0, 1.0, "toggle", "Low Mid", false, 1.0},
    {"threshold3", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "High Mid"},
    LogTaper({"ratio3", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "High Mid"}),
    {"knee3", "Knee", kDefaultKneeDb, 0.0, kMaxKneeDb, "dB", "High Mid"},
    {"gain3", "Makeup", 0.0, -12.0, 12.0, "dB", "High Mid"},
    {"on3", "On", 1.0, 0.0, 1.0, "toggle", "High Mid", false, 1.0},
    {"solo3", "Solo", 0.0, 0.0, 1.0, "toggle", "High Mid", false, 1.0},
    {"threshold4", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "High"},
    LogTaper({"ratio4", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "High"}),
    {"knee4", "Knee", kDefaultKneeDb, 0.0, kMaxKneeDb, "dB", "High"},
    {"gain4", "Makeup", 0.0, -12.0, 12.0, "dB", "High"},
    {"on4", "On", 1.0, 0.0, 1.0, "toggle", "High", false, 1.0},
    {"solo4", "Solo", 0.0, 0.0, 1.0, "toggle", "High", false, 1.0},
    LogTaper({"attack", "Attack", 10.0, 0.1, 100.0, "ms", ""}),
    LogTaper({"release", "Release", 120.0, 10.0, 1000.0, "ms", ""}),
    // Each band's compressed signal against the same band uncompressed, so the two share the
    // crossover's phase and a half-way Mix does not comb.
    {"mix", "Mix", 1.0, 0.0, 1.0, "amount", ""},
    {"output", "Output", 0.0, -18.0, 18.0, "dB", ""},
}};
// clang-format on

/// The crossovers as the engine uses them (lr_crossover::OrderedSplits).
[[nodiscard]] inline std::array<double, kSplits> OrderedSplits(const std::array<double, kSplits>& hz,
                                                               double nyquistGuardHz)
{
    return lr_crossover::OrderedSplits(hz, nyquistGuardHz);
}

/**
 * A band's gain reduction, in dB (0 or more), for its level `overDb` above its threshold
 * (negative: below it) at `ratio`, with a knee `kneeDb` wide centred on the threshold: none below
 * the knee, a parabola through it that leaves no corner, and (1 - 1/ratio) of the overshoot
 * beyond it. A knee of 0 is a hard knee.
 */
[[nodiscard]] inline double GainReductionDb(double overDb, double ratio, double kneeDb = kDefaultKneeDb)
{
    const double slope = 1.0 - 1.0 / std::max(ratio, 1.0);
    const double halfKnee = 0.5 * std::max(kneeDb, 0.0);

    if (overDb <= -halfKnee)
    {
        return 0.0;
    }

    if (overDb < halfKnee)
    {
        const double into = overDb + halfKnee;
        return slope * into * into / (4.0 * halfKnee);
    }

    return slope * overDb;
}
} // namespace guitarfx::multiband_compressor
