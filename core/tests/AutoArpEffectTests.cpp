/**
 * @file AutoArpEffectTests.cpp
 * @brief The Auto Arpeggiator's parameters and their safety on the audio thread.
 *
 *   - SetParam takes any key and any value without throwing, and nothing it or Process does
 *     allocates on the audio thread
 */

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/FiniteCheck.h"
#include "dsp/effects/AutoArpEffect.h"
#include "helpers/AudioThreadAllocations.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;

using guitarfx::AutoArpEffect;

int gFailures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

/// Six harmonics falling as 1/k, from `from` to `to` seconds of a `total`-second signal.
std::vector<float> Harmonic(double hz, double from, double to, double total, double sampleRate = 48000.0)
{
    std::vector<float> x(static_cast<size_t>(total * sampleRate), 0.0f);
    const auto end = std::min(x.size(), static_cast<size_t>(to * sampleRate));

    for (size_t n = static_cast<size_t>(from * sampleRate); n < end; ++n)
    {
        double v = 0.0;

        for (int k = 1; k <= 6; ++k)
        {
            v += std::sin(2.0 * kPi * k * hz * static_cast<double>(n) / sampleRate + 0.7 * k) / k;
        }

        x[n] = static_cast<float>(0.2 * v);
    }

    return x;
}

void TestParamSafety()
{
    std::cout << "\n--- SetParam takes any key and value ---\n";
    AutoArpEffect effect;
    effect.Prepare(48000.0, 64);
    const double before = effect.GetParam("step3");
    std::string problem;

    // None of these is declared, and "stepMode" used to throw std::invalid_argument from std::stoi.
    for (const char* key : {"stepMode", "step", "step8", "step-1", "step01", "stepX", "steps", "step 1"})
    {
        try
        {
            effect.SetParam(key, 5.0);

            if (effect.GetParam(key) != 0.0)
            {
                problem += std::string(problem.empty() ? "" : "; ") + key + " reads back";
            }
        }
        catch (const std::exception& e)
        {
            problem += std::string(problem.empty() ? "" : "; ") + key + " threw " + e.what();
        }
    }

    Check(problem.empty() && effect.GetParam("step3") == before, "undeclared step* keys are ignored, without throwing",
          problem);

    effect.SetParam("step7", 9.0);
    effect.SetParam("step0", -30.0);
    Check(effect.GetParam("step7") == 9.0 && effect.GetParam("step0") == -24.0, "step0..step7 still set and read back");

    // A NaN used to be cast to an int: a pattern, numSteps or step rate of INT_MIN, read as an index.
    const double nan = std::nan("");

    for (const char* key : {"pattern", "numSteps", "stepRate", "direction", "bpm", "gate", "mix", "step2"})
    {
        effect.SetParam(key, nan);
    }

    std::vector<float> in(64, 0.1f);
    std::vector<float> outL(64);
    std::vector<float> outR(64);
    float* inputs[2] = {in.data(), in.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    bool finite = true;

    for (int b = 0; b < 400; ++b)
    {
        effect.Process(inputs, outputs, 64);
        finite = finite && std::all_of(outL.begin(), outL.end(), [](float v) { return guitarfx::IsFinite(v); });
    }

    Check(finite && effect.GetParam("pattern") == 0.0 && effect.GetParam("numSteps") == 4.0 &&
              effect.GetParam("stepRate") == 1.0,
          "a NaN changes nothing");
}

void TestNoAudioThreadAllocations()
{
    std::cout << "\n--- Nothing allocates on the audio thread ---\n";
    AutoArpEffect effect;
    effect.Prepare(48000.0, 64);
    const auto tone = Harmonic(196.0, 0.0, 4.0, 4.0);
    std::vector<float> outL(64);
    std::vector<float> outR(64);

    // The keys exist before the audio thread sees them, as an automation binding's do: a Debug
    // std::string allocates a container proxy when it is made from a literal.
    const std::string patternKey = "pattern";
    const std::string directionKey = "direction";
    const std::string numStepsKey = "numSteps";
    const std::string stepKey = "step5";
    const std::string stepRateKey = "stepRate";
    const std::string undeclaredKey = "stepMode";
    const std::string pitchModeKey = "pitchMode";

    bool threw = false;
    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        size_t pos = 0;

        auto play = [&](int blocks) {
            for (int b = 0; b < blocks && pos + 64 <= tone.size(); ++b, pos += 64)
            {
                float* inputs[2] = {const_cast<float*>(tone.data() + pos), const_cast<float*>(tone.data() + pos)};
                float* outputs[2] = {outL.data(), outR.data()};
                effect.Process(inputs, outputs, 64);
            }
        };

        play(200);

        // What MIDI and DAW automation can move: every pattern, direction and length, the custom
        // steps (rebuilding the list), undeclared keys, and the pitch trigger, between blocks.
        for (double pattern = 0.0; pattern <= 5.0; pattern += 1.0)
        {
            effect.SetParam(patternKey, pattern);

            for (double direction = 0.0; direction <= 2.0; direction += 1.0)
            {
                effect.SetParam(directionKey, direction);
                effect.SetParam(numStepsKey, 8.0);
                effect.SetParam(stepKey, -7.0);
                effect.SetParam(stepRateKey, 2.0 + direction);

                try
                {
                    effect.SetParam(undeclaredKey, 1.0);
                }
                catch (const std::exception&)
                {
                    threw = true;
                }

                play(40);
            }
        }

        effect.SetParam(pitchModeKey, 1.0);
        play(200);
        effect.SetParam(pitchModeKey, 0.0);
        play(200);
    });

    Check(allocations.count == 0 && !threw, "SetParam, step changes and the pitch trigger",
          audio_thread_allocations::Describe(allocations) + (threw ? ", and \"stepMode\" threw" : ""));
}
} // namespace

int main()
{
    std::cout << "=== AutoArpEffectTests ===" << std::endl;
    TestNoAudioThreadAllocations();
    TestParamSafety();

    std::cout << "\n" << (gFailures == 0 ? "All passed" : std::to_string(gFailures) + " failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
