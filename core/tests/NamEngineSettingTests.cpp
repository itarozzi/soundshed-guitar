// The Fast NAM Engine setting (audio.nam.fusedEngine): on by default, persisted in the shared
// store, and a change rebuilds the running presets so their NAM models move to the other engine.

#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/NamModelCache.h"
#include "dsp/nam/FusedWaveNet.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr const char* kKey = "audio.nam.fusedEngine";

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
};

/// A NAM amp alone, on a standard WaveNet the fused pass takes.
Preset MakeRig()
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
    model.filePath = (fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" /
                      "amps/Guitar/Emil Rohbe/VOX AC15 NAM PACK/VOX AC15 Capture 3 - Emil Rohbe.nam")
                         .string();
    amp.resources.push_back(model);

    Preset preset;
    preset.id = "nam-engine-rig";
    preset.name = "nam-engine-rig";
    preset.graph.nodes = {node("in", kNodeTypeInput), amp, node("out", kNodeTypeOutput)};
    preset.graph.edges = {{"in", "amp"}, {"amp", "out"}};
    return preset;
}

void SetSetting(PluginController& controller, TestHost& host, const nlohmann::json& value)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "setSetting"}, {"key", kKey}, {"value", value}}.dump());
    host.Pump();
}

void LoadRig(PluginController& controller, TestHost& host)
{
    const auto preset = nlohmann::json::parse(PresetStorage::SerializeToJson(MakeRig()));
    controller.HandleUIMessage(
        nlohmann::json{{"type", "loadPreset"}, {"presetId", "nam-engine-rig"}, {"preset", preset}}.dump());
    host.Pump();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
}

void TestToggleRebuildsRunningModels(const fs::path& root)
{
    std::cout << "\n--- Toggle rebuilds the running NAM models ---\n";
    TestHost host(root);
    PluginController controller(host);
    controller.Initialize();
    host.Pump();
    Check(nammodelcache::IsFusedEngineEnabled(), "the fast engine is on by default");

    LoadRig(controller, host);
    nammodelcache::ResetStats();

    SetSetting(controller, host, false);
    Check(!nammodelcache::IsFusedEngineEnabled(), "turning the setting off turns the engine off");
    auto stats = nammodelcache::GetStats();
    Check(stats.coreBuilds > 0 && stats.fusedBuilds == 0, "the running preset rebuilt its NAM model on the NAM core (" +
                                                              std::to_string(stats.coreBuilds) + " core, " +
                                                              std::to_string(stats.fusedBuilds) + " fused)");

    nammodelcache::ResetStats();
    SetSetting(controller, host, false);
    stats = nammodelcache::GetStats();
    Check(stats.coreBuilds == 0 && stats.fusedBuilds == 0, "setting the same value again rebuilds nothing");

    SetSetting(controller, host, true);
    stats = nammodelcache::GetStats();
    Check(nammodelcache::IsFusedEngineEnabled(), "turning it back on turns the engine on");
    Check(stats.fusedBuilds > 0, "the running preset rebuilt its NAM model on the fast engine (" +
                                     std::to_string(stats.fusedBuilds) + " fused)");

    // Left off for the next test to read back.
    SetSetting(controller, host, false);
}

void TestStoredValueAppliesAtStartup(const fs::path& root)
{
    std::cout << "\n--- The stored value applies at startup ---\n";
    TestHost host(root);
    PluginController controller(host);
    controller.Initialize();
    host.Pump();
    Check(!nammodelcache::IsFusedEngineEnabled(), "a new controller reads the stored off");

    LoadRig(controller, host);
    nammodelcache::ResetStats();
    SetSetting(controller, host, nullptr);
    Check(nammodelcache::IsFusedEngineEnabled(), "unsetting the setting restores the default, on");
    Check(nammodelcache::GetStats().fusedBuilds > 0, "and rebuilds the running preset on it");
}
} // namespace

int main()
{
    std::cout << std::unitbuf << "\n--- Fast NAM Engine setting ---\n";

    if (!fusedwavenet::IsAvailable() || nammodelcache::IsNamCoreForcedByEnvironment())
    {
        std::cout << "Skipped: no fused kernel in this build, or GUITARFX_NAM_ENGINE=core is set.\n";
        return 0;
    }

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-nam-engine-setting-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);

    TestToggleRebuildsRunningModels(sandbox);
    TestStoredValueAppliesAtStartup(sandbox);

    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << "Fast NAM Engine setting tests failed: " << gFailures << "\n";
        return 1;
    }

    std::cout << "All Fast NAM Engine setting tests passed.\n";
    return 0;
}
