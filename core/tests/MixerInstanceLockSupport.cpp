#include "MixerInstanceLockSupport.h"

#include "PluginController.h"
#include "util/Base64.h"
#include "util/Wav.h"

#include <iostream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace mixer_instance_lock_test
{
bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(100us);
    }
    return true;
}

bool WaitForAdvance(const std::atomic<int>& counter, int from, int by, std::chrono::milliseconds timeout)
{
    return WaitUntil([&] { return counter.load(std::memory_order_acquire) - from >= by; }, timeout);
}

bool TestRiffTrimKeepsAudioRunning(guitarfx::PluginController& controller, const std::atomic<int>& audioBlocks,
                                   const std::atomic<int>& audioLockMisses, double sampleRate)
{
    constexpr std::size_t kFrames = 2'000'000;
    {
        const std::vector<float> left(kFrames, 0.1f);
        const std::vector<float> right(kFrames, -0.1f);
        const auto wavBytes = guitarfx::util::EncodeStereo16BitWav(left, right, static_cast<int>(sampleRate));
        controller.HandleUIMessage(nlohmann::json{{"type", "importRiffWav"},
                                                {"data", guitarfx::util::EncodeBase64(wavBytes)},
                                                {"tempoBpm", 120.0},
                                                {"timeSigNum", 4},
                                                {"timeSigDen", 4}}
                                       .dump());
    }

    const int blocksBefore = audioBlocks.load(std::memory_order_acquire);
    const int missesBefore = audioLockMisses.load(std::memory_order_acquire);
    controller.HandleUIMessage(nlohmann::json{{"type", "trimCapturedRiff"}, {"startRatio", 0.05}, {"endRatio", 0.95}}
                                   .dump());
    const int blocksDuringTrim = audioBlocks.load(std::memory_order_acquire) - blocksBefore;
    const int missesDuringTrim = audioLockMisses.load(std::memory_order_acquire) - missesBefore;

    bool passed = Check(blocksDuringTrim > 0,
                        "riff trim: audio processed while the large buffers were copied and scanned (" +
                            std::to_string(blocksDuringTrim) + " blocks)");
    passed = Check(missesDuringTrim <= 2,
                   "riff trim: only brief snapshot swaps contended with audio (" +
                       std::to_string(missesDuringTrim) + " missed blocks)") &&
             passed;

    controller.HandleUIMessage(nlohmann::json{{"type", "stopRiffCapture"}, {"canceled", true}}.dump());
    return passed;
}
} // namespace mixer_instance_lock_test
