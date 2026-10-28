/**
 * @file NamQualityPolicyTests.cpp
 * @brief What a host rendering offline changes: not the chain, only how the audio thread
 * treats a busy DSP lock.
 *
 * A DAW flips AudioProcessor::setNonRealtime() around a bounce/freeze/export, which reaches
 * PluginController::SetOfflineRendering() on whatever thread the host used; JUCE's VST3 wrapper
 * calls it from process(). 1.6.0 answered it by re-preparing every NAM node at a higher tier
 * under the DSP lock, and every block asked for meanwhile came back silent, which a bounce writes
 * into the file: FL Studio users lost up to a second at the start of every render.
 *
 *  - The switch leaves the NAM tier, the latency and the DSP lock alone: audio running through
 *    it never loses a block, and the host hears nothing.
 *  - Offline, ProcessAudio waits for a busy DSP lock rather than give the block up. In real time
 *    it still gives it up, since waiting there would miss the deadline.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

int gFailures = 0;

void Check(bool condition, const std::string& label)
{
    std::cout << "  " << label << ": " << (condition ? "PASS" : "FAIL") << "\n";

    if (!condition)
    {
        ++gFailures;
    }
}

class TestHost final : public test::PumpedTestHost
{
  public:
    explicit TestHost(fs::path userDataPath)
        : PumpedTestHost(std::move(userDataPath), std::this_thread::get_id(), kSampleRate, kBlock)
    {
    }

    void NotifyLatencyChanged(int) override
    {
        latencyReports.fetch_add(1);
    }

    std::atomic<int> latencyReports{0};
};

/// A host's audio thread: ProcessAudio back to back, counting the blocks it gave up and the
/// longest a block took.
class AudioThread
{
  public:
    explicit AudioThread(PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~AudioThread()
    {
        mStop.store(true);
        mThread.join();
    }

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    /// Blocks until `blocks` more have been processed; false after ten seconds.
    bool WaitForBlocks(int blocks) const
    {
        const int from = processed.load();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        while (processed.load() - from < blocks)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        return true;
    }

    std::atomic<int> processed{0};
    std::atomic<int> givenUp{0};
    std::atomic<double> longestBlockMs{0.0};
    std::atomic<double> processedMs{0.0}; ///< Summed time of the processed blocks; only this thread writes it.

  private:
    void Run()
    {
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        double phase = 0.0;

        while (!mStop.load())
        {
            for (int i = 0; i < kBlock; ++i)
            {
                inL[static_cast<std::size_t>(i)] = inR[static_cast<std::size_t>(i)] =
                    static_cast<float>(0.1 * std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 110.0 / kSampleRate;
            }

            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};
            const auto start = std::chrono::steady_clock::now();
            const bool ran = mController.ProcessAudio(inputs, outputs, kBlock);
            const auto took = std::chrono::steady_clock::now() - start;
            const double ms = std::chrono::duration<double, std::milli>(took).count();

            if (ran)
            {
                // Before the count, so a reader that sees the count sees its block's time.
                processedMs.store(processedMs.load() + ms);
            }

            (ran ? processed : givenUp).fetch_add(1);
            longestBlockMs.store(std::max(longestBlockMs.load(), ms));
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mStop{false};
};

/// NAM amp into an IR cab: the NAM node is what 1.6.0 re-prepared, and the cab gives Prepare a
/// convolver to rebuild, so a prepare holds the DSP lock long enough to be seen.
Preset MakeRig(const fs::path& assets)
{
    const auto node = [](const std::string& id, const char* type) {
        GraphNode n;
        n.id = id;
        n.type = type;
        n.enabled = true;
        return n;
    };

    GraphNode amp = node("amp", EffectGuids::kAmpNamOptimized);
    ResourceRef model;
    model.filePath = (assets / "amps/Guitar/Emil Rohbe/VOX AC15 NAM PACK/VOX AC15 Capture 3 - Emil Rohbe.nam").string();
    amp.resources.push_back(model);

    GraphNode cab = node("cab", EffectGuids::kCabIr);
    ResourceRef impulse;
    impulse.filePath = (assets / "ir/421 1960.wav").string();
    cab.resources.push_back(impulse);

    Preset preset;
    preset.id = "offline-policy-rig";
    preset.name = "offline-policy-rig";
    preset.graph.nodes = {node("in", kNodeTypeInput), amp, cab, node("out", kNodeTypeOutput)};
    preset.graph.edges = {{"in", "amp"}, {"amp", "cab"}, {"cab", "out"}};
    return preset;
}

std::string SavedNamQuality(PluginController& controller)
{
    const auto state = nlohmann::json::parse(controller.SerializeState(), nullptr, false);
    return state.is_object() && state.contains("namQuality") ? state["namQuality"].dump() : std::string{"<none>"};
}

/// Switching offline and back, as a host does around every bounce, touches nothing.
void TestSwitchTouchesNothing(PluginController& controller, TestHost& host, AudioThread& audio)
{
    const int givenUpBefore = audio.givenUp.load();
    const int reportsBefore = host.latencyReports.load();
    const std::string qualityBefore = SavedNamQuality(controller);
    bool kept = true;

    for (int round = 0; round < 10; ++round)
    {
        controller.SetOfflineRendering(true);
        kept = audio.WaitForBlocks(3) && kept;
        controller.SetOfflineRendering(false);
        kept = audio.WaitForBlocks(3) && kept;
    }

    host.Pump();
    Check(kept, "audio kept running through the switches");
    Check(audio.givenUp.load() == givenUpBefore, "switching offline and back never cost the audio thread a block (" +
                                                     std::to_string(audio.givenUp.load() - givenUpBefore) + " lost)");
    Check(host.latencyReports.load() == reportsBefore, "the host heard nothing about it");
    Check(SavedNamQuality(controller) == qualityBefore, "the instance's NAM tier is unchanged");
}

/// The same busy spell on the message thread costs blocks in real time, and none offline.
void TestOfflineWaitsForTheLock(PluginController& controller, AudioThread& audio)
{
    // A host re-activating the plugin: Prepare holds the DSP lock while it rebuilds the cab.
    const auto busySpell = [&] {
        for (int i = 0; i < 3; ++i)
        {
            controller.Reset();
            controller.Prepare(kSampleRate, kBlock);
            audio.WaitForBlocks(2);
        }
    };

    int givenUpBefore = audio.givenUp.load();
    busySpell();
    const int realtimeLost = audio.givenUp.load() - givenUpBefore;
    Check(realtimeLost > 0, "in real time a busy DSP lock costs blocks (" + std::to_string(realtimeLost) + ")");

    controller.SetOfflineRendering(true);

    // A typical block of this rig, offline, with nothing holding the lock.
    const int quietFrom = audio.processed.load();
    const double quietMsFrom = audio.processedMs.load();
    audio.WaitForBlocks(50);
    const double typicalMs = (audio.processedMs.load() - quietMsFrom) / std::max(1, audio.processed.load() - quietFrom);

    givenUpBefore = audio.givenUp.load();
    audio.longestBlockMs.store(0.0);
    busySpell();
    Check(audio.WaitForBlocks(5), "offline the audio thread keeps going (no deadlock)");
    Check(audio.givenUp.load() == givenUpBefore,
          "offline it gives up no block (" + std::to_string(audio.givenUp.load() - givenUpBefore) + " lost)");
    // A block that waited out a prepare takes several typical blocks. How long a prepare holds the
    // lock depends on the NAM engine: on the fused engine those blocks took 1.1-1.8 ms against a
    // typical 0.22, and on the NAM core (GUITARFX_NAM_ENGINE=core) 7.6-10 ms against 0.5. So the bar
    // is the typical block just measured, not a fixed time.
    Check(audio.longestBlockMs.load() >= 3.0 * typicalMs, "it waited for the lock instead (longest block " +
                                                              std::to_string(audio.longestBlockMs.load()) +
                                                              " ms, typical " + std::to_string(typicalMs) + " ms)");
    controller.SetOfflineRendering(false);
}
} // namespace

int main()
{
    std::cout << std::unitbuf << "\n--- Offline render policy ---\n";
    RegisterAllEffects();

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-nam-quality-policy-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        const auto preset = nlohmann::json::parse(
            PresetStorage::SerializeToJson(MakeRig(fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets")));
        controller.HandleUIMessage(
            nlohmann::json{{"type", "loadPreset"}, {"presetId", "offline-policy-rig"}, {"preset", preset}}.dump());
        host.Pump();
        controller.Prepare(kSampleRate, kBlock);
        host.Pump();

        {
            AudioThread audio(controller); // stopped before the controller goes
            Check(audio.WaitForBlocks(20), "audio thread is processing the rig");
            TestSwitchTouchesNothing(controller, host, audio);
            TestOfflineWaitsForTheLock(controller, audio);
        }

        host.Pump();
    }

    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << "Offline render policy tests failed: " << gFailures << "\n";
        return 1;
    }

    std::cout << "All offline render policy tests passed.\n";
    return 0;
}
