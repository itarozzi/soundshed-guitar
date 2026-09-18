/**
 * @file HostedPluginCaptureLockTests.cpp
 * @brief Capturing hosted plugin state must not race the audio thread, nor stall it.
 *
 * Saving a preset, saving the DAW project, focusing a mixer slot and reloading a preset all
 * read each hosted plugin's live state first. Finding the plugin walks mixer instances the
 * audio thread erases as their fades finish, which is exactly what it is doing just after a
 * preset switch, so that lookup is made under the DSP lock. The plugin itself is asked after
 * the lock is released: a real one serialises its state under its own lock, and holding the
 * DSP lock across that would silence every slot.
 *
 * An audio thread calls PluginController::ProcessAudio throughout, as a host callback does,
 * and each of two mixer slots runs a stand-in hosted plugin registered under the plugin-host
 * type. The stand-in makes both halves observable rather than leaving the result to luck:
 *  - GetType() is how the lookup tells a hosted plugin from anything else, under the DSP
 *    lock. On the message thread it holds a window open and records whether its Process()
 *    ran inside it. It must not have: the DSP lock is held.
 *  - GetConfig() for the state key waits for its Process() to run. It must: the DSP lock is
 *    not held.
 * SignalPathNodeConfigLockTests covers the same pair for updateSignalPathNodeConfig's capture.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace std::chrono_literals;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kSlotA = "capture-lock-a";
constexpr const char* kSlotB = "capture-lock-b";
constexpr const char* kPluginNodeId = "plugin";
constexpr const char* kStateKey = "pluginStateBase64";
constexpr const char* kStateLengthKey = "pluginStateBase64Length";
constexpr const char* kStoredState = "c3RvcmVkLXN0YXRl";
constexpr const char* kLiveState = "bGl2ZS1zdGF0ZQ==";

// How long a lookup holds its window open. An unlocked audio thread runs a block every
// fraction of a millisecond here, so one landing inside it is certain.
constexpr auto kLookupWindow = 20ms;
// Upper bound on any wait for the audio thread; a hang becomes a failure, not a stuck ctest.
constexpr auto kAudioTimeout = 5s;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

/// Waits until `counter` has moved at least `by` past `from`, or the timeout passes.
bool WaitForAdvance(const std::atomic<int>& counter, int from, int by, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (counter.load(std::memory_order_acquire) - from < by)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }

        std::this_thread::sleep_for(100us);
    }

    return true;
}

// ── Stand-in hosted plugin ───────────────────────────────────────────────────

struct ProbeState
{
    std::atomic<bool> armed{false};
    std::thread::id messageThread; // written before `armed` is first set
    std::atomic<int> processCalls{0};
    std::atomic<int> lookups{0};
    std::atomic<int> lookupOverlaps{0};
    std::atomic<int> stateReads{0};
    std::atomic<int> stateReadsBlocked{0};
};

ProbeState gProbe;

/// Only the test's own calls are measured: the audio thread and the mixer's reaper thread
/// must never be held up by a probe.
bool IsMeasuredCall()
{
    return gProbe.armed.load(std::memory_order_acquire) && std::this_thread::get_id() == gProbe.messageThread;
}

class HostedPluginStandIn final : public EffectProcessor
{
  public:
    void Prepare(double, int) override
    {
    }

    void Reset() override
    {
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        gProbe.processCalls.fetch_add(1, std::memory_order_acq_rel);

        for (int ch = 0; ch < 2; ++ch)
        {
            if (outputs[ch] && inputs[ch] && outputs[ch] != inputs[ch])
            {
                std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
            }
        }
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    /// Always answers with the "live" chunk, never the one the preset stored, so a capture
    /// that read the running plugin is told apart from one that fell back to stored state.
    [[nodiscard]] std::string GetConfig(const std::string& key) const override
    {
        if (key != kStateKey)
        {
            return {};
        }

        if (IsMeasuredCall())
        {
            const int before = gProbe.processCalls.load(std::memory_order_acquire);
            const bool ran = WaitForAdvance(gProbe.processCalls, before, 2, kAudioTimeout);
            gProbe.stateReads.fetch_add(1, std::memory_order_acq_rel);

            if (!ran)
            {
                gProbe.stateReadsBlocked.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        return kLiveState;
    }

    [[nodiscard]] std::string GetType() const override
    {
        if (IsMeasuredCall())
        {
            // Under the DSP lock no block can run in here, so this waits out the whole window.
            // Without it the audio thread processes one within a millisecond or so.
            const int before = gProbe.processCalls.load(std::memory_order_acquire);

            if (WaitForAdvance(gProbe.processCalls, before, 1, kLookupWindow))
            {
                gProbe.lookupOverlaps.fetch_add(1, std::memory_order_acq_rel);
            }

            gProbe.lookups.fetch_add(1, std::memory_order_acq_rel);
        }

        return EffectGuids::kPluginHost;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }
};

void RegisterStandIn()
{
    // The core tests build without JUCE, so the plugin-host type is free to stand in for.
    EffectTypeInfo hosted;
    hosted.type = EffectGuids::kPluginHost;
    hosted.aliases = {"plugin_host"};
    hosted.displayName = "Hosted plugin stand-in";
    hosted.category = "utility";
    EffectRegistry::Instance().Register(hosted.type, hosted, [] { return std::make_unique<HostedPluginStandIn>(); });
}

struct Seen
{
    int lookups = 0;
    int lookupOverlaps = 0;
    int stateReads = 0;
    int stateReadsBlocked = 0;
};

/// Runs `action` on this thread with the stand-ins measuring, and returns what they saw.
Seen Observe(const std::function<void()>& action)
{
    const Seen before{gProbe.lookups.load(), gProbe.lookupOverlaps.load(), gProbe.stateReads.load(),
                      gProbe.stateReadsBlocked.load()};
    gProbe.armed.store(true, std::memory_order_release);
    action();
    gProbe.armed.store(false, std::memory_order_release);
    return {gProbe.lookups.load() - before.lookups, gProbe.lookupOverlaps.load() - before.lookupOverlaps,
            gProbe.stateReads.load() - before.stateReads, gProbe.stateReadsBlocked.load() - before.stateReadsBlocked};
}

/// A capture path found the running plugin under the DSP lock and asked it outside the lock.
bool CheckCapture(const std::string& path, const Seen& seen, int minLookups = 1)
{
    bool passed = Check(seen.lookups >= minLookups && seen.stateReads >= minLookups,
                        path + ": read live state from the running plugin (" + std::to_string(seen.lookups) +
                            " lookups, " + std::to_string(seen.stateReads) + " reads)");
    passed =
        Check(seen.lookupOverlaps == 0, path + ": the slot lookup held the DSP lock (" +
                                            std::to_string(seen.lookupOverlaps) + " lookups with audio running)") &&
        passed;
    passed =
        Check(seen.stateReadsBlocked == 0, path + ": audio kept running while the plugin was asked (" +
                                               std::to_string(seen.stateReadsBlocked) + " reads under the DSP lock)") &&
        passed;
    return passed;
}

// ── Host and presets ─────────────────────────────────────────────────────────

class TestHost final : public IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&,
                         std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlock;
    }

  private:
    fs::path mUserDataPath;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

/// input -> hosted stand-in -> output. `scrubbed` sends the plugin node the way the UI does,
/// with only the chunk's length in place of the chunk, which makes the load fill it in live.
Preset BuildPreset(const std::string& id, bool scrubbed)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;

    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;

    GraphNode plugin;
    plugin.id = kPluginNodeId;
    plugin.type = EffectGuids::kPluginHost;
    plugin.category = "utility";

    if (scrubbed)
    {
        plugin.config[kStateLengthKey] = std::to_string(std::strlen(kStoredState));
    }
    else
    {
        plugin.config[kStateKey] = kStoredState;
    }

    preset.graph.nodes = {in, out, plugin};
    preset.graph.edges = {{"__input__", kPluginNodeId, 0, 0, 1.0}, {kPluginNodeId, "__output__", 0, 0, 1.0}};
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
}

void Send(PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

/// The plugin state a serialised preset carries, or "<missing>".
std::string PluginStateIn(const nlohmann::json& presetJson)
{
    const auto preset = PresetStorage::DeserializeFromJson(presetJson.dump());
    const auto* node = preset ? preset->graph.FindNode(kPluginNodeId) : nullptr;

    if (!node || !node->config.count(kStateKey))
    {
        return "<missing>";
    }

    return node->config.at(kStateKey);
}

// ── The audio thread ─────────────────────────────────────────────────────────

/// Calls ProcessAudio back to back on its own thread, the way a host's callback does, with a
/// short gap so the message thread's blocking lock is not starved by the try_lock.
class AudioThread
{
  public:
    explicit AudioThread(PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~AudioThread()
    {
        Stop();
    }

    void Stop()
    {
        mStop.store(true, std::memory_order_release);

        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    /// Blocks until the mixer has processed `blocks` more blocks; false on timeout.
    bool WaitForBlocks(int blocks) const
    {
        return WaitForAdvance(mProcessed, mProcessed.load(std::memory_order_acquire), blocks, kAudioTimeout);
    }

  private:
    void Run()
    {
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        double phase = 0.0;
        const double phaseStep = 6.283185307179586 * 220.0 / kSampleRate;

        while (!mStop.load(std::memory_order_acquire))
        {
            for (int i = 0; i < kBlock; ++i)
            {
                inL[static_cast<size_t>(i)] = inR[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(phase));
                phase = std::fmod(phase + phaseStep, 6.283185307179586);
            }

            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};

            if (mController.ProcessAudio(inputs, outputs, kBlock))
            {
                mProcessed.fetch_add(1, std::memory_order_acq_rel);
            }

            const auto resume = std::chrono::steady_clock::now() + 200us;

            while (std::chrono::steady_clock::now() < resume)
            {
                std::this_thread::yield();
            }
        }
    }

    PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mStop{false};
    std::atomic<int> mProcessed{0};
};

// ── Tests ────────────────────────────────────────────────────────────────────

/// SerializeState: CaptureRuntimePluginStates for the focused slot, and
/// CaptureMixerSlotHostedPluginState for the other one.
bool TestProjectSave(PluginController& controller)
{
    nlohmann::json state;
    const auto seen = Observe([&] { state = nlohmann::json::parse(controller.SerializeState()); });

    // One lookup at least for each slot.
    bool passed = CheckCapture("project save", seen, 2);
    passed = Check(PluginStateIn(state.value("preset", nlohmann::json::object())) == kLiveState,
                   "project save: the focused slot's live state is in the saved preset") &&
             passed;

    const auto presetData = state["mixer"].value("presetData", nlohmann::json::object());
    passed = Check(presetData.contains(kSlotB) && PluginStateIn(presetData[kSlotB]) == kLiveState,
                   "project save: the other mixer slot's live state is in the saved preset data") &&
             passed;
    return passed;
}

/// focusMixerPreset: CaptureLiveHostedPluginStateIntoActivePreset banks the outgoing slot.
bool TestFocusChange(PluginController& controller)
{
    const auto seen = Observe([&] {
        Send(controller, {{"type", "focusMixerPreset"}, {"presetId", kSlotB}});
        Send(controller, {{"type", "focusMixerPreset"}, {"presetId", kSlotA}});
    });

    // Each focus change banks the slot it leaves.
    bool passed = CheckCapture("focus change", seen, 2);

    const auto& preset = controller.GetActivePreset();
    const auto* node = preset ? preset->graph.FindNode(kPluginNodeId) : nullptr;
    passed = Check(preset && preset->id == kSlotA && node && node->config.count(kStateKey) &&
                       node->config.at(kStateKey) == kLiveState,
                   "focus change: the slot focused again carries the live state it banked") &&
             passed;
    return passed;
}

/// savePreset naming a source slot that is not running: CaptureRuntimePluginStates finds no
/// plugin there, and falls back to the focused slot.
bool TestPresetSave(PluginController& controller)
{
    const auto seen = Observe([&] {
        Send(controller, {{"type", "savePreset"},
                          {"name", "Capture lock A"},
                          {"presetId", kSlotA},
                          {"sourcePresetId", "not-a-running-slot"}});
    });

    // The first lookup finds no processor, so any plugin read came from the fallback.
    bool passed = CheckCapture("preset save", seen);

    const auto& preset = controller.GetActivePreset();
    const auto* node = preset ? preset->graph.FindNode(kPluginNodeId) : nullptr;
    passed = Check(node && node->config.count(kStateKey) && node->config.at(kStateKey) == kLiveState,
                   "preset save: the saved preset carries the live state") &&
             passed;
    return passed;
}

/// Reloading the focused slot the way the UI does (plugin state scrubbed) banks the working
/// copy, fills the payload in from the running plugin, and leaves the replaced instance
/// fading out. The captures that follow run while the audio thread erases it.
bool TestCapturesAcrossReloads(PluginController& controller, AudioThread& audio)
{
    constexpr int kRounds = 6;
    Seen total;
    bool audioKeptUp = true;
    bool stateKept = true;

    for (int round = 0; round < kRounds; ++round)
    {
        const auto seen = Observe([&] {
            Send(controller,
                 {{"type", "loadPreset"}, {"presetId", kSlotA}, {"preset", PresetJson(BuildPreset(kSlotA, true))}});
            (void)controller.SerializeState();
            Send(controller, {{"type", "focusMixerPreset"}, {"presetId", kSlotB}});
            Send(controller, {{"type", "focusMixerPreset"}, {"presetId", kSlotA}});
        });

        total.lookups += seen.lookups;
        total.lookupOverlaps += seen.lookupOverlaps;
        total.stateReads += seen.stateReads;
        total.stateReadsBlocked += seen.stateReadsBlocked;
        audioKeptUp = audio.WaitForBlocks(1) && audioKeptUp;

        const auto& preset = controller.GetActivePreset();
        const auto* node = preset ? preset->graph.FindNode(kPluginNodeId) : nullptr;
        stateKept = stateKept && node && node->config.count(kStateKey) && node->config.at(kStateKey) == kLiveState;
    }

    // Per round: the load's fold and fill, the project save's two slots, and two focus changes.
    bool passed = CheckCapture("reloads", total, kRounds * 4);
    passed = Check(audioKeptUp, "reloads: audio resumed after every round") && passed;
    passed = Check(stateKept, "reloads: every reload filled in the live state") && passed;
    return passed;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-hosted-capture-lock-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);
    bool passed = true;

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        Send(controller,
             {{"type", "loadPreset"}, {"presetId", kSlotA}, {"preset", PresetJson(BuildPreset(kSlotA, false))}});
        Send(controller,
             {{"type", "addActivePreset"}, {"presetId", kSlotB}, {"preset", PresetJson(BuildPreset(kSlotB, false))}});

        // Checked before the audio thread starts: a mixer lookup is not safe alongside it.
        const auto activeIds = controller.GetMixer().GetActivePresetIds();
        const bool slotsRunning =
            Check(activeIds.size() == 2 && controller.GetMixer().GetNodeProcessor(kSlotA, kPluginNodeId) &&
                      controller.GetMixer().GetNodeProcessor(kSlotB, kPluginNodeId),
                  "two mixer slots are each running a hosted plugin stand-in");

        gProbe.messageThread = std::this_thread::get_id();
        AudioThread audio(controller);

        if (!slotsRunning || !Check(audio.WaitForBlocks(20), "audio thread is processing the mixer"))
        {
            passed = false;
        }
        else
        {
            passed = TestProjectSave(controller) && passed;
            passed = TestFocusChange(controller) && passed;
            passed = TestPresetSave(controller) && passed;
            passed = TestCapturesAcrossReloads(controller, audio) && passed;
        }

        audio.Stop();

        // A reload of one slot must not have swapped the mixer down to it.
        const auto finalIds = controller.GetMixer().GetActivePresetIds();
        passed = Check(finalIds.size() == 2,
                       "both mixer slots are still running (" + std::to_string(finalIds.size()) + " active)") &&
                 passed;
    }

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    // A race this test exists to catch can abort the process; show how far it got.
    std::cout << std::unitbuf;
    RegisterAllEffects();
    RegisterStandIn();

    const bool passed = Run();
    std::cout << (passed ? "HostedPluginCaptureLockTests PASSED\n" : "HostedPluginCaptureLockTests FAILED\n");
    return passed ? 0 : 1;
}
