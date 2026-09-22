#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
/**
 * Polyphase IIR half-band filters for 2x and 4x oversampling a nonlinearity.
 *
 * The half-band is two parallel chains of first-order allpass sections, one per polyphase
 * branch:
 *
 *     H(z) = 1/2 [ A0(z^2) + z^-1 A1(z^2) ]
 *
 * so each branch runs at the lower of the two rates and costs one multiply per section. The
 * coefficients come from an elliptic design (Valenzuela and Constantinides; the closed form
 * is the one Laurent de Soras' hiir library uses), which makes the ripple equal across the
 * stopband.
 *
 * Why IIR rather than the linear-phase FIR the built-in amp uses: a drive pedal sits in
 * front of an amp that oversamples too, and every linear-phase half-band pair adds its full
 * length of latency. These filters are minimum-phase. Their delay is a few samples at low
 * frequencies and rises only near the band edge, which is no more than an analog pedal's own
 * filters shift phase, and it reports as zero latency.
 *
 * The magnitude of an up-then-down round trip is flat through the passband to within the
 * design's ripple; its phase is not linear, so a dry signal that is to be mixed with the
 * processed one should take the same round trip (see DrivePedal, which mixes at the high
 * rate for exactly that reason).
 */
namespace halfband
{
inline constexpr int kMaxCoefficients = 12;

/// Allpass coefficients for a half-band with the given number of sections, `transition` wide
/// (as a fraction of the higher sample rate; the passband ends at 0.25 - transition), in the
/// order the designer produces them: even indices feed branch 0, odd ones branch 1.
struct Design
{
    std::array<double, kMaxCoefficients> coefficients = {};
    int count = 0;
};

namespace detail
{
inline constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] inline double SumNumerator(double q, int order, int c) noexcept
{
    double result = 0.0;
    double sign = 1.0;

    for (int i = 0; i < 64; ++i)
    {
        const double term = std::pow(q, i * (i + 1)) * std::sin((i * 2 + 1) * c * kPi / order) * sign;
        result += term;
        sign = -sign;

        if (std::fabs(term) < 1.0e-100)
        {
            break;
        }
    }

    return result;
}

[[nodiscard]] inline double SumDenominator(double q, int order, int c) noexcept
{
    double result = 0.0;
    double sign = -1.0;

    for (int i = 1; i < 64; ++i)
    {
        const double term = std::pow(q, i * i) * std::cos(i * 2 * c * kPi / order) * sign;
        result += term;
        sign = -sign;

        if (std::fabs(term) < 1.0e-100)
        {
            break;
        }
    }

    return result;
}
} // namespace detail

/// Designs a half-band with `count` allpass sections (1..kMaxCoefficients).
[[nodiscard]] inline Design DesignHalfBand(int count, double transition) noexcept
{
    using detail::kPi;
    Design design;
    design.count = std::clamp(count, 1, kMaxCoefficients);
    transition = std::clamp(transition, 1.0e-4, 0.4999);

    double k = std::tan((1.0 - transition * 2.0) * kPi / 4.0);
    k *= k;
    const double kksqrt = std::pow(1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e;
    const double e4 = e2 * e2;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    const int order = design.count * 2 + 1;

    for (int index = 0; index < design.count; ++index)
    {
        const int c = index + 1;
        const double numerator = detail::SumNumerator(q, order, c) * std::pow(q, 0.25);
        const double denominator = detail::SumDenominator(q, order, c) + 0.5;
        const double ww = numerator / denominator;
        const double wwsq = ww * ww;
        const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        design.coefficients[static_cast<std::size_t>(index)] = (1.0 - x) / (1.0 + x);
    }

    return design;
}

/// One polyphase branch: a chain of first-order allpass sections (a + z^-1) / (1 + a z^-1)
/// running at the lower rate.
struct Branch
{
    std::array<double, kMaxCoefficients> coefficient = {};
    std::array<double, kMaxCoefficients> previousInput = {};
    std::array<double, kMaxCoefficients> previousOutput = {};
    int count = 0;

    double Process(double input) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            const auto s = static_cast<std::size_t>(i);
            const double output = coefficient[s] * (input - previousOutput[s]) + previousInput[s];
            previousInput[s] = input;
            previousOutput[s] = output;
            input = output;
        }

        return input;
    }

    void Reset() noexcept
    {
        previousInput.fill(0.0);
        previousOutput.fill(0.0);
    }
};

/// One 2x stage: interpolates one sample into two, and decimates two back into one.
/// Up and down paths keep separate memory, so one object serves both directions.
class Stage2x
{
  public:
    void Configure(const Design& design) noexcept
    {
        mUp0 = {};
        mUp1 = {};

        for (int index = 0; index < design.count; ++index)
        {
            Branch& branch = (index % 2 == 0) ? mUp0 : mUp1;
            branch.coefficient[static_cast<std::size_t>(branch.count)] =
                design.coefficients[static_cast<std::size_t>(index)];
            ++branch.count;
        }

        mDown0 = mUp0;
        mDown1 = mUp1;
        Reset();
    }

    void Reset() noexcept
    {
        mUp0.Reset();
        mUp1.Reset();
        mDown0.Reset();
        mDown1.Reset();
        mDownOddHeld = 0.0;
    }

    /// Two output samples, in time order, for one input sample. The gain of 2 that zero
    /// stuffing needs is already in the branches.
    void Up(double input, double& first, double& second) noexcept
    {
        first = mUp0.Process(input);
        second = mUp1.Process(input);
    }

    /// One output sample for two input samples in time order.
    double Down(double first, double second) noexcept
    {
        const double output = 0.5 * (mDown0.Process(first) + mDownOddHeld);
        mDownOddHeld = mDown1.Process(second);
        return output;
    }

  private:
    Branch mUp0;
    Branch mUp1;
    Branch mDown0;
    Branch mDown1;
    double mDownOddHeld = 0.0;
};

/// The passband of every stage ends here, as a fraction of the host rate, before the first
/// stage's transition band begins: 20 kHz at 44.1 kHz.
inline constexpr double kPassbandFraction = 0.4535;

/// 1x, 2x, 4x or 8x oversampling of one channel, built from cascaded Stage2x.
///
/// The first stage, next to the host rate, carries the steep transition from the top of the
/// audio band to the host's Nyquist. Each later stage only has to reject images above the
/// previous stage's Nyquist, ever further from the audio band, so it needs fewer sections.
class Oversampler
{
  public:
    static constexpr int kMaxStages = 3;
    static constexpr int kMaxFactor = 1 << kMaxStages;

    /// Factor 1, 2, 4 or 8.
    void Prepare(int factor) noexcept
    {
        mStageCount = factor >= 8 ? 3 : (factor >= 4 ? 2 : (factor >= 2 ? 1 : 0));

        for (int stage = 0; stage < kMaxStages; ++stage)
        {
            // Each stage's transition, as a fraction of its higher rate, runs from the audio
            // passband edge up to the half-band centre (a quarter of that rate).
            const double passband = kPassbandFraction / static_cast<double>(2 << stage);
            mStages[static_cast<std::size_t>(stage)].Configure(
                DesignHalfBand(kSections[static_cast<std::size_t>(stage)], 0.25 - passband));
        }

        Reset();
    }

    void Reset() noexcept
    {
        for (auto& stage : mStages)
        {
            stage.Reset();
        }
    }

    [[nodiscard]] int Factor() const noexcept
    {
        return 1 << mStageCount;
    }

    /// Writes Factor() samples, in time order.
    void Up(double input, double* output) noexcept
    {
        std::array<double, kMaxFactor> scratch = {};
        output[0] = input;
        int count = 1;

        for (int stage = 0; stage < mStageCount; ++stage)
        {
            std::copy_n(output, count, scratch.data());

            for (int i = 0; i < count; ++i)
            {
                mStages[static_cast<std::size_t>(stage)].Up(scratch[static_cast<std::size_t>(i)], output[2 * i],
                                                            output[2 * i + 1]);
            }

            count *= 2;
        }
    }

    /// Reads Factor() samples, in time order.
    double Down(const double* input) noexcept
    {
        std::array<double, kMaxFactor> scratch = {};
        int count = Factor();
        std::copy_n(input, count, scratch.data());

        for (int stage = mStageCount - 1; stage >= 0; --stage)
        {
            count /= 2;

            for (int i = 0; i < count; ++i)
            {
                scratch[static_cast<std::size_t>(i)] = mStages[static_cast<std::size_t>(stage)].Down(
                    scratch[static_cast<std::size_t>(2 * i)], scratch[static_cast<std::size_t>(2 * i + 1)]);
            }
        }

        return scratch[0];
    }

  private:
    static constexpr std::array<int, kMaxStages> kSections = {10, 4, 3};

    std::array<Stage2x, kMaxStages> mStages = {};
    int mStageCount = 0;
};
} // namespace halfband
} // namespace guitarfx
