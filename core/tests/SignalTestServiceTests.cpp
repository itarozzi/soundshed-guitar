/**
 * @file SignalTestServiceTests.cpp
 * @brief The signal-path test tone: it ends, on any input bus, and measures only its own audio.
 *
 *   - on a mono input bus (no right channel, as the JUCE adapter passes it) the tone still plays
 *     out and the result arrives; it used to count down only with both channels present, so the
 *     test never ended and every demo and riff preview was refused for the rest of the session
 *   - the result covers the tone's blocks and none after: blocks run between the tone ending and
 *     the message thread reading the result used to be added in, while it read them
 *   - a second Start while a test runs, or before its result is reported, is refused
 */

#include "controller/SignalTestService.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
using guitarfx::SignalTestService;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 480;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
    gFailures += condition ? 0 : 1;
}

/// Runs a block the way PluginController::ProcessAudioLocked does: inject, the chain (a pass-through
/// here, then `outputGain`), collect.
void RunBlock(SignalTestService& test, bool monoInput, float outputGain = 1.0f)
{
    std::vector<float> left(kBlock, 0.0f), right(kBlock, 0.0f), outL(kBlock), outR(kBlock);
    float* inputs[2] = {left.data(), monoInput ? nullptr : right.data()};
    test.InjectInput(inputs, kBlock);

    for (int i = 0; i < kBlock; ++i)
    {
        outL[i] = left[i] * outputGain;
        outR[i] = (monoInput ? left[i] : right[i]) * outputGain;
    }

    float* outputs[2] = {outL.data(), outR.data()};
    test.CollectOutput(outputs, kBlock);
}

void TestMonoInputBusFinishes()
{
    std::cout << "\n-- a mono input bus --" << std::endl;
    std::vector<std::string> sent;
    SignalTestService test([&](const std::string& message) { sent.push_back(message); });
    Check(test.Start(440.0, 0.1, kSampleRate), "the test starts");

    for (int block = 0; block < 20 && test.IsActive(); ++block)
    {
        RunBlock(test, true);
    }

    test.OnIdle();
    Check(!test.IsActive(), "the tone plays out");
    Check(sent.size() == 1, "and the result is reported");

    if (!sent.empty())
    {
        const auto result = nlohmann::json::parse(sent.front());
        Check(result.value("passed", false) && result.value("inputRMS", 0.0) > 0.6,
              "the tone was heard: input RMS " + std::to_string(result.value("inputRMS", 0.0)));
    }
}

void TestOnlyTheToneIsMeasured()
{
    std::cout << "\n-- what is measured --" << std::endl;
    std::vector<std::string> sent;
    SignalTestService test([&](const std::string& message) { sent.push_back(message); });
    Check(test.Start(440.0, 0.1, kSampleRate), "the test starts");

    // 0.1 s is ten blocks. Then, before the message thread gets to the result, blocks of loud
    // playing: none of that is the tone's.
    for (int block = 0; block < 10; ++block)
    {
        RunBlock(test, false);
    }

    Check(!test.Start(440.0, 0.1, kSampleRate), "a second Start before the result is reported is refused");

    for (int block = 0; block < 10; ++block)
    {
        RunBlock(test, false, 10.0f);
    }

    test.OnIdle();
    Check(sent.size() == 1, "the result is reported once");

    if (!sent.empty())
    {
        const auto result = nlohmann::json::parse(sent.front());
        const auto output = result.value("outputRMS", nlohmann::json::array({0.0, 0.0}));
        const double left = output.is_array() && !output.empty() ? output[0].get<double>() : 0.0;
        Check(std::abs(left - result.value("inputRMS", 0.0)) < 1e-3,
              "the output measured is the tone's alone: " + std::to_string(left));
    }

    Check(test.Start(440.0, 0.1, kSampleRate), "once it is reported, a new test can start");
}
} // namespace

int main()
{
    std::cout << "=== SignalTestServiceTests ===" << std::endl;
    TestMonoInputBusFinishes();
    TestOnlyTheToneIsMeasured();
    std::cout << (gFailures == 0 ? "\nAll checks passed" : "\nSome checks failed") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
