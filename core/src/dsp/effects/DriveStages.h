#pragma once

#include "dsp/LevelTargets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <tuple>

namespace guitarfx::drive
{
/**
 * Building blocks for the drive pedals (Overdrive, Distortion, Fuzz): the analog stages the
 * classic circuits are made of, reduced to what shapes their sound.
 *
 * Signals are in volts. A pedal clips where its diodes or transistors do, at a fraction of a
 * volt, so the level a guitar arrives at decides how hard it clips. The app's nominal
 * operating level stands for a typical guitar: kGuitarVoltsAtNominal RMS, a humbucker
 * strummed firmly. A signal 12 dB below nominal is then a gently played single coil, and a
 * pedal responds to it the way the hardware would.
 */
inline constexpr double kPi = 3.14159265358979323846;

/// RMS volts that a signal at the nominal operating level stands for.
inline constexpr double kGuitarVoltsAtNominal = 0.25;

/// Volts per unit of sample value, for the given nominal operating level.
[[nodiscard]] inline double VoltsPerUnit(double nominalLevelDbfs) noexcept
{
    return kGuitarVoltsAtNominal / DbToLinearGain(SanitizeNominalOperatingLevelDbfs(nominalLevelDbfs));
}

[[nodiscard]] inline double DbToGain(double db) noexcept
{
    return std::pow(10.0, db * 0.05);
}

/// A gain between `minDb` and `maxDb`, even in dB across `amount` in [0, 1]: the audio-taper
/// pot every one of these pedals uses for its gain.
[[nodiscard]] inline double TaperedGain(double minDb, double maxDb, double amount) noexcept
{
    return DbToGain(minDb + (maxDb - minDb) * std::clamp(amount, 0.0, 1.0));
}

/// The factor a centre-detented character knob scales a corner frequency by: 1 at 0.5, and
/// `octaves` octaves either way at the ends. Positive `octaves` raise the corner as the knob
/// turns up.
[[nodiscard]] inline double KnobOctaves(double knob, double octaves) noexcept
{
    return std::exp2((std::clamp(knob, 0.0, 1.0) - 0.5) * 2.0 * octaves);
}

/// Resistance of an audio-taper (A) pot of `ohms` at rotation `amount`: 10% at half travel.
[[nodiscard]] inline double AudioTaperOhms(double ohms, double amount) noexcept
{
    const double a = std::clamp(amount, 0.0, 1.0);
    return ohms * (std::pow(10.0, 2.0 * a) - 1.0) / 99.0;
}

/// The corner frequency of an RC section.
[[nodiscard]] inline double RcHz(double ohms, double farads) noexcept
{
    return 1.0 / (2.0 * kPi * std::max(ohms, 1.0e-3) * std::max(farads, 1.0e-18));
}

/// The one-pole coefficient for `hz` at `sampleRate`, in the topology-preserving
/// (trapezoidal) form: prewarped, so the corner lands where the analog one does, and stable
/// however the coefficient moves. The corner is held below 0.45 of the rate.
[[nodiscard]] inline double OnePoleCoefficient(double hz, double sampleRate) noexcept
{
    const double f = std::clamp(hz, 1.0, 0.45 * sampleRate);
    const double g = std::tan(kPi * f / sampleRate);
    return g / (1.0 + g);
}

/// One channel's memory for a trapezoidal one-pole.
struct OnePole
{
    double state = 0.0;

    double LowPass(double coefficient, double input) noexcept
    {
        const double v = (input - state) * coefficient;
        const double output = v + state;
        state = output + v;
        return output;
    }

    double HighPass(double coefficient, double input) noexcept
    {
        return input - LowPass(coefficient, input);
    }

    void Reset() noexcept
    {
        state = 0.0;
    }
};

/// How sharply a clipping element bends at its threshold V. Each is clean for small signals
/// (slope 1 and no curvature at zero) and approaches V.
enum class Knee
{
    Gradual, ///< (2V/pi) atan(pi x / 2V): germanium, a MOSFET, a transistor easing into saturation
    Soft,    ///< x / sqrt(1 + (x/V)^2): silicon diodes and LEDs
    Hard     ///< a flat limit at V: an op-amp's rails, a transistor cut off
};

inline constexpr double kHalfPi = 0.5 * kPi;

/// Memory for one antialiased clipping site: its previous input and that input's integral.
/// Until the first sample there is no previous input, and a biased stage must not average
/// from zero to its bias point: that would be a click on every reset.
struct AntialiasMemory
{
    double input = 0.0;
    double integral = 0.0;
    bool primed = false;
};

/// Below this input step the antialiased form divides by too little, and the curve is
/// evaluated at the midpoint instead.
inline constexpr double kAntialiasMinStep = 1.0e-7;

/**
 * A clipping element: a knee at a threshold on each side.
 *
 * The slope at zero is 1 from both sides whatever the two thresholds are, so a quiet signal
 * passes clean and an asymmetric clipper only adds even harmonics once it clips, as diodes
 * do.
 *
 * Every knee has a closed-form integral, which is what Antialiased() needs: first-order
 * antiderivative antialiasing (Parker, Zavalishin and Le Bivic, DAFx 2016) outputs the
 * curve's average over the straight line between successive inputs, instead of its value at
 * the sample. A high-gain signal crosses the knee in a fraction of a sample even at 8x, so a
 * plain sample of the curve is a square wave's edge, and its harmonics fold back across the
 * audio band. The average is the edge band-limited to one sample, for the price of a
 * division.
 */
struct ClipCurve
{
    double positive = 0.6;
    double negative = 0.6;
    Knee positiveKnee = Knee::Soft;
    Knee negativeKnee = Knee::Soft;

    [[nodiscard]] double Apply(double x) const noexcept
    {
        return x >= 0.0 ? Bend(x, positive, positiveKnee) : -Bend(-x, negative, negativeKnee);
    }

    /// The integral of Apply from 0 to x.
    [[nodiscard]] double Integral(double x) const noexcept
    {
        return x >= 0.0 ? Area(x, positive, positiveKnee) : Area(-x, negative, negativeKnee);
    }

    /// Apply(x + bias), antialiased, less Apply(bias): a stage biased away from centre, re-
    /// centred so silence stays silence. Push the bias toward one knee and small signals land
    /// on it: the starved, gated sputter of a transistor fuzz run cold.
    [[nodiscard]] double Antialiased(double x, AntialiasMemory& memory, double bias = 0.0) const noexcept
    {
        const double shifted = x + bias;
        const double integral = Integral(shifted);

        if (!memory.primed)
        {
            memory = {shifted, integral, true};
            return Apply(shifted) - Apply(bias);
        }

        const double step = shifted - memory.input;
        const double output = std::fabs(step) > kAntialiasMinStep ? (integral - memory.integral) / step
                                                                  : Apply(0.5 * (shifted + memory.input));
        memory.input = shifted;
        memory.integral = integral;
        return bias == 0.0 ? output : output - Apply(bias);
    }

  private:
    /// The curve for x >= 0.
    [[nodiscard]] static double Bend(double x, double threshold, Knee knee) noexcept
    {
        switch (knee)
        {
        case Knee::Gradual: {
            const double a = kHalfPi / threshold;
            return std::atan(a * x) / a;
        }

        case Knee::Soft: {
            const double u = x / threshold;
            return x / std::sqrt(1.0 + u * u);
        }

        default:
            return std::min(x, threshold);
        }
    }

    /// Its integral from 0, for x >= 0. Odd curves have even integrals, so the negative side
    /// uses the same formula on -x.
    [[nodiscard]] static double Area(double x, double threshold, Knee knee) noexcept
    {
        switch (knee)
        {
        case Knee::Gradual: {
            const double a = kHalfPi / threshold;
            const double ax = a * x;
            return (x * std::atan(ax) - 0.5 * std::log1p(ax * ax) / a) / a;
        }

        case Knee::Soft: {
            const double u = x / threshold;
            return threshold * threshold * (std::sqrt(1.0 + u * u) - 1.0);
        }

        default:
            return x <= threshold ? 0.5 * x * x : threshold * (x - 0.5 * threshold);
        }
    }
};

/// The full-wave rectifier an octave fuzz is built on, with its corner at zero rounded over
/// `knee` volts as a germanium diode's is: sqrt(x^2 + k^2) - k. Antialiased like ClipCurve.
[[nodiscard]] inline double RectifyAntialiased(double x, double knee, AntialiasMemory& memory) noexcept
{
    const auto integral = [knee](double v) {
        const double root = std::sqrt(v * v + knee * knee);
        return 0.5 * (v * root + knee * knee * std::asinh(v / knee)) - knee * v;
    };
    const double area = integral(x);
    const double step = x - memory.input;
    double output = 0.0;

    if (std::fabs(step) > kAntialiasMinStep)
    {
        output = (area - memory.integral) / step;
    }
    else
    {
        const double mid = 0.5 * (x + memory.input);
        output = std::sqrt(mid * mid + knee * knee) - knee;
    }

    memory.input = x;
    memory.integral = area;
    return output;
}

/// The Clipping switch every overdrive and distortion model offers. Index order is stored in
/// presets: append, never reorder.
enum class ClipChoice
{
    Stock,
    Silicon,
    Asymmetric,
    Led,
    Germanium,
    Mosfet,
    Open,
    Count
};

inline constexpr const char* kClipChoiceLabels[] = {"Stock",     "Silicon", "Asymmetric", "LED",
                                                    "Germanium", "MOSFET",  "Open"};
static_assert(std::size(kClipChoiceLabels) == static_cast<std::size_t>(ClipChoice::Count));

/// The curve a Clipping choice puts where the model's diodes are. Stock is the model's own.
[[nodiscard]] inline ClipCurve ClipFor(ClipChoice choice, const ClipCurve& stock) noexcept
{
    switch (choice)
    {
    case ClipChoice::Silicon:
        return {0.6, 0.6, Knee::Soft, Knee::Soft};

    case ClipChoice::Asymmetric:
        // Two diodes one way and one the other, the SD-1's arrangement.
        return {1.2, 0.6, Knee::Soft, Knee::Soft};

    case ClipChoice::Led:
        return {1.7, 1.7, Knee::Soft, Knee::Soft};

    case ClipChoice::Germanium:
        return {0.32, 0.32, Knee::Gradual, Knee::Gradual};

    case ClipChoice::Mosfet:
        return {0.85, 0.85, Knee::Gradual, Knee::Gradual};

    case ClipChoice::Open:
        // No diodes: the op-amp swings to within about 0.5 V of a 9 V supply's rails.
        return {4.0, 4.0, Knee::Hard, Knee::Hard};

    default:
        return stock;
    }
}

/// How much quieter or louder a diode choice leaves a pedal than its stock diodes, undone so
/// that the switch changes the character rather than the volume. A pedal that is fully
/// saturated gets louder in proportion to the threshold (exponent 1); one that sums its clean
/// signal back in, or clips again later, much less. Each model's exponent is a least-squares
/// fit to its measured level change across the six choices at default drive.
[[nodiscard]] inline double ClipLevelCompensation(const ClipCurve& stock, const ClipCurve& chosen,
                                                  double exponent) noexcept
{
    const double stockLevel = stock.positive + stock.negative;
    const double chosenLevel = chosen.positive + chosen.negative;
    return std::pow(stockLevel / chosenLevel, exponent);
}

/// Makeup gain, in dB, at nine evenly spaced drive settings from 0 to 1, interpolated between
/// them. Each model's table is measured, not designed: it is what brings a guitar phrase at
/// the nominal operating level back to bypass loudness (K-weighted, as a loudness meter reads
/// it) with the other knobs at their defaults.
using TrimTable = std::array<double, 9>;

[[nodiscard]] inline double InterpolateTrimDb(const TrimTable& table, double drive) noexcept
{
    constexpr std::size_t kLast = std::tuple_size_v<TrimTable> - 1;
    const double position = std::clamp(drive, 0.0, 1.0) * static_cast<double>(kLast);
    const auto index = std::min<std::size_t>(static_cast<std::size_t>(position), kLast - 1);
    const double fraction = position - static_cast<double>(index);
    return table[index] + (table[index + 1] - table[index]) * fraction;
}
} // namespace guitarfx::drive
