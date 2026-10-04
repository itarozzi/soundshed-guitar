/**
 * @file WahEnvelopeTests.cpp
 * @brief Tests for the wah's Envelope control, which sweeps the filter with the playing level.
 *
 * These measure what the control claims to do:
 *   - at rest the filter sits at Pedal Position, and a level that reads as 1 opens it to the toe
 *   - the detector rises at the Attack time constant and falls at the Release one
 *   - Sensitivity scales the level read, and the opening stops at the toe
 *   - Pedal Position is the rest the envelope opens from
 *   - the level moves nothing on the Pedal control, and Auto-Engage never switches an envelope wah off
 *   - one detector for both channels: the louder side opens the filter for both
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "WahTestSupport.h"
#include "dsp/effects/WahEffect.h"

namespace
{
using namespace wah_test;

/// A steady level is what the detector sees from a held note, without a rectified sine's ripple.
/// The filter blocks it, so only the detector responds.
std::vector<float> Level(double value, double seconds)
{
    return std::vector<float>(static_cast<std::size_t>(seconds * kSampleRate), static_cast<float>(value));
}

void TestEnvelopeControl()
{
    std::cout << "\nEnvelope control\n";
    const Params envelope =
        Linear({{"control", 1.0}, {"position", 0.0}, {"sensitivity", 0.0}, {"attack", 50.0}, {"release", 500.0}});

    // At rest the filter sits at the heel, and a level that reads as 1 opens it to the toe.
    auto wah = MakeWah(envelope);
    Render(*wah, Silence(0.1));
    Check(Near(wah->GetParam("currentFrequency"), 450.0, 1.0e-6) && wah->GetParam("envelope") == 0.0,
          "silence leaves the filter at the heel", Num(wah->GetParam("currentFrequency"), 1) + " Hz");
    Render(*wah, Level(1.0, 1.0));
    Check(Near(wah->GetParam("currentFrequency"), 2200.0, 1.0e-3) && wah->GetParam("envelope") > 0.999,
          "a full-scale level opens it to the toe", Num(wah->GetParam("currentFrequency"), 1) + " Hz");

    // Attack and Release are the detector's time constants: after one of each it has covered
    // 1 - 1/e of the way.
    wah = MakeWah(envelope);
    Render(*wah, Level(0.5, 0.05));
    const double afterAttack = wah->GetParam("envelope");
    Render(*wah, Level(0.5, 2.0));
    Render(*wah, Silence(0.5));
    const double afterRelease = wah->GetParam("envelope");
    Check(Near(afterAttack, 0.5 * (1.0 - std::exp(-1.0)), 0.03), "the opening rises at the Attack time constant",
          Num(afterAttack));
    Check(Near(afterRelease, 0.5 * std::exp(-1.0), 0.03), "and falls at the Release time constant", Num(afterRelease));

    // Sensitivity scales the level read, and the opening stops at the toe.
    wah = MakeWah(Linear({{"control", 1.0}, {"position", 0.0}, {"sensitivity", 1.0}}));
    Render(*wah, Level(0.005, 0.5));
    Check(Near(wah->GetParam("envelope"), 0.5, 0.02), "Sensitivity 1 reads the level 40 dB hotter",
          Num(wah->GetParam("envelope")));
    Render(*wah, Level(0.5, 0.5));
    Check(wah->GetParam("envelope") == 1.0, "and the opening stops at the toe");

    // Pedal Position is the rest the envelope opens from.
    wah = MakeWah(Linear({{"control", 1.0}, {"position", 0.5}, {"sensitivity", 0.0}}));
    Render(*wah, Silence(0.1));
    const double restHz = wah->GetParam("currentFrequency");
    Render(*wah, Level(0.5, 1.0));
    Check(Near(restHz, std::sqrt(450.0 * 2200.0), 1.0e-6), "Pedal Position sets where the filter rests",
          Num(restHz, 1) + " Hz");
    Check(Near(wah->GetParam("currentFrequency"), 450.0 * std::pow(2200.0 / 450.0, 0.75), 1.0e-3),
          "and the envelope opens it from there toward the toe", Num(wah->GetParam("currentFrequency"), 1) + " Hz");

    // The level moves nothing on the Pedal control, and Auto-Engage never switches an envelope wah off.
    wah = MakeWah(Linear({{"control", 0.0}, {"position", 0.0}}));
    Render(*wah, Level(1.0, 0.5));
    Check(Near(wah->GetParam("currentFrequency"), 450.0, 1.0e-6) && wah->GetParam("envelope") == 0.0,
          "on the Pedal control the playing level moves nothing");
    wah = MakeWah(Linear({{"control", 1.0}, {"position", 0.0}, {"autoEngage", 1.0}}));
    Render(*wah, Silence(1.0));
    Check(wah->GetParam("engaged") == 1.0, "Auto-Engage does not switch an envelope wah off at a rest");

    // One detector for both channels: a loud left side opens the filter for a quiet right side,
    // which is heard as the right side's toe-frequency tone coming through.
    const auto quietRight = Sine(2200.0, 0.001, 0.5);
    const auto rightLevel = [&](const std::vector<float>& left) {
        auto linked = MakeWah(Linear({{"control", 1.0}, {"position", 0.0}, {"sensitivity", 1.0}}));
        std::vector<float> l(left);
        std::vector<float> r(quietRight);
        std::vector<float> outL(left.size(), 0.0f);
        std::vector<float> outR(left.size(), 0.0f);

        for (std::size_t start = 0; start < l.size(); start += kBlockSize)
        {
            const int n = static_cast<int>(std::min<std::size_t>(kBlockSize, l.size() - start));
            float* inputs[2] = {l.data() + start, r.data() + start};
            float* outputs[2] = {outL.data() + start, outR.data() + start};
            linked->Process(inputs, outputs, n);
        }

        return Rms(outR, outR.size() / 2);
    };
    const double opened = rightLevel(Sine(2200.0, 0.5, 0.5));
    const double closed = rightLevel(Silence(0.5));
    Check(opened > 4.0 * closed, "the louder channel opens the filter for both", Db(opened / closed));
}
} // namespace

int main()
{
    std::cout << "=== WahEnvelopeTests ===" << std::endl;
    guitarfx::RegisterWahEffect();

    TestEnvelopeControl();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
