#pragma once

#include "dsp/BiquadFrequency.h"
#include "dsp/LinearRamp.h"
#include <cmath>
#include <complex>

namespace guitarfx
{
/// One second-order section, normalised so a0 == 1:
///
///     H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
///
/// Default-constructed it is the identity, so an unused band passes audio untouched.
struct BiquadCoefficients
{
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// Element-wise arithmetic, so a coefficient set can be interpolated like a number.

[[nodiscard]] constexpr BiquadCoefficients operator+(const BiquadCoefficients& x, const BiquadCoefficients& y) noexcept
{
    return {x.b0 + y.b0, x.b1 + y.b1, x.b2 + y.b2, x.a1 + y.a1, x.a2 + y.a2};
}

[[nodiscard]] constexpr BiquadCoefficients operator-(const BiquadCoefficients& x, const BiquadCoefficients& y) noexcept
{
    return {x.b0 - y.b0, x.b1 - y.b1, x.b2 - y.b2, x.a1 - y.a1, x.a2 - y.a2};
}

[[nodiscard]] constexpr BiquadCoefficients operator*(const BiquadCoefficients& x, double scale) noexcept
{
    return {x.b0 * scale, x.b1 * scale, x.b2 * scale, x.a1 * scale, x.a2 * scale};
}

/// RBJ "Audio EQ Cookbook" designs, their responses and the state to run them.
///
/// Every design passes its frequency through ClampBiquadFrequency, so a filter written for
/// 48 kHz stays stable when a host runs at 8 kHz (see BiquadFrequency.h for why that
/// matters). New code should design filters here rather than carry another copy of the
/// cookbook formulas.
namespace biquad
{
inline constexpr double kPi = 3.14159265358979323846;

/// Q of a second-order Butterworth section, and of an RBJ shelf with slope S = 1.
inline constexpr double kButterworthQ = 0.70710678118654752;

/// Q of section `section` (0-based) of an even-order Butterworth low- or high-pass built
/// from `order / 2` cascaded biquads. For order 6 that is 0.518, 0.707 and 1.932.
[[nodiscard]] inline double ButterworthSectionQ(int order, int section) noexcept
{
    return 1.0 / (2.0 * std::cos((2.0 * section + 1.0) * kPi / (2.0 * order)));
}

/// The Q that gives an RBJ shelf the same shape as the cookbook's slope parameter S,
/// for effects whose shelves were specified by slope.
[[nodiscard]] inline double ShelfQFromSlope(double slope, double gainDb) noexcept
{
    const double A = std::pow(10.0, gainDb / 40.0);
    return 1.0 / std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
}

namespace detail
{
struct Angle
{
    double cosine;
    double alpha;
};

[[nodiscard]] inline Angle DesignAngle(double frequencyHz, double q, double sampleRate) noexcept
{
    const double w0 = 2.0 * kPi * ClampBiquadFrequency(frequencyHz, sampleRate) / sampleRate;
    return {std::cos(w0), std::sin(w0) / (2.0 * q)};
}
} // namespace detail

[[nodiscard]] inline BiquadCoefficients LowPass(double frequencyHz, double q, double sampleRate) noexcept
{
    const auto [cosine, alpha] = detail::DesignAngle(frequencyHz, q, sampleRate);
    const double invA0 = 1.0 / (1.0 + alpha);
    return {(1.0 - cosine) * 0.5 * invA0, (1.0 - cosine) * invA0, (1.0 - cosine) * 0.5 * invA0, -2.0 * cosine * invA0,
            (1.0 - alpha) * invA0};
}

[[nodiscard]] inline BiquadCoefficients HighPass(double frequencyHz, double q, double sampleRate) noexcept
{
    const auto [cosine, alpha] = detail::DesignAngle(frequencyHz, q, sampleRate);
    const double invA0 = 1.0 / (1.0 + alpha);
    return {(1.0 + cosine) * 0.5 * invA0, -(1.0 + cosine) * invA0, (1.0 + cosine) * 0.5 * invA0, -2.0 * cosine * invA0,
            (1.0 - alpha) * invA0};
}

// The bell and the shelves return the identity at 0 dB. The cookbook formulas reach it only
// to within rounding; returning it exactly keeps an idle band bit-transparent.

[[nodiscard]] inline BiquadCoefficients Peaking(double frequencyHz, double q, double gainDb, double sampleRate) noexcept
{
    if (gainDb == 0.0)
    {
        return {};
    }

    const double A = std::pow(10.0, gainDb / 40.0);
    const auto [cosine, alpha] = detail::DesignAngle(frequencyHz, q, sampleRate);
    const double invA0 = 1.0 / (1.0 + alpha / A);
    return {(1.0 + alpha * A) * invA0, -2.0 * cosine * invA0, (1.0 - alpha * A) * invA0, -2.0 * cosine * invA0,
            (1.0 - alpha / A) * invA0};
}

[[nodiscard]] inline BiquadCoefficients LowShelf(double frequencyHz, double q, double gainDb,
                                                 double sampleRate) noexcept
{
    if (gainDb == 0.0)
    {
        return {};
    }

    const double A = std::pow(10.0, gainDb / 40.0);
    const auto [cosine, alpha] = detail::DesignAngle(frequencyHz, q, sampleRate);
    const double twoRootAAlpha = 2.0 * std::sqrt(A) * alpha;
    const double invA0 = 1.0 / ((A + 1.0) + (A - 1.0) * cosine + twoRootAAlpha);
    return {A * ((A + 1.0) - (A - 1.0) * cosine + twoRootAAlpha) * invA0,
            2.0 * A * ((A - 1.0) - (A + 1.0) * cosine) * invA0,
            A * ((A + 1.0) - (A - 1.0) * cosine - twoRootAAlpha) * invA0,
            -2.0 * ((A - 1.0) + (A + 1.0) * cosine) * invA0, ((A + 1.0) + (A - 1.0) * cosine - twoRootAAlpha) * invA0};
}

[[nodiscard]] inline BiquadCoefficients HighShelf(double frequencyHz, double q, double gainDb,
                                                  double sampleRate) noexcept
{
    if (gainDb == 0.0)
    {
        return {};
    }

    const double A = std::pow(10.0, gainDb / 40.0);
    const auto [cosine, alpha] = detail::DesignAngle(frequencyHz, q, sampleRate);
    const double twoRootAAlpha = 2.0 * std::sqrt(A) * alpha;
    const double invA0 = 1.0 / ((A + 1.0) - (A - 1.0) * cosine + twoRootAAlpha);
    return {A * ((A + 1.0) + (A - 1.0) * cosine + twoRootAAlpha) * invA0,
            -2.0 * A * ((A - 1.0) + (A + 1.0) * cosine) * invA0,
            A * ((A + 1.0) + (A - 1.0) * cosine - twoRootAAlpha) * invA0,
            2.0 * ((A - 1.0) - (A + 1.0) * cosine) * invA0, ((A + 1.0) - (A - 1.0) * cosine - twoRootAAlpha) * invA0};
}

/// The complex response of one section given z^-1 = e^(-jw). Callers evaluating many
/// sections at one frequency compute z^-1 once and pass it to each.
[[nodiscard]] inline std::complex<double> Response(const BiquadCoefficients& c, std::complex<double> z1) noexcept
{
    const std::complex<double> z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

/// The complex response of one section at `frequencyHz`: H evaluated at z = e^(jw).
[[nodiscard]] inline std::complex<double> Response(const BiquadCoefficients& c, double frequencyHz,
                                                   double sampleRate) noexcept
{
    return Response(c, std::polar(1.0, -2.0 * kPi * frequencyHz / sampleRate));
}

/// One channel's memory for a section, in transposed direct form II.
struct State
{
    double s1 = 0.0;
    double s2 = 0.0;

    double Process(const BiquadCoefficients& c, double input) noexcept
    {
        const double output = c.b0 * input + s1;
        s1 = c.b1 * input - c.a1 * output + s2;
        s2 = c.b2 * input - c.a2 * output;
        return output;
    }

    void Reset() noexcept
    {
        s1 = 0.0;
        s2 = 0.0;
    }
};

/// Coefficients that glide to a new design one step per sample instead of jumping.
///
/// Interpolating the coefficients directly is safe: the pairs (a1, a2) of stable sections
/// fill a triangle, |a2| < 1 and |a1| < 1 + a2, and a triangle is convex, so every point on
/// the straight line between two stable designs is stable as well.
using Ramp = LinearRamp<BiquadCoefficients>;
} // namespace biquad
} // namespace guitarfx
