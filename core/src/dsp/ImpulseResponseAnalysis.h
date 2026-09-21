#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <span>
#include <vector>

namespace guitarfx
{
/**
 * The magnitude response of an impulse response, in dB, smoothed the way an ear hears it.
 *
 * A cabinet IR's raw response is full of narrow notches from the mic and the room; what
 * gives a cab its character is the envelope. Each output value is the power average over
 * `bandwidthOctaves` centred on its frequency. Only the first `windowSeconds` of the IR are
 * used, faded out over the last fifth, which keeps a long room tail from dominating.
 *
 * Evaluated directly (a complex sum per frequency) rather than with an FFT: the handful of
 * frequencies a curve needs is cheaper that way, and the result does not depend on how the
 * FFT's bins happen to fall. Returns an empty vector for an empty IR or a bad sample rate.
 */
[[nodiscard]] inline std::vector<double> SmoothedMagnitudeDb(std::span<const float> impulse, double sampleRate,
                                                             std::span<const double> frequenciesHz,
                                                             double bandwidthOctaves = 1.0 / 6.0,
                                                             double windowSeconds = 0.05)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kPointsPerBand = 7;

    if (impulse.empty() || !(sampleRate > 0.0))
    {
        return {};
    }

    const std::size_t length =
        std::min(impulse.size(), static_cast<std::size_t>(std::max(1.0, windowSeconds * sampleRate)));
    const std::size_t fadeStart = length - length / 5;
    std::vector<double> windowed(length);

    for (std::size_t n = 0; n < length; ++n)
    {
        double gain = 1.0;

        if (n >= fadeStart && length > fadeStart)
        {
            const double t = static_cast<double>(n - fadeStart) / static_cast<double>(length - fadeStart);
            gain = 0.5 * (1.0 + std::cos(kPi * t));
        }

        windowed[n] = static_cast<double>(impulse[n]) * gain;
    }

    std::vector<double> magnitudesDb;
    magnitudesDb.reserve(frequenciesHz.size());

    for (const double centreHz : frequenciesHz)
    {
        double power = 0.0;

        for (int point = 0; point < kPointsPerBand; ++point)
        {
            const double offsetOctaves = bandwidthOctaves * (static_cast<double>(point) / (kPointsPerBand - 1) - 0.5);
            const double hz = std::min(centreHz * std::exp2(offsetOctaves), sampleRate * 0.5);
            // Rotate a unit phasor rather than calling sin and cos per sample; in double the
            // drift over a few thousand steps is far below anything audible.
            const std::complex<double> step = std::polar(1.0, -2.0 * kPi * hz / sampleRate);
            std::complex<double> phasor = 1.0;
            std::complex<double> sum = 0.0;

            for (const double sample : windowed)
            {
                sum += sample * phasor;
                phasor *= step;
            }

            power += std::norm(sum);
        }

        magnitudesDb.push_back(10.0 * std::log10(std::max(power / kPointsPerBand, 1e-20)));
    }

    return magnitudesDb;
}
} // namespace guitarfx
