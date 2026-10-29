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

inline constexpr double kKneeDb = 6.0;

/// The gain curve is worked out every this many samples; the applied gain ramps every sample.
inline constexpr int kControlInterval = 16;

enum Param : std::size_t
{
    kCrossover1,
    kCrossover2,
    kCrossover3,
    kThreshold1,
    kRatio1,
    kGain1,
    kThreshold2,
    kRatio2,
    kGain2,
    kThreshold3,
    kRatio3,
    kGain3,
    kThreshold4,
    kRatio4,
    kGain4,
    kAttack,
    kRelease,
    kMix,
    kOutput,
    kParamCount,
};

/// Band `band`'s threshold, ratio and makeup: three params a band, in that order.
[[nodiscard]] constexpr Param BandParam(int band, int offset) noexcept
{
    return static_cast<Param>(kThreshold1 + band * 3 + offset);
}

inline constexpr double kDefaultThresholdDb = -24.0;
inline constexpr double kDefaultRatio = 3.0;

// clang-format off
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    LogTaper({"crossover1", "Low / Low Mid", 200.0, 40.0, 1000.0, "Hz", "Crossovers"}),
    LogTaper({"crossover2", "Low Mid / High Mid", 800.0, 150.0, 5000.0, "Hz", "Crossovers"}),
    LogTaper({"crossover3", "High Mid / High", 3000.0, 600.0, 12000.0, "Hz", "Crossovers"}),
    {"threshold1", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "Low"},
    LogTaper({"ratio1", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "Low"}),
    {"gain1", "Makeup", 0.0, -12.0, 12.0, "dB", "Low"},
    {"threshold2", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "Low Mid"},
    LogTaper({"ratio2", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "Low Mid"}),
    {"gain2", "Makeup", 0.0, -12.0, 12.0, "dB", "Low Mid"},
    {"threshold3", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "High Mid"},
    LogTaper({"ratio3", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "High Mid"}),
    {"gain3", "Makeup", 0.0, -12.0, 12.0, "dB", "High Mid"},
    {"threshold4", "Threshold", kDefaultThresholdDb, -60.0, 0.0, "dB", "High"},
    LogTaper({"ratio4", "Ratio", kDefaultRatio, 1.0, 20.0, ":1", "High"}),
    {"gain4", "Makeup", 0.0, -12.0, 12.0, "dB", "High"},
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
 * (negative: below it) at `ratio`: none below the knee, a parabola through the knee that leaves
 * it with no corner, and (1 - 1/ratio) of the overshoot beyond it.
 */
[[nodiscard]] inline double GainReductionDb(double overDb, double ratio)
{
    const double slope = 1.0 - 1.0 / std::max(ratio, 1.0);
    const double halfKnee = 0.5 * kKneeDb;

    if (overDb <= -halfKnee)
    {
        return 0.0;
    }

    if (overDb < halfKnee)
    {
        const double into = overDb + halfKnee;
        return slope * into * into / (2.0 * kKneeDb);
    }

    return slope * overDb;
}
} // namespace guitarfx::multiband_compressor
