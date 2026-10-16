#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace guitarfx::analog
{
/**
 * Passive RC networks, solved exactly and turned into digital filters.
 *
 * An amp's tone comes as much from its passive networks as from its tubes: the tone stack,
 * a volume pot with its bright cap, the resistor and cap that couple one stage to the next.
 * Each of those is a handful of resistors and capacitors whose behaviour follows from the
 * part values, so it can be computed rather than approximated by a shelf or a peak.
 *
 * A Network lists its parts between numbered nodes. Node 0 is ground and node 1 the input,
 * driven by an ideal voltage source (a cathode follower, or a plate seen through its own
 * resistance, which is then a part of the network). TransferFunction() solves the network by
 * nodal analysis for the transfer function to the output node, as polynomials in s: exact,
 * whatever the parts are, with every interaction between them (a pot loading the next one,
 * the tone stack's knobs moving each other's corners) included.
 *
 * It works by evaluating the nodal determinant and the output's cofactor (Cramer's rule) at
 * points spread around a circle in the s-plane and reading their polynomial coefficients
 * back with a discrete Fourier transform. Both are polynomials of degree at most the number
 * of unknown nodes, so kMaxNodes points recover them exactly, up to rounding. The circle's
 * radius is the network's own typical rate, the geometric mean of 1/(RC) over its parts,
 * which keeps every coefficient well conditioned.
 *
 * Nothing here allocates, so a network can be re-solved on the audio thread when a pot moves.
 */
inline constexpr std::size_t kMaxNodes = 8; ///< including ground and the input
inline constexpr std::size_t kMaxElements = 16;
inline constexpr std::size_t kMaxOrder = kMaxNodes - 2;

/// A resistance at or below this is a short: a pot turned fully one way leaves a 0 ohm
/// segment, which nodal analysis cannot hold (infinite conductance), and even a milliohm
/// next to a megohm loses the coefficients to rounding. Shorted nodes are merged instead.
inline constexpr double kShortOhms = 1.0e-3;

enum class Part
{
    Resistor,
    Capacitor
};

struct Element
{
    Part part = Part::Resistor;
    std::size_t a = 0;
    std::size_t b = 0;
    double value = 0.0; ///< ohms or farads
};

struct Network
{
    std::array<Element, kMaxElements> elements{};
    std::size_t elementCount = 0;
    std::size_t nodeCount = 2; ///< ground, input, then each internal node
    std::size_t output = 1;

    /// Adds a resistor between nodes `a` and `b`, growing the node count to fit.
    void Resistor(std::size_t a, std::size_t b, double ohms) noexcept
    {
        Add({Part::Resistor, a, b, std::max(ohms, 0.0)});
    }

    void Capacitor(std::size_t a, std::size_t b, double farads) noexcept
    {
        Add({Part::Capacitor, a, b, std::max(farads, 0.0)});
    }

    /// A pot from `top` to `bottom` with its wiper on `wiper`, turned to `position` of its
    /// resistance measured from the bottom (0: wiper at the bottom end).
    void Pot(std::size_t top, std::size_t wiper, std::size_t bottom, double ohms, double position) noexcept
    {
        const double p = std::clamp(position, 0.0, 1.0);
        Resistor(top, wiper, ohms * (1.0 - p));
        Resistor(wiper, bottom, ohms * p);
    }

  private:
    void Add(const Element& element) noexcept
    {
        if (elementCount >= kMaxElements || element.a >= kMaxNodes || element.b >= kMaxNodes)
        {
            return;
        }

        elements[elementCount++] = element;
        nodeCount = std::max({nodeCount, element.a + 1, element.b + 1});
    }
};

/// H(s) = (num[0] + num[1] s + ...) / (den[0] + den[1] s + ...), with den[0] == 1 for any
/// network that passes DC or blocks it through a resistor to ground.
struct Polynomials
{
    std::array<double, kMaxOrder + 1> num{};
    std::array<double, kMaxOrder + 1> den{};
    std::size_t order = 0;
};

namespace detail
{
using Complex = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

/// det(m) by Gaussian elimination with partial pivoting; `m` is destroyed.
[[nodiscard]] inline Complex Determinant(std::array<std::array<Complex, kMaxNodes>, kMaxNodes>& m,
                                         std::size_t n) noexcept
{
    Complex det = 1.0;

    for (std::size_t col = 0; col < n; ++col)
    {
        std::size_t pivot = col;

        for (std::size_t row = col + 1; row < n; ++row)
        {
            if (std::abs(m[row][col]) > std::abs(m[pivot][col]))
            {
                pivot = row;
            }
        }

        if (std::abs(m[pivot][col]) == 0.0)
        {
            return 0.0;
        }

        if (pivot != col)
        {
            std::swap(m[pivot], m[col]);
            det = -det;
        }

        det *= m[col][col];

        for (std::size_t row = col + 1; row < n; ++row)
        {
            const Complex factor = m[row][col] / m[col][col];

            for (std::size_t k = col; k < n; ++k)
            {
                m[row][k] -= factor * m[col][k];
            }
        }
    }

    return det;
}

/// The admittance of one part at complex frequency s.
[[nodiscard]] inline Complex Admittance(const Element& element, Complex s) noexcept
{
    return element.part == Part::Resistor ? Complex(1.0 / element.value) : s * element.value;
}

/// The geometric mean of 1/(RC) over the network's parts: the radius the coefficients are
/// recovered at.
[[nodiscard]] inline double TypicalRate(const Network& network) noexcept
{
    double logR = 0.0;
    double logC = 0.0;
    int resistors = 0;
    int capacitors = 0;

    for (std::size_t i = 0; i < network.elementCount; ++i)
    {
        const Element& element = network.elements[i];

        if (element.part == Part::Resistor)
        {
            logR += std::log(std::max(element.value, kShortOhms));
            ++resistors;
        }
        else if (element.value > 0.0)
        {
            logC += std::log(element.value);
            ++capacitors;
        }
    }

    if (resistors == 0 || capacitors == 0)
    {
        return 1.0;
    }

    return std::exp(-(logR / resistors + logC / capacitors));
}

/// The nodal determinant and the output's Cramer numerator at complex frequency s. Unknowns
/// are the node voltages from node 2 up; the input is held at 1 V.
inline void Solve(const Network& network, Complex s, Complex& determinant, Complex& numerator) noexcept
{
    const std::size_t unknowns = network.nodeCount - 2;
    std::array<std::array<Complex, kMaxNodes>, kMaxNodes> matrix{};
    std::array<Complex, kMaxNodes> source{};

    for (std::size_t i = 0; i < network.elementCount; ++i)
    {
        const Element& element = network.elements[i];
        const Complex y = Admittance(element, s);

        // Node index n maps to row n - 2; ground (0) and the input (1) are not unknowns.
        const auto stamp = [&](std::size_t from, std::size_t to) {
            if (from < 2)
            {
                return;
            }

            const std::size_t row = from - 2;
            matrix[row][row] += y;

            if (to == 1)
            {
                source[row] += y; // the input's 1 V drives this node through y
            }
            else if (to >= 2)
            {
                matrix[row][to - 2] -= y;
            }
        };

        stamp(element.a, element.b);
        stamp(element.b, element.a);
    }

    auto copy = matrix;
    determinant = Determinant(copy, unknowns);

    const std::size_t column = network.output - 2;

    for (std::size_t row = 0; row < unknowns; ++row)
    {
        matrix[row][column] = source[row];
    }

    numerator = Determinant(matrix, unknowns);
}
} // namespace detail

namespace detail
{
/// `network` with every short (a resistor at or below kShortOhms) closed: the nodes it joins
/// become one, ground and the input keep their numbers, the rest are renumbered from 2, and
/// parts left with both ends on one node are dropped. `inputGrounded` is set when a short
/// joins the input to ground.
[[nodiscard]] inline Network MergeShorts(const Network& network, bool& inputGrounded) noexcept
{
    std::array<std::size_t, kMaxNodes> group{};

    for (std::size_t n = 0; n < kMaxNodes; ++n)
    {
        group[n] = n;
    }

    const auto find = [&group](std::size_t n) {
        while (group[n] != n)
        {
            n = group[n];
        }

        return n;
    };

    for (std::size_t i = 0; i < network.elementCount; ++i)
    {
        const Element& element = network.elements[i];

        if (element.part == Part::Resistor && element.value <= kShortOhms)
        {
            const std::size_t a = find(element.a);
            const std::size_t b = find(element.b);

            // The lower number wins, so ground and the input stay themselves.
            group[std::max(a, b)] = std::min(a, b);
        }
    }

    inputGrounded = find(1) == 0;
    std::array<std::size_t, kMaxNodes> renumber{};
    std::size_t next = 2;

    for (std::size_t n = 0; n < network.nodeCount; ++n)
    {
        const std::size_t root = find(n);
        renumber[n] = root < 2 ? root : (root == n ? next++ : renumber[root]);
    }

    Network merged;

    for (std::size_t i = 0; i < network.elementCount; ++i)
    {
        Element element = network.elements[i];
        element.a = renumber[element.a];
        element.b = renumber[element.b];

        const bool shorted = element.part == Part::Resistor && element.value <= kShortOhms;
        const bool open = element.part == Part::Capacitor && element.value <= 0.0;

        if (element.a != element.b && !shorted && !open)
        {
            merged.elements[merged.elementCount++] = element;
        }
    }

    merged.nodeCount = next;
    merged.output = renumber[network.output];
    return merged;
}
} // namespace detail

/// The network's transfer function from the input to its output node, as polynomials in s
/// normalised so the denominator's constant term is 1 (or its lowest non-zero term, for a
/// network with no DC path to ground at all).
[[nodiscard]] inline Polynomials TransferFunction(const Network& original) noexcept
{
    using detail::Complex;
    Polynomials result;
    result.den[0] = 1.0;
    bool inputGrounded = false;
    const Network network = detail::MergeShorts(original, inputGrounded);

    if (inputGrounded || network.output == 0)
    {
        return result; // silence
    }

    if (network.output == 1)
    {
        result.num[0] = 1.0; // a wire
        return result;
    }

    // Both polynomials have degree at most the number of capacitors, and at most the number
    // of unknown nodes. Sampling exactly one more point than that recovers them exactly.
    std::size_t capacitors = 0;

    for (std::size_t i = 0; i < network.elementCount; ++i)
    {
        capacitors += network.elements[i].part == Part::Capacitor ? 1 : 0;
    }

    const std::size_t bound = std::min({capacitors, network.nodeCount - 2, kMaxOrder});
    const std::size_t points = bound + 1;
    const double radius = detail::TypicalRate(network);
    std::array<Complex, kMaxOrder + 1> determinants{};
    std::array<Complex, kMaxOrder + 1> numerators{};

    for (std::size_t k = 0; k < points; ++k)
    {
        const double angle = 2.0 * detail::kPi * static_cast<double>(k) / static_cast<double>(points);
        detail::Solve(network, std::polar(radius, angle), determinants[k], numerators[k]);
    }

    // The coefficient of s^j, times radius^j, is the j-th DFT bin of the samples. Both
    // polynomials have real coefficients, so the imaginary parts are rounding.
    std::array<double, kMaxOrder + 1> den{};
    std::array<double, kMaxOrder + 1> num{};
    std::array<double, kMaxOrder + 1> scaledDen{};

    for (std::size_t j = 0; j < points; ++j)
    {
        Complex d = 0.0;
        Complex n = 0.0;

        for (std::size_t k = 0; k < points; ++k)
        {
            const double angle = -2.0 * detail::kPi * static_cast<double>(j * k) / static_cast<double>(points);
            const Complex twiddle = std::polar(1.0, angle);
            d += determinants[k] * twiddle;
            n += numerators[k] * twiddle;
        }

        scaledDen[j] = std::abs(d.real()) / static_cast<double>(points);
        const double scale = 1.0 / (static_cast<double>(points) * std::pow(radius, static_cast<double>(j)));
        den[j] = d.real() * scale;
        num[j] = n.real() * scale;
    }

    // A term the network cannot have (its capacitors in series, say) comes out as rounding;
    // trim it from either end against the largest term at the solving radius.
    const double largest = *std::max_element(scaledDen.begin(), scaledDen.begin() + static_cast<long>(points));
    const double floor = 1.0e-10 * largest;
    std::size_t order = bound;

    while (order > 0 && scaledDen[order] <= floor)
    {
        --order;
    }

    std::size_t lowest = 0;

    while (lowest < order && scaledDen[lowest] <= floor)
    {
        ++lowest;
    }

    const double norm = den[lowest];

    if (norm == 0.0)
    {
        return result;
    }

    for (std::size_t j = 0; j <= order; ++j)
    {
        result.den[j] = j < lowest ? 0.0 : den[j] / norm;
        result.num[j] = num[j] / norm;
    }

    result.order = order;
    return result;
}

/// |H| and phase at `hz`, straight from the analog polynomials.
[[nodiscard]] inline std::complex<double> Response(const Polynomials& h, double hz) noexcept
{
    const std::complex<double> s(0.0, 2.0 * detail::kPi * hz);
    std::complex<double> num = 0.0;
    std::complex<double> den = 0.0;

    for (std::size_t j = h.order + 1; j-- > 0;)
    {
        num = num * s + h.num[j];
        den = den * s + h.den[j];
    }

    return num / den;
}

/**
 * A digital IIR of up to kMaxOrder, from analog polynomials by the bilinear transform.
 *
 * Runs in transposed direct form II in double. The networks it carries have their poles at
 * audio frequencies, run at the amp's oversampled rate, so a pole sits at most a few
 * thousandths inside the unit circle; double holds that with margin to spare at any order
 * these networks reach.
 */
struct Iir
{
    std::array<double, kMaxOrder + 1> b{};
    std::array<double, kMaxOrder + 1> a{}; ///< a[0] == 1
    std::size_t order = 0;

    /// Designs from `h` at `sampleRate`. With `prewarpHz` > 0 that frequency lands exactly
    /// where the analog response has it; otherwise the plain bilinear transform (exact at DC,
    /// compressing towards Nyquist).
    void Design(const Polynomials& h, double sampleRate, double prewarpHz = 0.0) noexcept
    {
        const std::size_t n = std::min(h.order, kMaxOrder);
        double c = 2.0 * sampleRate;

        if (prewarpHz > 0.0 && prewarpHz < 0.45 * sampleRate)
        {
            const double w = 2.0 * detail::kPi * prewarpHz;
            c = w / std::tan(w / (2.0 * sampleRate));
        }

        // s = c (1 - z^-1) / (1 + z^-1). Multiplying through by (1 + z^-1)^n, s^k becomes
        // c^k (1 - z^-1)^k (1 + z^-1)^(n - k), whose z^-i coefficients are built below.
        std::array<double, kMaxOrder + 1> bz{};
        std::array<double, kMaxOrder + 1> az{};

        for (std::size_t k = 0; k <= n; ++k)
        {
            std::array<double, kMaxOrder + 1> poly{};
            poly[0] = 1.0;
            std::size_t degree = 0;

            for (std::size_t f = 0; f < n; ++f)
            {
                const double sign = f < k ? -1.0 : 1.0; // (1 - z^-1) for the first k factors

                for (std::size_t i = degree + 1; i-- > 0;)
                {
                    poly[i + 1] += sign * poly[i];
                }

                ++degree;
            }

            const double ck = std::pow(c, static_cast<double>(k));

            for (std::size_t i = 0; i <= n; ++i)
            {
                bz[i] += h.num[k] * ck * poly[i];
                az[i] += h.den[k] * ck * poly[i];
            }
        }

        const double norm = az[0];
        order = n;

        for (std::size_t i = 0; i <= kMaxOrder; ++i)
        {
            b[i] = i <= n ? bz[i] / norm : 0.0;
            a[i] = i <= n ? az[i] / norm : 0.0;
        }
    }

    /// The digital response at `hz`, for tests and analysis.
    [[nodiscard]] std::complex<double> Response(double hz, double sampleRate) const noexcept
    {
        const std::complex<double> zInv = std::polar(1.0, -2.0 * detail::kPi * hz / sampleRate);
        std::complex<double> num = 0.0;
        std::complex<double> den = 0.0;

        for (std::size_t i = order + 1; i-- > 0;)
        {
            num = num * zInv + b[i];
            den = den * zInv + a[i];
        }

        return num / den;
    }
};

/// One channel's memory for an Iir.
struct IirState
{
    std::array<double, kMaxOrder> z{};

    double Process(const Iir& f, double x) noexcept
    {
        const double y = f.b[0] * x + z[0];

        for (std::size_t i = 1; i < f.order; ++i)
        {
            z[i - 1] = f.b[i] * x - f.a[i] * y + z[i];
        }

        if (f.order > 0)
        {
            z[f.order - 1] = f.b[f.order] * x - f.a[f.order] * y;
        }

        return y;
    }

    void Reset() noexcept
    {
        z.fill(0.0);
    }
};
} // namespace guitarfx::analog
