#pragma once

#include "dsp/AnalogNetwork.h"
#include <algorithm>
#include <cmath>

namespace guitarfx::tone_stack
{
/**
 * The passive treble/bass/middle tone stack that most classic tube amps share, solved from
 * its parts.
 *
 * Unlike three independent EQ bands, its knobs interact: at noon it already scoops the
 * mids, the middle knob moves the whole curve's level and the bass and treble corners with
 * it, and turning everything down gives near silence rather than flat. Which amp it sounds
 * like is set entirely by the part values, so one topology covers the American and British
 * classics. The circuit (Yeh and Smith, "Discretization of the '59 Fender Bassman Tone
 * Stack", DAFx 2006):
 *
 *     in --C1-- treble pot top, its wiper is the output
 *     in --R4-- slope --C2-- treble pot bottom --bass pot (rheostat)-- middle pot top
 *               slope --C3-- middle pot wiper            middle pot bottom -- ground
 *
 * The middle pot is a divider: turned down, it grounds C3 and leaves its whole resistance
 * under the bass pot, which is why it also moves the bass and treble. (Checked against the
 * paper's closed-form coefficients in PassiveToneStackTests.)
 * driven from a low impedance (a cathode follower, or a plate whose own resistance is folded
 * into R4), optionally loaded by whatever follows (a volume pot and the next grid).
 */
enum class Taper
{
    Linear,
    Audio ///< a log ("A") pot: about 10% of its resistance at half travel
};

/// The fraction of a pot's resistance at `position` of its travel.
[[nodiscard]] inline double TaperFraction(Taper taper, double position) noexcept
{
    const double p = std::clamp(position, 0.0, 1.0);
    return taper == Taper::Audio ? (std::pow(10.0, 2.0 * p) - 1.0) / 99.0 : p;
}

struct Parts
{
    double trebleOhms = 250.0e3;     ///< R1
    double bassOhms = 1.0e6;         ///< R2
    double middleOhms = 25.0e3;      ///< R3
    double slopeOhms = 56.0e3;       ///< R4
    double trebleFarads = 250.0e-12; ///< C1
    double bassFarads = 20.0e-9;     ///< C2
    double middleFarads = 20.0e-9;   ///< C3
    Taper trebleTaper = Taper::Audio;
    Taper bassTaper = Taper::Audio;
    Taper middleTaper = Taper::Audio;
    double loadOhms = 0.0; ///< 0: unloaded
};

/// The '59 tweed bass amp's values, the reference circuit of the paper.
inline constexpr Parts kTweedBass{};

/// The British lead amps' values: a smaller slope resistor and a larger treble cap make it
/// brighter and less scooped, the 1980s master-volume lead heads' stack (220k treble, 1M bass,
/// 22k middle, 33k slope, 470 pF, 22 nF, 22 nF).
inline constexpr Parts kBritishLead{220.0e3, 1.0e6,         22.0e3,       33.0e3,        470.0e-12, 22.0e-9,
                                    22.0e-9, Taper::Linear, Taper::Audio, Taper::Linear, 0.0};

struct Knobs
{
    double treble = 0.5;
    double bass = 0.5;
    double middle = 0.5;
};

/// The stack as a network, with each knob's pot set by its taper.
[[nodiscard]] inline analog::Network Build(const Parts& parts, const Knobs& knobs) noexcept
{
    enum Node : std::size_t
    {
        kGround = 0,
        kInput = 1,
        kTrebleTop = 2,
        kOutput = 3,
        kBassTop = 4,
        kSlope = 5,
        kMiddleTop = 6,
        kMiddleWiper = 7
    };

    analog::Network network;
    network.Capacitor(kInput, kTrebleTop, parts.trebleFarads);
    network.Pot(kTrebleTop, kOutput, kBassTop, parts.trebleOhms, TaperFraction(parts.trebleTaper, knobs.treble));
    network.Resistor(kInput, kSlope, parts.slopeOhms);
    network.Capacitor(kSlope, kBassTop, parts.bassFarads);
    network.Capacitor(kSlope, kMiddleWiper, parts.middleFarads);
    network.Resistor(kBassTop, kMiddleTop, parts.bassOhms * TaperFraction(parts.bassTaper, knobs.bass));
    network.Pot(kMiddleTop, kMiddleWiper, kGround, parts.middleOhms, TaperFraction(parts.middleTaper, knobs.middle));

    if (parts.loadOhms > 0.0)
    {
        network.Resistor(kOutput, kGround, parts.loadOhms);
    }

    network.output = kOutput;
    return network;
}

/// The stack's analog transfer function at these knob settings.
[[nodiscard]] inline analog::Polynomials Solve(const Parts& parts, const Knobs& knobs) noexcept
{
    return analog::TransferFunction(Build(parts, knobs));
}
} // namespace guitarfx::tone_stack
