#include "dsp/IrPairResponse.h"

#include "dsp/BiquadDesign.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::ir_pair_response
{
namespace
{
constexpr double kTwoPi = 2.0 * biquad::kPi;

/// The first kWindowMs of `mono`, the last fifth of that span faded out with a half Hann. An IR
/// that ends sooner is taken whole: fading its own last fifth would change the IR, not just cut
/// a long tail short.
std::vector<double> Windowed(const std::vector<float>& mono, double sampleRate)
{
    const auto span = static_cast<std::size_t>(std::max(1.0, std::round(kWindowMs * sampleRate / 1000.0)));
    const std::size_t length = std::min(mono.size(), span);
    const std::size_t fadeStart = span - span / 5;
    std::vector<double> out(length);

    for (std::size_t n = 0; n < length; ++n)
    {
        const double gain =
            n >= fadeStart
                ? 0.5 * (1.0 + std::cos(biquad::kPi * static_cast<double>(n - fadeStart) /
                                        static_cast<double>(span - fadeStart)))
                : 1.0;
        out[n] = static_cast<double>(mono[n]) * gain;
    }

    return out;
}

std::complex<double> Cascade(const std::vector<BiquadCoefficients>& sections, std::complex<double> z1)
{
    std::complex<double> response(1.0, 0.0);

    for (const auto& section : sections)
    {
        response *= biquad::Response(section, z1);
    }

    return response;
}
} // namespace

BandSpectrum MeasureBandSpectrum(const std::vector<float>& mono, double sampleRate,
                                 const std::vector<double>& frequencies)
{
    BandSpectrum spectrum;
    spectrum.frequencies = frequencies;
    const std::size_t count = frequencies.size() * kPointsPerBand;
    spectrum.pointHz.resize(count);
    spectrum.values.resize(count);
    const std::vector<double> samples = Windowed(mono, sampleRate);
    const double nyquist = sampleRate / 2.0;

    for (std::size_t band = 0; band < frequencies.size(); ++band)
    {
        for (int point = 0; point < kPointsPerBand; ++point)
        {
            const double offsetOctaves =
                kBandOctaves * (static_cast<double>(point) / static_cast<double>(kPointsPerBand - 1) - 0.5);
            const double hz = std::min(frequencies[band] * std::pow(2.0, offsetOctaves), nyquist);
            // A unit phasor turned once per sample, rather than a sine and cosine per sample.
            const std::complex<double> step = std::polar(1.0, -kTwoPi * hz / sampleRate);
            std::complex<double> phase(1.0, 0.0);
            std::complex<double> sum(0.0, 0.0);

            for (const double sample : samples)
            {
                sum += sample * phase;
                phase *= step;
            }

            const std::size_t index = band * kPointsPerBand + static_cast<std::size_t>(point);
            spectrum.pointHz[index] = hz;
            spectrum.values[index] = sum;
        }
    }

    return spectrum;
}

Curves Combine(const BandSpectrum* a, const BandSpectrum* b, const IRCabEffect::SlotSum& sum,
               double normalizeGainA, double normalizeGainB, double offsetMs, double sampleRate)
{
    Curves curves;
    const BandSpectrum* reference = a ? a : b;

    if (!reference)
    {
        return curves;
    }

    const std::size_t bands = reference->frequencies.size();
    curves.summedDb.resize(bands);
    curves.unrelatedDb.resize(bands);
    const double gainA = sum.slotAGain * (sum.normalize ? normalizeGainA : 1.0);
    const double gainB = sum.slotBGain * (sum.normalize ? normalizeGainB : 1.0);
    const double offsetSeconds = offsetMs / 1000.0;

    for (std::size_t band = 0; band < bands; ++band)
    {
        double summed = 0.0;
        double unrelated = 0.0;

        for (int point = 0; point < kPointsPerBand; ++point)
        {
            const std::size_t index = band * kPointsPerBand + static_cast<std::size_t>(point);
            const double hz = reference->pointHz[index];
            const std::complex<double> z1 = std::polar(1.0, -kTwoPi * hz / sampleRate);
            const std::complex<double> shared = sum.compGain * Cascade(sum.sumFilters, z1);
            const std::complex<double> slotA =
                a ? gainA * Cascade(sum.slotAFilters, z1) * a->values[index] : std::complex<double>{};
            // B played later by the offset: its response turns by -2 pi f offset.
            const std::complex<double> slotB = b ? gainB * Cascade(sum.slotBFilters, z1) * b->values[index] *
                                                       std::polar(1.0, -kTwoPi * hz * offsetSeconds)
                                                 : std::complex<double>{};
            const double sharedPower = std::norm(shared);
            summed += sharedPower * std::norm(slotA + slotB);
            unrelated += sharedPower * (std::norm(slotA) + std::norm(slotB));
        }

        curves.summedDb[band] = 10.0 * std::log10(std::max(summed / kPointsPerBand, 1e-20));
        curves.unrelatedDb[band] = 10.0 * std::log10(std::max(unrelated / kPointsPerBand, 1e-20));
    }

    return curves;
}
} // namespace guitarfx::ir_pair_response
