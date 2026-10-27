#pragma once

/**
 * IrPairResponse.h - The response of the IR Cabinet's two IRs summed, as its Alignment section
 * draws it.
 *
 * Two IRs blended sum like two mics on one speaker: lined up they reinforce, a fraction of a
 * millisecond apart they comb-filter. The section draws the two summed with IR B moved by its
 * offset, against the two added as if they could not interfere (their powers summed), so a
 * cancellation shows as a dip. How the cab shapes the sum (each slot's gain and Mic Position,
 * Auto Gain Comp, Low Cut, High Cut and Air) comes from IRCabEffect::DescribeSlotSum, the
 * designs the cab plays, so the curve cannot drift from what is heard.
 *
 * Each IR's spectrum is measured once (MeasureBandSpectrum) and the sum for any offset and any
 * settings is then a rotation and a few filter responses per point (Combine), cheap enough to
 * answer every step of a slider drag.
 */

#include "dsp/effects/IRCabEffect.h"

#include <complex>
#include <vector>

namespace guitarfx::ir_pair_response
{
/// Each band's figure is the power average of this many points across it, 1/6 octave wide.
inline constexpr int kPointsPerBand = 7;
inline constexpr double kBandOctaves = 1.0 / 6.0;
/// The part of each IR the response is taken from, faded out over its last fifth.
inline constexpr double kWindowMs = 50.0;

/// One IR's complex response at every point of every band.
struct BandSpectrum
{
    std::vector<double> frequencies;           // the band centres
    std::vector<double> pointHz;               // per band, kPointsPerBand points
    std::vector<std::complex<double>> values;  // the IR's response at each point
};

/// The response of the start of `mono` (kWindowMs of it, at `sampleRate`) around each of
/// `frequencies`.
[[nodiscard]] BandSpectrum MeasureBandSpectrum(const std::vector<float>& mono, double sampleRate,
                                               const std::vector<double>& frequencies);

struct Curves
{
    std::vector<double> summedDb;    // the two summed, IR B moved by the offset
    std::vector<double> unrelatedDb; // their powers summed: where summedDb falls below, they cancel
};

/// A and B (either may be null: a lone IR) shaped and summed as `sum` says, IR B `offsetMs`
/// later than A. `normalizeGainA/B` are the gains Normalize gives each IR, used when
/// `sum.normalize` is set. Both spectra must be measured at the same frequencies.
[[nodiscard]] Curves Combine(const BandSpectrum* a, const BandSpectrum* b, const IRCabEffect::SlotSum& sum,
                             double normalizeGainA, double normalizeGainB, double offsetMs, double sampleRate);
} // namespace guitarfx::ir_pair_response
