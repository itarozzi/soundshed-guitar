/**
 * @file SignalPathNodeConfigLockTests.cpp
 * @brief updateSignalPathNodeConfig must not race the audio thread, nor stall it for a hosted plugin.
 *
 * Some effects' SetConfig rebuilds state their Process() reads: a NAM amp re-prepares its
 * model lanes and resampler when its oversampling tier changes. The slot lookup that finds
 * the node also walks instances the audio thread erases as their fades finish. So the
 * handler applies config under the DSP lock -- except to a hosted plugin, which locks itself
 * and must not be driven under the DSP lock, or a plugin load or editor open would silence
 * the audio for as long as it takes.
 *
 * An audio thread calls PluginController::ProcessAudio throughout, as a host callback does,
 * while the test sends the message the UI sends. Two probe effects make the lock observable
 * rather than leaving the result to luck:
 *  - a plain effect whose SetConfig holds a window open and records whether its Process()
 *    ran inside it. It must not have: the DSP lock is held.
 *  - a stand-in hosted plugin, registered under the plugin-host type, whose SetConfig and
 *    state capture wait for their Process() to run. It must: the DSP lock is not held.
 * Real NAM amps (optimized and blend, on small generated Linear models) then take the keys
 * the UI's undo replay can send, across preset reloads, and the output has to stay finite.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
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
constexpr const char* kPresetId = "node-config-lock-preset";
constexpr const char* kPlainProbeType = "test_config_lock_probe";
constexpr const char* kProbeKey = "probeWindow";
constexpr const char* kPluginStateKey = "pluginStateBase64";
constexpr const char* kStandInState = "c3RhbmQtaW4tc3RhdGU=";

// How long the plain probe holds its window open. An unlocked audio thread runs a block
// every fraction of a millisecond here, so one landing inside it is certain.
constexpr auto kPlainProbeWindow = 40ms;
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

void CopyThrough(float** inputs, float** outputs, int numSamples)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        if (outputs[ch] && inputs[ch] && outputs[ch] != inputs[ch])
        {
            std::copy(inputs[ch], inputs[ch] + numSamples, outputs[ch]);
        }
    }
}

// ── Plain probe: config must be applied under the DSP lock ──────────────────

struct PlainProbeState
{
    std::atomic<bool> armed{false};
    std::atomic<bool> inWindow{false};
    std::atomic<int> processCalls{0};
    std::atomic<int> windows{0};
    std::atomic<int> overlaps{0};
};

PlainProbeState gPlainProbe;

class PlainConfigProbe final : public EffectProcessor
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
        if (gPlainProbe.inWindow.load(std::memory_order_acquire))
        {
            gPlainProbe.overlaps.fetch_add(1, std::memory_order_acq_rel);
        }

        gPlainProbe.processCalls.fetch_add(1, std::memory_order_acq_rel);
        CopyThrough(inputs, outputs, numSamples);
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string& key, const std::string&) override
    {
        if (key != kProbeKey || !gPlainProbe.armed.load(std::memory_order_acquire))
        {
            return;
        }

        // Under the DSP lock no block can run in here, so this waits out the whole window.
        // Without it the audio thread processes one within a millisecond or so, and the
        // wait ends early with the overlap recorded.
        gPlainProbe.inWindow.store(true, std::memory_order_release);
        const int before = gPlainProbe.processCalls.load(std::memory_order_acquire);
        WaitForAdvance(gPlainProbe.processCalls, before, 1, kPlainProbeWindow);
        gPlainProbe.inWindow.store(false, std::memory_order_release);
        gPlainProbe.windows.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return kPlainProbeType;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }
};

// ── Stand-in hosted plugin: config must be applied off the DSP lock ─────────

struct HostedStandInState
{
    std::atomic<bool> measuring{false};
    std::atomic<int> processCalls{0};
    std::atomic<int> measuredCalls{0};
    std::atomic<int> blockedCalls{0};
    std::mutex keysMutex;
    std::vector<std::string> measuredKeys;
};

HostedStandInState gHosted;

/// While measuring, a call has to see the audio thread run this node's Process(). That can
/// only happen if nobody holds the DSP lock, which is what the real plugin host relies on.
void ExpectAudioRunsDuring(const std::string& what)
{
    if (!gHosted.measuring.load(std::memory_order_acquire))
    {
        return;
    }

    const int before = gHosted.processCalls.load(std::memory_order_acquire);
    const bool ran = WaitForAdvance(gHosted.processCalls, before, 2, kAudioTimeout);
    gHosted.measuredCalls.fetch_add(1, std::memory_order_acq_rel);

    if (!ran)
    {
        gHosted.blockedCalls.fetch_add(1, std::memory_order_acq_rel);
    }

    std::lock_guard<std::mutex> lock(gHosted.keysMutex);
    gHosted.measuredKeys.push_back(what + (ran ? "" : " (audio blocked)"));
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
        gHosted.processCalls.fetch_add(1, std::memory_order_acq_rel);
        CopyThrough(inputs, outputs, numSamples);
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string& key, const std::string&) override
    {
        ExpectAudioRunsDuring("SetConfig " + key);
    }

    [[nodiscard]] std::string GetConfig(const std::string& key) const override
    {
        if (key != kPluginStateKey)
        {
            return {};
        }

        ExpectAudioRunsDuring("GetConfig " + key);
        return kStandInState;
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return EffectGuids::kPluginHost;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }
};

void RegisterProbes()
{
    EffectTypeInfo plain;
    plain.type = kPlainProbeType;
    plain.displayName = "Config lock probe";
    plain.category = "utility";
    EffectRegistry::Instance().Register(plain.type, plain, [] { return std::make_unique<PlainConfigProbe>(); });

    // The core tests build without JUCE, so the plugin-host type is free to stand in for.
    EffectTypeInfo hosted;
    hosted.type = EffectGuids::kPluginHost;
    hosted.aliases = {"plugin_host"};
    hosted.displayName = "Hosted plugin stand-in";
    hosted.category = "utility";
    EffectRegistry::Instance().Register(hosted.type, hosted, [] { return std::make_unique<HostedPluginStandIn>(); });
}

// ── Host, models and presets ─────────────────────────────────────────────────

class TestHost final : public IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(message);
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

    /// True if a message of `type` was sent whose fields all match `fields`.
    [[nodiscard]] bool SawMessage(const std::string& type, const nlohmann::json& fields) const
    {
        std::lock_guard<std::mutex> lock(mMutex);

        for (const auto& text : mMessages)
        {
            const auto message = nlohmann::json::parse(text, nullptr, false);

            if (message.is_discarded() || message.value("type", "") != type)
            {
                continue;
            }

            const bool matches = std::all_of(fields.items().begin(), fields.items().end(), [&](const auto& field) {
                return message.contains(field.key()) && message[field.key()] == field.value();
            });

            if (matches)
            {
                return true;
            }
        }

        return false;
    }

  private:
    fs::path mUserDataPath;
    mutable std::mutex mMutex;
    std::vector<std::string> mMessages;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

/// A Linear NAM model: a scaled copy of the input. Loads and runs in microseconds even in
/// Debug, where a WaveNet takes minutes, and still goes through the full NAM lane setup
/// (resampler, anti-alias filter, prewarm) that an oversampling change rebuilds.
fs::path WriteLinearModel(const fs::path& dir, const std::string& name, float gain)
{
    const nlohmann::json model = {{"version", "0.5.4"},
                                  {"architecture", "Linear"},
                                  {"config", {{"receptive_field", 4}, {"bias", false}}},
                                  {"weights", {gain, 0.0f, 0.0f, 0.0f}},
                                  {"sample_rate", kSampleRate},
                                  {"metadata", nlohmann::json::object()}};

    const fs::path path = dir / (name + ".nam");
    std::ofstream(path) << model.dump();
    return path;
}

ResourceRef ModelRef(const fs::path& path, std::optional<double> blendPosition = std::nullopt)
{
    ResourceRef ref;
    ref.resourceType = "nam";
    ref.filePath = path;

    if (blendPosition)
    {
        ref.parameterId = "gain";
        ref.parameterValue = *blendPosition;
    }

    return ref;
}

/// input -> probe -> hosted stand-in -> NAM amp -> NAM blend -> output. `variant` changes
/// only a parameter, so each reload is a real swap that leaves the old slot fading out.
Preset BuildPreset(const fs::path& modelA, const fs::path& modelB, int variant)
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = "Node config lock";
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;

    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;

    GraphNode probe;
    probe.id = "probe";
    probe.type = kPlainProbeType;
    probe.category = "utility";

    GraphNode plugin;
    plugin.id = "plugin";
    plugin.type = EffectGuids::kPluginHost;
    plugin.category = "utility";

    GraphNode amp;
    amp.id = "amp";
    amp.type = "amp_nam_optimized";
    amp.category = "amp";
    amp.params["inputGain"] = variant % 2 == 0 ? 0.0 : -1.0;
    amp.resources.push_back(ModelRef(modelA));

    GraphNode blend;
    blend.id = "blend";
    blend.type = "amp_nam_blend";
    blend.category = "amp";
    blend.params["blend"] = 0.35;
    blend.resources.push_back(ModelRef(modelA, 0.0));
    blend.resources.push_back(ModelRef(modelB, 1.0));

    preset.graph.nodes = {in, out, probe, plugin, amp, blend};
    preset.graph.edges = {{"__input__", "probe", 0, 0, 1.0},
                          {"probe", "plugin", 0, 0, 1.0},
                          {"plugin", "amp", 0, 0, 1.0},
                          {"amp", "blend", 0, 0, 1.0},
                          {"blend", "__output__", 0, 0, 1.0}};
    return preset;
}

void LoadPreset(PluginController& controller, const Preset& preset)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "loadPreset"},
                                              {"presetId", preset.id},
                                              {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}}
                                   .dump());
}

void SendNodeConfig(PluginController& controller, const std::string& nodeId, const std::string& key,
                    const std::string& value, bool persist = true, bool capture = false)
{
    // The payload sendSignalPathNodeConfigUpdate() posts (core/ui/ts/signalPath/commands.ts).
    controller.HandleUIMessage(nlohmann::json{
        {"type", "updateSignalPathNodeConfig"},
        {"presetId", kPresetId},
        {"nodeId", nodeId},
        {"key", key},
        {"value", value},
        {"persist", persist},
        {"capture", capture}}.dump());
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

    /// Blocks until the chain has processed `blocks` more blocks; false on timeout.
    bool WaitForBlocks(int blocks) const
    {
        return WaitForAdvance(mProcessed, mProcessed.load(std::memory_order_acquire), blocks, kAudioTimeout);
    }

    [[nodiscard]] int Processed() const
    {
        return mProcessed.load(std::memory_order_acquire);
    }

    [[nodiscard]] int NonFiniteBlocks() const
    {
        return mNonFinite.load(std::memory_order_acquire);
    }

    [[nodiscard]] float PeakOutput() const
    {
        return mPeak.load(std::memory_order_acquire);
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
                bool finite = true;
                float peak = mPeak.load(std::memory_order_relaxed);

                for (int i = 0; i < kBlock; ++i)
                {
                    const float l = outL[static_cast<size_t>(i)];
                    const float r = outR[static_cast<size_t>(i)];
                    finite = finite && IsFinite(l) && IsFinite(r);
                    peak = std::max({peak, std::abs(l), std::abs(r)});
                }

                if (!finite)
                {
                    mNonFinite.fetch_add(1, std::memory_order_acq_rel);
                }
                else
                {
                    mPeak.store(peak, std::memory_order_release);
                }

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
    std::atomic<int> mNonFinite{0};
    std::atomic<float> mPeak{0.0f};
};

// ── Tests ────────────────────────────────────────────────────────────────────

bool TestPlainEffectConfigIsLocked(PluginController& controller)
{
    constexpr int kSends = 5;
    gPlainProbe.armed.store(true, std::memory_order_release);

    for (int i = 0; i < kSends; ++i)
    {
        SendNodeConfig(controller, "probe", kProbeKey, std::to_string(i));
    }

    gPlainProbe.armed.store(false, std::memory_order_release);

    bool passed = Check(gPlainProbe.windows.load() == kSends, "plain effect: every message reached SetConfig (" +
                                                                  std::to_string(gPlainProbe.windows.load()) + "/" +
                                                                  std::to_string(kSends) + ")");
    passed = Check(gPlainProbe.overlaps.load() == 0, "plain effect: Process() never ran inside SetConfig (" +
                                                         std::to_string(gPlainProbe.overlaps.load()) + " overlaps)") &&
             passed;
    return passed;
}

bool TestHostedPluginConfigIsUnlocked(PluginController& controller, const TestHost& host)
{
    gHosted.measuring.store(true, std::memory_order_release);
    SendNodeConfig(controller, "plugin", "showPluginEditor", "1", false);
    SendNodeConfig(controller, "plugin", kPluginStateKey, "__capture_plugin_state__", true, true);
    gHosted.measuring.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(gHosted.keysMutex);

        for (const auto& call : gHosted.measuredKeys)
        {
            std::cout << "  hosted stand-in: " << call << "\n";
        }
    }

    // showPluginEditor's SetConfig, then capture's GetConfig and the SetConfig that stores it.
    bool passed =
        Check(gHosted.measuredCalls.load() >= 3, "hosted plugin: editor open and state capture reached the plugin (" +
                                                     std::to_string(gHosted.measuredCalls.load()) + " calls)");
    passed = Check(gHosted.blockedCalls.load() == 0, "hosted plugin: audio kept running while the plugin was driven (" +
                                                         std::to_string(gHosted.blockedCalls.load()) +
                                                         " calls under the DSP lock)") &&
             passed;
    passed = Check(host.SawMessage("signalPathNodeConfigUpdated",
                                   {{"nodeId", "plugin"}, {"key", kPluginStateKey}, {"captured", true}}),
                   "hosted plugin: capture still reports signalPathNodeConfigUpdated") &&
             passed;

    const auto& preset = controller.GetActivePreset();
    const auto* node = preset ? preset->graph.FindNode("plugin") : nullptr;
    passed = Check(node && node->config.count(kPluginStateKey) && node->config.at(kPluginStateKey) == kStandInState,
                   "hosted plugin: captured state persisted into the working preset") &&
             passed;
    return passed;
}

bool TestNamConfigUnderLoad(PluginController& controller, AudioThread& audio, const fs::path& modelA,
                            const fs::path& modelB)
{
    struct Step
    {
        const char* nodeId;
        const char* key;
        const char* value;
    };

    // The keys the NAM amps act on, as the UI's undo replay or the blend-mode picker sends them.
    const std::vector<Step> steps = {
        {"amp", "oversampling", "1"},       {"amp", "antiAliasPhase", "1"},    {"amp", "oversampling", "2"},
        {"amp", "useCalibration", "false"}, {"amp", "slimmableSize", "0.5"},   {"blend", "oversampling", "1"},
        {"blend", "blendMode", "snap"},     {"blend", "antiAliasPhase", "1"},  {"blend", "slimmableSize", "0.5"},
        {"amp", "oversampling", "0"},       {"blend", "blendMode", "smooth"},  {"blend", "oversampling", "0"},
        {"amp", "antiAliasPhase", "0"},     {"amp", "useCalibration", "true"}, {"blend", "antiAliasPhase", "0"},
    };

    constexpr int kRounds = 6;
    const int processedBefore = audio.Processed();
    bool audioKeptUp = true;

    for (int round = 0; round < kRounds; ++round)
    {
        // A reload leaves the previous slot fading out, so the audio thread erases it from
        // the instance list while the config lookups below are walking that list.
        LoadPreset(controller, BuildPreset(modelA, modelB, round));

        for (const auto& step : steps)
        {
            SendNodeConfig(controller, step.nodeId, step.key, step.value);
            audioKeptUp = audio.WaitForBlocks(1) && audioKeptUp;
        }
    }

    bool passed = Check(audioKeptUp, "NAM: audio resumed after every config change");
    passed = Check(audio.NonFiniteBlocks() == 0, "NAM: output stayed finite (" +
                                                     std::to_string(audio.NonFiniteBlocks()) + " bad blocks of " +
                                                     std::to_string(audio.Processed() - processedBefore) + ")") &&
             passed;
    passed = Check(audio.PeakOutput() > 1.0e-4f, "NAM: the chain produced signal") && passed;

    const auto& preset = controller.GetActivePreset();
    const auto* node = preset ? preset->graph.FindNode("blend") : nullptr;
    passed = Check(node && node->config.count("blendMode") && node->config.at("blendMode") == "smooth" &&
                       node->config.count("oversampling") && node->config.at("oversampling") == "0",
                   "NAM: config is still persisted into the working preset") &&
             passed;
    return passed;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-node-config-lock-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path modelA = WriteLinearModel(sandbox, "linear-a", 0.8f);
    const fs::path modelB = WriteLinearModel(sandbox, "linear-b", 0.4f);
    bool passed = true;

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        LoadPreset(controller, BuildPreset(modelA, modelB, 0));

        // Checked before the audio thread starts: a mixer lookup is not safe alongside it.
        const auto* amp = controller.GetMixer().GetNodeProcessor(kPresetId, "amp");
        const auto* blend = controller.GetMixer().GetNodeProcessor(kPresetId, "blend");
        const bool modelsLoaded = Check(amp && amp->HasResource() && blend && blend->HasResource(),
                                        "NAM: both amps are running generated Linear models");

        AudioThread audio(controller);

        if (!modelsLoaded || !Check(audio.WaitForBlocks(20), "audio thread is processing the chain"))
        {
            passed = false;
        }
        else
        {
            passed = TestPlainEffectConfigIsLocked(controller) && passed;
            passed = TestHostedPluginConfigIsUnlocked(controller, host) && passed;
            passed = TestNamConfigUnderLoad(controller, audio, modelA, modelB) && passed;
        }

        audio.Stop();
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
    RegisterProbes();

    const bool passed = Run();
    std::cout << (passed ? "SignalPathNodeConfigLockTests PASSED\n" : "SignalPathNodeConfigLockTests FAILED\n");
    return passed ? 0 : 1;
}
