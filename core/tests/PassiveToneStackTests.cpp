#include "dsp/AnalogNetwork.h"
#include "dsp/PassiveToneStack.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
namespace analog = guitarfx::analog;
namespace ts = guitarfx::tone_stack;

constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

double Db(std::complex<double> h)
{
    return 20.0 * std::log10(std::max(std::abs(h), 1.0e-30));
}

std::vector<double> LogFrequencies(double low, double high, int count)
{
    std::vector<double> hz(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i)
    {
        hz[static_cast<std::size_t>(i)] = low * std::pow(high / low, static_cast<double>(i) / (count - 1));
    }

    return hz;
}

/// The stack's transfer function written out by hand, from Yeh and Smith (DAFx 2006), with
/// t, m and l the pot fractions. Derived independently of the nodal solver, so agreement
/// checks both the solver and the netlist.
std::complex<double> ClosedForm(const ts::Parts& p, double t, double m, double l, double hz)
{
    const double C1 = p.trebleFarads, C2 = p.bassFarads, C3 = p.middleFarads;
    const double R1 = p.trebleOhms, R2 = p.bassOhms, R3 = p.middleOhms, R4 = p.slopeOhms;

    const double b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    const double b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
                      l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
                      (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    const double b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                      m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                      m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                      t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4 +
                      t * l * C1 * C2 * C3 * R1 * R2 * R4;
    const double a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    const double a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) -
                      m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
                      l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
                      (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 +
                       C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
    const double a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
                      m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
                      m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
                      l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;

    const std::complex<double> s(0.0, 2.0 * kPi * hz);
    return (b1 * s + b2 * s * s + b3 * s * s * s) / (1.0 + a1 * s + a2 * s * s + a3 * s * s * s);
}

/// Linear tapers, so a knob position is the pot fraction the closed form takes.
ts::Parts WithLinearTapers(ts::Parts parts)
{
    parts.trebleTaper = ts::Taper::Linear;
    parts.bassTaper = ts::Taper::Linear;
    parts.middleTaper = ts::Taper::Linear;
    return parts;
}

void TestSolverMatchesClosedForm()
{
    const auto frequencies = LogFrequencies(20.0, 20000.0, 61);
    const double positions[] = {0.0, 0.1, 0.5, 0.8, 1.0};
    double worst = 0.0;
    std::string worstAt;

    for (const ts::Parts& base : {ts::kTweedBass, ts::kBritishLead})
    {
        const ts::Parts parts = WithLinearTapers(base);

        for (double t : positions)
        {
            for (double m : positions)
            {
                for (double l : positions)
                {
                    const auto h = ts::Solve(parts, {t, l, m});

                    for (double hz : frequencies)
                    {
                        const double expected = Db(ClosedForm(parts, t, m, l, hz));

                        // At the extremes the stack can be near silent; compare where it is
                        // audible at all.
                        if (const double error = std::abs(Db(analog::Response(h, hz)) - expected);
                            expected > -80.0 && error > worst)
                        {
                            worst = error;
                            worstAt = "t " + std::to_string(t) + " m " + std::to_string(m) + " l " + std::to_string(l) +
                                      " at " + std::to_string(hz) + " Hz, expected " + std::to_string(expected) + " dB";
                        }
                    }
                }
            }
        }
    }

    std::cout << "  solver vs closed form, worst " << worst << " dB (" << worstAt << ")\n";
    Check(worst < 0.01, "the nodal solver reproduces the published tone-stack transfer function");
}

void TestOrderIsThree()
{
    const auto h = ts::Solve(ts::kBritishLead, {});
    Check(h.order == 3, "a three-capacitor stack solves to third order (got " + std::to_string(h.order) + ")");
    Check(std::abs(h.num[0]) < 1.0e-9, "the stack passes no DC");
}

void TestBilinearMatchesAnalog()
{
    constexpr double kOversampledRate = 384000.0;
    const auto frequencies = LogFrequencies(20.0, 16000.0, 49);
    double worst = 0.0;

    for (const ts::Knobs& knobs : {ts::Knobs{0.5, 0.5, 0.5}, ts::Knobs{1.0, 0.0, 1.0}, ts::Knobs{0.2, 1.0, 0.0}})
    {
        const auto h = ts::Solve(ts::kBritishLead, knobs);
        analog::Iir iir;
        iir.Design(h, kOversampledRate);

        for (double hz : frequencies)
        {
            const double analogDb = Db(analog::Response(h, hz));

            if (analogDb > -60.0)
            {
                worst = std::max(worst, std::abs(Db(iir.Response(hz, kOversampledRate)) - analogDb));
            }
        }
    }

    std::cout << "  bilinear at 384 kHz vs analog, worst " << worst << " dB\n";
    Check(worst < 0.05, "the digital stack matches the analog one to 16 kHz at the oversampled rate");
}

void TestPrewarpLandsExactly()
{
    constexpr double kRate = 48000.0;
    const auto h = ts::Solve(ts::kBritishLead, {0.7, 0.5, 0.5});
    analog::Iir iir;
    iir.Design(h, kRate, 6000.0);
    const double error = std::abs(Db(iir.Response(6000.0, kRate)) - Db(analog::Response(h, 6000.0)));
    Check(error < 1.0e-6, "a prewarped design lands its frequency exactly");
}

void TestProcessFollowsResponse()
{
    constexpr double kRate = 192000.0;
    const auto h = ts::Solve(ts::kBritishLead, {0.5, 0.5, 0.5});
    analog::Iir iir;
    iir.Design(h, kRate);
    analog::IirState state;

    for (double hz : {100.0, 700.0, 5000.0})
    {
        state.Reset();
        const int settle = static_cast<int>(kRate * 0.2);
        const int measure = static_cast<int>(kRate * 0.1);
        double sumSquares = 0.0;

        for (int n = 0; n < settle + measure; ++n)
        {
            const double y = state.Process(iir, std::sin(2.0 * kPi * hz * n / kRate));

            if (n >= settle)
            {
                sumSquares += y * y;
            }
        }

        const double measuredDb = 20.0 * std::log10(std::sqrt(2.0 * sumSquares / measure));
        const double expectedDb = Db(iir.Response(hz, kRate));
        Check(std::abs(measuredDb - expectedDb) < 0.02,
              "a steady sine at " + std::to_string(hz) + " Hz comes out at the designed gain");
    }
}

void TestMidScoopAndInteraction()
{
    const auto noon = ts::Solve(ts::kBritishLead, {0.5, 0.5, 0.5});
    double scoop = 0.0;
    double scoopHz = 0.0;

    for (double hz : LogFrequencies(200.0, 2000.0, 41))
    {
        const double db = Db(analog::Response(noon, hz));

        if (scoopHz == 0.0 || db < scoop)
        {
            scoop = db;
            scoopHz = hz;
        }
    }

    const double low = Db(analog::Response(noon, 80.0));
    const double high = Db(analog::Response(noon, 4000.0));
    std::cout << "  British stack at noon: 80 Hz " << low << " dB, scoop " << scoop << " dB at " << scoopHz
              << " Hz, 4 kHz " << high << " dB\n";
    Check(scoop < low - 3.0 && scoop < high - 3.0, "the stack scoops the mids at noon");

    // The middle knob moves the treble region too: no independent bands here.
    const auto midUp = ts::Solve(ts::kBritishLead, {0.5, 0.5, 1.0});
    const auto midDown = ts::Solve(ts::kBritishLead, {0.5, 0.5, 0.0});
    const double trebleShift = Db(analog::Response(midUp, 5000.0)) - Db(analog::Response(midDown, 5000.0));
    Check(trebleShift > 1.0, "turning the middle up also lifts the treble region");

    // Bass and treble still mostly keep to their own ends.
    const auto bassUp = ts::Solve(ts::kBritishLead, {0.5, 1.0, 0.5});
    const auto bassDown = ts::Solve(ts::kBritishLead, {0.5, 0.0, 0.5});
    const auto bassAt = [&](double hz) {
        return Db(analog::Response(bassUp, hz)) - Db(analog::Response(bassDown, hz));
    };
    Check(bassAt(80.0) > 8.0 && std::abs(bassAt(5000.0)) < 0.5, "the bass knob works the lows and leaves the top");

    const auto trebleUp = ts::Solve(ts::kBritishLead, {1.0, 0.5, 0.5});
    const auto trebleDown = ts::Solve(ts::kBritishLead, {0.0, 0.5, 0.5});
    Check(Db(analog::Response(trebleUp, 5000.0)) - Db(analog::Response(trebleDown, 5000.0)) > 8.0,
          "the treble knob works the top");

    // Even dimed, a passive stack cannot fill its own mid dip.
    const auto dimed = ts::Solve(ts::kBritishLead, {1.0, 1.0, 1.0});
    Check(Db(analog::Response(dimed, 700.0)) < Db(analog::Response(dimed, 80.0)) - 3.0,
          "the mid dip survives with every knob at full");
}

void TestLoadLowersLevel()
{
    ts::Parts loaded = ts::kBritishLead;
    loaded.loadOhms = 1.0e6;
    const double unloadedDb = Db(analog::Response(ts::Solve(ts::kBritishLead, {}), 1000.0));
    const double loadedDb = Db(analog::Response(ts::Solve(loaded, {}), 1000.0));
    Check(loadedDb < unloadedDb && loadedDb > unloadedDb - 3.0, "a 1M load pulls the stack's level down a little");
}

void TestNetworkSolverOnKnownRc()
{
    // A plain RC low-pass: 1/(1 + sRC).
    analog::Network network;
    network.Resistor(1, 2, 10.0e3);
    network.Capacitor(2, 0, 10.0e-9);
    network.output = 2;
    const auto h = analog::TransferFunction(network);
    Check(h.order == 1, "an RC low-pass is first order");
    Check(std::abs(h.num[0] - 1.0) < 1.0e-9 && std::abs(h.den[1] - 1.0e-4) < 1.0e-13,
          "an RC low-pass solves to 1/(1 + sRC)");
}
} // namespace

int main()
{
    std::cout << "PassiveToneStackTests\n";
    TestNetworkSolverOnKnownRc();
    TestSolverMatchesClosedForm();
    TestOrderIsThree();
    TestBilinearMatchesAnalog();
    TestPrewarpLandsExactly();
    TestProcessFollowsResponse();
    TestMidScoopAndInteraction();
    TestLoadLowersLevel();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All PassiveToneStack tests passed\n";
    return EXIT_SUCCESS;
}
