/**
 * @file RiffCaptureNoticeTests.cpp
 * @brief Riff capture's audio-thread half tells the UI nothing itself: it raises notices, and the
 *        message thread builds and sends the messages (PluginController::PublishRiffCaptureNotices).
 *
 *  - processing a whole armed take sends nothing until the message thread's idle tick, which
 *    then sends started, one progress and stopped, in that order, with the take's numbers
 *  - with idle ticks during the take, progress arrives as it goes, counting up
 *  - a stop from the UI answers for the take: nothing raised before it follows it out
 */

#include "PluginController.h"
#include "dsp/effects/BuiltinEffects.h"

#include "MessageThreadTestHost.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr double kPi = 3.14159265358979323846;
/// One bar of 4/4 at 240 bpm: one second.
constexpr std::size_t kTakeSamples = 48000;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

/// Records the riff capture messages the controller sends, from whichever thread.
class RecordingHost : public test::PumpedTestHost
{
  public:
    using test::PumpedTestHost::PumpedTestHost;

    void SendMessageToUI(const std::string& message) override
    {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);

        if (!parsed.is_object() || parsed.value("type", std::string()).rfind("riffCapture", 0) != 0)
        {
            return;
        }

        const std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(parsed);
    }

    std::vector<nlohmann::json> Take()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        std::vector<nlohmann::json> taken;
        taken.swap(mMessages);
        return taken;
    }

  private:
    std::mutex mMutex;
    std::vector<nlohmann::json> mMessages;
};

std::vector<std::string> Types(const std::vector<nlohmann::json>& messages)
{
    std::vector<std::string> types;

    for (const auto& message : messages)
    {
        types.push_back(message.value("type", std::string()));
    }

    return types;
}

std::string Join(const std::vector<std::string>& items)
{
    std::string joined;

    for (const auto& item : items)
    {
        joined += (joined.empty() ? "" : ", ") + item;
    }

    return joined;
}

/// Plays `blocks` blocks of a tone into the controller, calling OnIdle every `idleEvery` blocks
/// (never when 0). Returns how many riff capture messages arrived during a block, as opposed to an
/// idle tick.
int Play(PluginController& controller, RecordingHost& host, int blocks, int idleEvery,
         std::vector<nlohmann::json>& fromIdle)
{
    std::vector<float> left(kBlock);
    std::vector<float> right(kBlock);
    std::vector<float> outLeft(kBlock);
    std::vector<float> outRight(kBlock);
    static std::size_t phase = 0;
    int duringBlocks = 0;

    for (int block = 0; block < blocks; ++block)
    {
        for (int i = 0; i < kBlock; ++i, ++phase)
        {
            left[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * kPi * 220.0 * static_cast<double>(phase) / kSampleRate));
            right[static_cast<std::size_t>(i)] = left[static_cast<std::size_t>(i)];
        }

        float* inputs[2] = {left.data(), right.data()};
        float* outputs[2] = {outLeft.data(), outRight.data()};
        controller.ProcessAudio(inputs, outputs, kBlock);
        duringBlocks += static_cast<int>(host.Take().size());

        if (idleEvery > 0 && (block + 1) % idleEvery == 0)
        {
            controller.OnIdle();
            auto messages = host.Take();
            fromIdle.insert(fromIdle.end(), messages.begin(), messages.end());
        }
    }

    return duringBlocks;
}

nlohmann::json ArmMessage()
{
    return {{"type", "armRiffCapture"},
            {"tempoBpm", 240.0},
            {"timeSigNum", 4},
            {"timeSigDen", 4},
            {"bars", 1},
            {"countInBars", 0},
            {"metronomeClickEnabled", false}};
}

bool TestWholeTakeBeforeIdle(PluginController& controller, RecordingHost& host)
{
    bool passed = true;
    controller.HandleUIMessage(ArmMessage().dump());
    const auto armed = host.Take();
    passed = Check(Types(armed) == std::vector<std::string>{"riffCaptureArmed"}, "arming answers at once") && passed;

    // A little over the bar, with no idle tick at all.
    std::vector<nlohmann::json> fromIdle;
    const int blocks = static_cast<int>(kTakeSamples / kBlock) + 40;
    const int duringBlocks = Play(controller, host, blocks, 0, fromIdle);
    passed =
        Check(duringBlocks == 0, "the audio thread sends nothing (" + std::to_string(duringBlocks) + " messages)") &&
        passed;

    controller.OnIdle();
    const auto messages = host.Take();
    passed = Check(Types(messages) ==
                       std::vector<std::string>{"riffCaptureStarted", "riffCaptureProgress", "riffCaptureStopped"},
                   "the idle tick sends started, progress, stopped (got: " + Join(Types(messages)) + ")") &&
             passed;

    if (messages.size() == 3)
    {
        const auto& started = messages[0];
        const auto& stopped = messages[2];
        passed = Check(started.contains("barAlignOffsetSamples") && started.value("bars", 0) == 1,
                       "started carries the bar alignment and the take's bars") &&
                 passed;
        passed = Check(stopped.value("capturedSamples", std::size_t{0}) == kTakeSamples &&
                           stopped.value("bars", 0) == 1 && stopped.value("hasAudio", false),
                       "stopped carries the full take: " + stopped.dump().substr(0, 120)) &&
                 passed;

        const auto& peaks = stopped["waveformPeaks"];
        bool peaksHeard = peaks.is_array() && peaks.size() == 256;

        for (const auto& peak : peaks)
        {
            peaksHeard = peaksHeard && peak.get<double>() > 0.4;
        }

        passed = Check(peaksHeard, "every live peak bucket heard the tone") && passed;
    }

    controller.OnIdle();
    passed = Check(host.Take().empty(), "nothing more once the take is out") && passed;
    return passed;
}

bool TestProgressAsItGoes(PluginController& controller, RecordingHost& host)
{
    bool passed = true;
    controller.HandleUIMessage(ArmMessage().dump());
    (void)host.Take();

    std::vector<nlohmann::json> fromIdle;
    const int blocks = static_cast<int>(kTakeSamples / kBlock) + 40;
    const int duringBlocks = Play(controller, host, blocks, 4, fromIdle);
    passed = Check(duringBlocks == 0, "the audio thread sends nothing with idle ticks running") && passed;

    const auto types = Types(fromIdle);
    int progress = 0;
    std::size_t lastCaptured = 0;
    bool countsUp = true;

    for (const auto& message : fromIdle)
    {
        if (message.value("type", std::string()) == "riffCaptureProgress")
        {
            ++progress;
            const auto captured = message.value("capturedSamples", std::size_t{0});
            countsUp = countsUp && captured > lastCaptured;
            lastCaptured = captured;
        }
    }

    passed = Check(!types.empty() && types.front() == "riffCaptureStarted" && types.back() == "riffCaptureStopped",
                   "started comes first and stopped last") &&
             passed;
    // Every 250 ms of a one-second take, less the one that lands with the stop.
    passed =
        Check(progress >= 3, "progress arrives as the take goes (" + std::to_string(progress) + " messages)") && passed;
    passed = Check(countsUp, "progress counts up") && passed;
    return passed;
}

bool TestStopAnswersForTheTake(PluginController& controller, RecordingHost& host)
{
    bool passed = true;
    controller.HandleUIMessage(ArmMessage().dump());
    (void)host.Take();

    // Half the take with no idle tick, so started and progress are both waiting.
    std::vector<nlohmann::json> fromIdle;
    (void)Play(controller, host, static_cast<int>(kTakeSamples / kBlock / 2), 0, fromIdle);
    controller.HandleUIMessage(nlohmann::json{{"type", "stopRiffCapture"}}.dump());
    const auto stopped = host.Take();
    passed = Check(Types(stopped) == std::vector<std::string>{"riffCaptureStopped"},
                   "a stop from the UI answers at once (got: " + Join(Types(stopped)) + ")") &&
             passed;

    controller.OnIdle();
    const auto after = host.Take();
    passed =
        Check(after.empty(), "nothing raised before the stop follows it (got: " + Join(Types(after)) + ")") && passed;
    return passed;
}

bool Run(const fs::path& sandbox)
{
    RecordingHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    controller.OnIdle();
    (void)host.Take();

    bool passed = TestWholeTakeBeforeIdle(controller, host);
    passed = TestProgressAsItGoes(controller, host) && passed;
    passed = TestStopAnswersForTheTake(controller, host) && passed;
    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-riff-capture-notice-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    const bool passed = Run(sandbox);
    fs::remove_all(sandbox, ec);
    std::cout << (passed ? "RiffCaptureNoticeTests PASSED\n" : "RiffCaptureNoticeTests FAILED\n");
    return passed ? 0 : 1;
}
