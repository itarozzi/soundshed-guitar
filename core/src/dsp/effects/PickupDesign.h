#pragma once

#include "dsp/effects/PickupTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/**
 * A pickup designed from its construction: what Pickup Match's "Custom Design" choice runs.
 *
 * A passive pickup is a coil (inductance L, resistance R, winding capacitance C) whose eddy
 * currents in magnets, poles and covers act as a resistance Rp across it. Into a load that is
 * still a second-order low-pass:
 *
 *     H(s) = 1 / (L C s^2 + (L / Rsh + R C) s + (1 + R / Rsh)),   Rsh = Rload || Rp
 *
 * so a design comes out as the resonance, Q and level every other pickup has.
 *
 * Only what measurements support is modelled. The coefficients are fits to 397 measured
 * pickups: L, R, C, gauge, magnet, gauss and damping parts, back-tested by predicting each
 * pickup with itself left out (private research notes, research/pickup-design).
 * - Inductance grows with wire length (DC resistance / the gauge's ohms per metre) to the
 *   power 1.12, from a per-type intercept, with ceramic magnets adding about 19% and A2/A3
 *   about 4-7% over A5. Median error 7.5%.
 * - Winding capacitance cannot be predicted from construction (18% error whatever is
 *   known), so it is the type's median, with Capacitance Trim standing for insulation and
 *   wind pattern.
 * - Eddy loss takes about 3.4 dB off the peak. One shunt per type brings the peak height to
 *   0.7 dB median.
 * - Together these place the loaded peak to about 1 semitone median, against 6-7 semitones of
 *   spread within a type.
 *
 * Covers, wiring and magnets beyond inductance are thinner:
 * - A brass cover's shunt comes from two measured pickups. Nickel silver showed no measurable
 *   damping in three.
 * - Series, parallel and split humbuckers are circuit maths that assume two matched coils
 *   with little coupling between them.
 * - Magnet grade changes inductance only. Pole gauss swings the other way between types
 *   (ceramic reads lower than A5 on Strats and three times higher on PAFs), so output is left
 *   to the turns and the Output trim.
 *
 * The response is placed on the catalogue's scale with a reference load (760 pF, 114 kOhm)
 * calibrated once, so that a median design of each type lands on the matching generic class.
 * Strat, Tele and PAF land within 0.8 semitone and 8% Q. P-90s are left out: the two sources
 * disagree by 5.6 semitones, on 5-8 pickups each.
 */
namespace guitarfx::pickup_match
{
enum class DesignType : std::size_t
{
    StratSingleCoil,
    TeleSingleCoil,
    PafHumbucker,
    Count
};

enum class DesignMagnet : std::size_t
{
    Alnico2,
    Alnico3,
    Alnico5,
    Ceramic,
    Count
};

enum class DesignCover : std::size_t
{
    None,
    NickelSilver,
    Brass,
    Count
};

enum class DesignWiring : std::size_t
{
    Series,
    Parallel,
    SingleCoil,
    Count
};

inline constexpr int kMinGauge = 42;
inline constexpr int kMaxGauge = 44;

/// One construction type's fitted constants.
struct DesignTypeSpec
{
    const char* label;
    const char* detail;         ///< what the picker says under it
    double logInductanceAtUnit; ///< ln L at 1 m of wire, A5 magnet
    double medianWireLengthM;   ///< the measured median, which levelDb refers to
    double capacitancePf;       ///< the measured median winding capacitance
    double eddyShuntOhms;       ///< the fitted median eddy-loss shunt
    double levelDb;             ///< output of the median design, on the catalogue's scale
    bool humbucker;
};

inline constexpr std::array<DesignTypeSpec, static_cast<std::size_t>(DesignType::Count)> kDesignTypes = {{
    // levelDb: the generic classes' own, so a median design converts to them without a jump:
    // single coils between the neck and middle classes, Teles the bridge class, PAFs between
    // the two humbucker classes.
    {"Strat-Style Single Coil", "Typical 6.4 kΩ neck, 7.0 kΩ bridge (42 AWG)", -6.9779, 1191.0, 118.0, 439.4e3, 0.4,
     false},
    {"Tele-Style Single Coil", "Typical 7.4 kΩ neck or bridge (42 AWG)", -6.9962, 1199.0, 168.0, 232.8e3, 2.1, false},
    {"PAF-Style Humbucker", "Typical 7.6 kΩ neck, 8.6 kΩ bridge (42 AWG)", -6.7193, 1500.0, 125.0, 181.1e3, 8.6, true},
}};

inline constexpr double kInductanceLengthExponent = 1.1231;

/// ln L offsets against A5 (the reference the intercepts carry).
inline constexpr std::array<double, static_cast<std::size_t>(DesignMagnet::Count)> kMagnetLogInductance = {
    0.0371, 0.0713, 0.0, 0.172};

/// The shunt a brass cover adds; nickel silver adds none measurable.
inline constexpr double kBrassCoverShuntOhms = 15.1e3;

/// The catalogue's reference load, calibrated so median designs land on the generic classes.
inline constexpr double kReferenceLoadOhms = 114.2e3;
inline constexpr double kReferenceLoadPf = 760.0;

inline constexpr double kCopperResistivity = 1.68e-8; // ohm metres

/// Ohms per metre of copper magnet wire of an AWG gauge.
[[nodiscard]] inline double OhmsPerMetre(int gauge) noexcept
{
    const double diameter = 0.127e-3 * std::pow(92.0, (36.0 - gauge) / 39.0);
    return kCopperResistivity / (0.25 * 3.14159265358979323846 * diameter * diameter);
}

/// What a design is made of.
struct DesignSpec
{
    DesignType type = DesignType::StratSingleCoil;
    double resistanceOhms = 6500.0; ///< DC resistance of the whole pickup, as measured
    int gauge = 42;
    DesignMagnet magnet = DesignMagnet::Alnico5;
    DesignCover cover = DesignCover::None;
    DesignWiring wiring = DesignWiring::Series; ///< humbuckers only
    double capacitanceTrim = 0.0;               ///< -0.5..0.5: the winding capacitance, +/- this fraction
};

/// The electrical pickup a design comes out as, and its response into the reference load.
struct DesignResult
{
    double wireLengthM = 0.0;
    double inductanceH = 0.0;
    double resistanceOhms = 0.0;
    double capacitancePf = 0.0;
    double eddyShuntOhms = 0.0;
    double resonanceHz = 0.0;
    double q = 0.0;
    double levelDb = 0.0;
};

namespace detail
{
struct Circuit
{
    double resonanceHz;
    double q;
    double dcGain;
};

/// Series R and L into (C || shunt): resonance, Q and DC gain of the second-order low-pass.
[[nodiscard]] inline Circuit Solve(double resistance, double inductance, double capacitancePf, double shunt) noexcept
{
    const double capacitance = (capacitancePf + kReferenceLoadPf) * 1.0e-12;
    const double a2 = inductance * capacitance;
    const double a1 = inductance / shunt + resistance * capacitance;
    const double a0 = 1.0 + resistance / shunt;
    return {std::sqrt(a0 / a2) / (2.0 * 3.14159265358979323846), std::sqrt(a0 * a2) / a1, 1.0 / a0};
}

[[nodiscard]] inline double Parallel(double a, double b) noexcept
{
    return 1.0 / (1.0 / a + 1.0 / b);
}
} // namespace detail

[[nodiscard]] inline DesignResult DesignPickup(const DesignSpec& spec) noexcept
{
    const auto& type = kDesignTypes[static_cast<std::size_t>(spec.type)];
    const int gauge = std::clamp(spec.gauge, kMinGauge, kMaxGauge);
    const double resistance = std::max(spec.resistanceOhms, 100.0);

    DesignResult result;
    result.wireLengthM = resistance / OhmsPerMetre(gauge);
    double inductance = std::exp(type.logInductanceAtUnit + kInductanceLengthExponent * std::log(result.wireLengthM) +
                                 kMagnetLogInductance[static_cast<std::size_t>(spec.magnet)]);
    double capacitance = type.capacitancePf * (1.0 + std::clamp(spec.capacitanceTrim, -0.5, 0.5));
    double shunt = type.eddyShuntOhms;
    double levelDb = type.levelDb + 20.0 * std::log10(result.wireLengthM / type.medianWireLengthM);
    double coilResistance = resistance;

    if (spec.cover == DesignCover::Brass)
    {
        shunt = detail::Parallel(shunt, kBrassCoverShuntOhms);
    }

    // Two matched coils with little coupling: one coil is half of each, with twice the
    // capacitance (two equal capacitances in series halve), and its eddy shunt scales with
    // its impedance. Parallel is the two halves side by side. Either way the voltage is one
    // coil's, 6 dB below the pair in series.
    if (type.humbucker && spec.wiring != DesignWiring::Series)
    {
        const double scale = spec.wiring == DesignWiring::Parallel ? 0.25 : 0.5;
        inductance *= scale;
        coilResistance *= scale;
        capacitance /= scale;
        shunt *= scale;
        levelDb -= 6.0206;
    }

    const auto circuit =
        detail::Solve(coilResistance, inductance, capacitance, detail::Parallel(kReferenceLoadOhms, shunt));
    // The level the median design was calibrated at included its own loading; this one's differs.
    const double medianResistance = type.medianWireLengthM * OhmsPerMetre(42);
    const double medianInductance =
        std::exp(type.logInductanceAtUnit + kInductanceLengthExponent * std::log(type.medianWireLengthM));
    const auto median = detail::Solve(medianResistance, medianInductance, type.capacitancePf,
                                      detail::Parallel(kReferenceLoadOhms, type.eddyShuntOhms));

    result.inductanceH = inductance;
    result.resistanceOhms = coilResistance;
    result.capacitancePf = capacitance;
    result.eddyShuntOhms = shunt;
    result.resonanceHz = circuit.resonanceHz;
    result.q = circuit.q;
    result.levelDb = levelDb + 20.0 * std::log10(circuit.dcGain / median.dcGain);
    return result;
}
} // namespace guitarfx::pickup_match
