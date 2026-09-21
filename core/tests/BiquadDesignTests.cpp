#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"
#include "dsp/LinearRamp.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// BiquadDesign.h is the one copy of the RBJ cookbook the effects share. These checks hold
// each design to the shape its name promises and its Response() to what running it does.

namespace
{
using guitarfx::BiquadCoefficients;
namespace bq = guitarfx::biquad;

constexpr double kSampleRate = 48000.0;
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

bool Near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

double ResponseDb(const BiquadCoefficients& c, double hz)
{
    return 20.0 * std::log10(std::abs(bq::Response(c, hz, kSampleRate)));
}

/// Steady-state gain of a sine through the section, measured by running it.
double MeasuredDb(const BiquadCoefficients& c, double hz)
{
    bq::State state;
    double inputPower = 0.0, outputPower = 0.0;

    for (int i = 0; i < 48000; ++i)
    {
        const double x = std::sin(2.0 * kPi * hz * i / kSampleRate);
        const double y = state.Process(c, x);

        if (i >= 9600)
        {
            inputPower += x * x;
            outputPower += y * y;
        }
    }

    return 10.0 * std::log10(outputPower / inputPower);
}

void TestShapes()
{
    const BiquadCoefficients lowPass = bq::LowPass(1000.0, bq::kButterworthQ, kSampleRate);
    Check(Near(ResponseDb(lowPass, 20.0), 0.0, 0.01), "low-pass passes the lows");
    Check(Near(ResponseDb(lowPass, 1000.0), -3.01, 0.05), "Butterworth low-pass is -3 dB at its corner");
    Check(ResponseDb(lowPass, 10000.0) < -35.0, "low-pass rejects the highs");

    const BiquadCoefficients highPass = bq::HighPass(1000.0, bq::kButterworthQ, kSampleRate);
    Check(Near(ResponseDb(highPass, 20000.0), 0.0, 0.05), "high-pass passes the highs");
    Check(Near(ResponseDb(highPass, 1000.0), -3.01, 0.05), "Butterworth high-pass is -3 dB at its corner");

    Check(Near(ResponseDb(bq::Peaking(2000.0, 1.0, 6.0, kSampleRate), 2000.0), 6.0, 0.01), "bell peaks at its gain");
    Check(Near(ResponseDb(bq::LowShelf(200.0, bq::kButterworthQ, -9.0, kSampleRate), 20.0), -9.0, 0.05),
          "low shelf reaches its gain below the corner");
    Check(Near(ResponseDb(bq::HighShelf(3000.0, bq::kButterworthQ, 4.0, kSampleRate), 20000.0), 4.0, 0.1),
          "high shelf reaches its gain above the corner");
}

void TestIdentityAndConstants()
{
    for (const BiquadCoefficients& c :
         {bq::Peaking(500.0, 2.0, 0.0, kSampleRate), bq::LowShelf(100.0, 0.7, 0.0, kSampleRate),
          bq::HighShelf(5000.0, 0.7, 0.0, kSampleRate)})
    {
        Check(c.b0 == 1.0 && c.b1 == 0.0 && c.b2 == 0.0 && c.a1 == 0.0 && c.a2 == 0.0,
              "a 0 dB bell or shelf is exactly the identity");
    }

    Check(Near(bq::ButterworthSectionQ(6, 0), 0.5176380902, 1e-9) &&
              Near(bq::ButterworthSectionQ(6, 1), 0.7071067812, 1e-9) &&
              Near(bq::ButterworthSectionQ(6, 2), 1.9318516526, 1e-9),
          "sixth-order Butterworth section Qs");
    Check(Near(bq::ButterworthSectionQ(2, 0), bq::kButterworthQ, 1e-12), "second-order Butterworth Q");

    for (const double gainDb : {-12.0, 3.0, 12.0})
    {
        Check(Near(bq::ShelfQFromSlope(1.0, gainDb), bq::kButterworthQ, 1e-12), "slope 1 is Q = 1/sqrt(2)");
    }
}

void TestResponseMatchesProcessing()
{
    const std::vector<BiquadCoefficients> designs = {
        bq::LowPass(3000.0, 0.52, kSampleRate),          bq::HighPass(80.0, 0.707, kSampleRate),
        bq::Peaking(140.0, 0.6, 10.0, kSampleRate),      bq::LowShelf(200.0, 0.707, 3.0, kSampleRate),
        bq::HighShelf(3000.0, 0.707, -9.0, kSampleRate),
    };

    for (const auto& design : designs)
    {
        for (const double hz : {100.0, 1000.0, 5000.0})
        {
            Check(Near(ResponseDb(design, hz), MeasuredDb(design, hz), 0.05), "Response() agrees with Process()");
        }
    }
}

void TestNyquistClamp()
{
    // Designed at 6.3 kHz but run at 8 kHz: without the clamp the poles leave the unit circle.
    for (const BiquadCoefficients& c : {bq::Peaking(6300.0, 1.5, 9.0, 8000.0), bq::LowPass(8000.0, 1.93, 11025.0),
                                        bq::HighShelf(7000.0, 0.7, 6.0, 8000.0)})
    {
        Check(std::abs(c.a2) < 1.0 && std::abs(c.a1) < 1.0 + c.a2, "a design past Nyquist is clamped to a stable one");
    }
}

void TestRamps()
{
    guitarfx::LinearRamp<double> gain;
    gain.target = 0.3;
    gain.Begin(7);

    for (int i = 0; i < 6; ++i)
    {
        gain.Advance();
    }

    gain.Finish();
    Check(gain.current == 0.3, "a ramp lands exactly on its target");

    // Sweeping between two extreme but stable designs stays stable all the way.
    bq::Ramp ramp;
    ramp.current = bq::LowPass(40.0, 8.0, kSampleRate);
    ramp.target = bq::Peaking(18000.0, 0.3, 18.0, kSampleRate);
    ramp.Begin(4096);
    bq::State state;
    std::uint32_t noise = 1u;
    bool finite = true;

    for (int i = 0; i < 4096; ++i)
    {
        noise = noise * 1664525u + 1013904223u;
        const double y = state.Process(ramp.current, static_cast<double>(noise) / 4294967296.0 - 0.5);
        finite = finite && guitarfx::IsFinite(y) && std::abs(y) < 1000.0;
        ramp.Advance();
    }

    Check(finite, "a coefficient ramp between stable designs stays stable");
}
} // namespace

int main()
{
    TestShapes();
    TestIdentityAndConstants();
    TestResponseMatchesProcessing();
    TestNyquistClamp();
    TestRamps();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "BiquadDesignTests passed\n";
    return 0;
}
