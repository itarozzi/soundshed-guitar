#pragma once

// The graph-building helpers both AudioSnapshot passes use, and the chains pass itself: a
// headless standalone host, the chain definitions turned into presets, and one stimulus
// rendered through a real PluginController with a fresh profile. Copied into older checkouts
// with the rest of the harness (see AudioSnapshotSupport.h), so it keeps to the same APIs.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "AudioSnapshotSupport.h"
#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectRegistry.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace audio_snapshot
{
using nlohmann::json;

constexpr double kTempoBpm = 120.0;
constexpr int kChainPrimeTenths = 10; // a second of silence before each chain render

// ---------------------------------------------------------------- resources and graphs

/// Named test resources from the definition, resolved against the assets directory.
struct Resources
{
    std::map<std::string, fs::path> byKey;
    std::map<std::string, std::string> forEffectType; // resourceType ("nam", "ir") -> key

    [[nodiscard]] std::optional<fs::path> Find(const std::string& key) const
    {
        const auto it = byKey.find(key);
        return it == byKey.end() ? std::nullopt : std::optional<fs::path>(it->second);
    }
};

inline Resources LoadResources(const json& def, const fs::path& assets)
{
    Resources r;
    // Named, not iterated in place: a range-for over value(...).items() outlives the temporary.
    const json files = def.value("resources", json::object());
    const json forTypes = def.value("effectResources", json::object());

    for (const auto& [key, rel] : files.items())
    {
        r.byKey[key] = assets / Utf8Path(rel.get<std::string>());
    }

    for (const auto& [type, key] : forTypes.items())
    {
        r.forEffectType[type] = key.get<std::string>();
    }

    return r;
}

inline guitarfx::ResourceRef FileRef(const std::string& resourceType, const fs::path& file)
{
    guitarfx::ResourceRef ref;
    ref.resourceType = resourceType;
    ref.filePath = file;
    return ref;
}

inline guitarfx::GraphNode BoundaryNode(const std::string& id, const char* type)
{
    guitarfx::GraphNode n;
    n.id = id;
    n.type = type;
    n.category = "utility";
    n.enabled = true;
    return n;
}

inline guitarfx::GraphEdge Edge(const std::string& from, const std::string& to, int fromPort = 0, int toPort = 0,
                                double gain = 1.0)
{
    guitarfx::GraphEdge e;
    e.from = from;
    e.to = to;
    e.fromPort = fromPort;
    e.toPort = toPort;
    e.gain = gain;
    return e;
}

inline json ParamsJson(const std::map<std::string, double>& params)
{
    json j = json::object();

    for (const auto& [k, v] : params)
    {
        j[k] = v;
    }

    return j;
}

// ---------------------------------------------------------------- chains pass

/// A standalone host with no UI and no audio device: the controller's messages to the UI are
/// dropped and its main-thread work runs inline, as it does in the workflow tests.
class SnapshotHost final : public guitarfx::IPluginHost
{
  public:
    SnapshotHost(fs::path userData, double sampleRate, int block)
        : mUserData(std::move(userData)), mSampleRate(sampleRate), mBlock(block)
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mUserData;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mUserData / "bundled";
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return mSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return mBlock;
    }

    [[nodiscard]] double GetHostTempo() const override
    {
        return kTempoBpm;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

  private:
    fs::path mUserData;
    double mSampleRate;
    int mBlock;
};

struct ChainBuild
{
    guitarfx::Preset preset;
    json nodes = json::array();
    std::string error;
};

/// Builds a chain's preset. Nodes name their type by alias, so one definition serves every
/// revision; each gets FreshNodeParams unless it sets "freshParams": false, then its own
/// "params" on top. Without "edges" the nodes run in series.
inline ChainBuild BuildChain(const json& chain, const Resources& resources)
{
    auto& registry = guitarfx::EffectRegistry::Instance();
    ChainBuild b;
    b.preset.id = "audio-snapshot-" + chain.at("id").get<std::string>();
    b.preset.name = chain.value("name", chain.at("id").get<std::string>());
    b.preset.category = "Audio Snapshot";
    auto& graph = b.preset.graph;
    graph.nodes.push_back(BoundaryNode("__input__", guitarfx::kNodeTypeInput));
    std::vector<std::string> order = {"__input__"};
    int index = 0;

    for (const auto& spec : chain.at("nodes"))
    {
        const std::string alias = spec.at("type").get<std::string>();
        const std::string type = registry.Resolve(alias);
        const auto info = registry.GetTypeInfo(type);

        if (!info)
        {
            b.error = "unknown effect type '" + alias + "'";
            return b;
        }

        guitarfx::GraphNode node;
        node.id = spec.value("id", "n" + std::to_string(index++) + "_" + EffectSlug(*info));
        node.type = type;
        node.category = info->category;
        node.label = info->displayName;
        node.enabled = spec.value("enabled", true);

        if (spec.value("freshParams", true))
        {
            node.params = FreshNodeParams(*info);
        }

        const json params = spec.value("params", json::object());
        const json config = spec.value("config", json::object());

        for (const auto& [k, v] : params.items())
        {
            node.params[k] = v.get<double>();
        }

        for (const auto& [k, v] : config.items())
        {
            node.config[k] = v.get<std::string>();
        }

        if (spec.contains("resource"))
        {
            const auto key = spec["resource"].get<std::string>();
            const auto file = resources.Find(key);

            if (!file)
            {
                b.error = "unknown resource '" + key + "'";
                return b;
            }

            node.resources.push_back(FileRef(info->resourceType, *file));
        }

        b.nodes.push_back({{"id", node.id},
                           {"alias", alias},
                           {"type", type},
                           {"enabled", node.enabled},
                           {"resource", spec.value("resource", "")},
                           {"params", ParamsJson(node.params)}});
        graph.nodes.push_back(node);
        order.push_back(node.id);
    }

    graph.nodes.push_back(BoundaryNode("__output__", guitarfx::kNodeTypeOutput));
    order.push_back("__output__");

    if (chain.contains("edges"))
    {
        for (const auto& e : chain["edges"])
        {
            graph.edges.push_back(Edge(e.at("from").get<std::string>(), e.at("to").get<std::string>(),
                                       e.value("fromPort", 0), e.value("toPort", 0), e.value("gain", 1.0)));
        }
    }
    else
    {
        for (std::size_t i = 0; i + 1 < order.size(); ++i)
        {
            graph.edges.push_back(Edge(order[i], order[i + 1]));
        }
    }

    return b;
}

/// Turns on the global chain's nodes a chain asks for, at their defaults, the way the Global
/// FX toggles do. Reached through `requires` so a revision without one simply skips it.
template <typename Mixer> std::vector<std::string> ApplyGlobalChain(Mixer& mixer, const json& spec)
{
    std::vector<std::string> applied;
    const auto want = [&](const char* key) { return spec.value(key, false); };

    if constexpr (requires { mixer.SetGlobalGateEnabled(true); })
    {
        if (want("gate"))
        {
            mixer.SetGlobalGateEnabled(true);
            applied.push_back("gate");
        }
    }

    if constexpr (requires { mixer.SetGlobalTransposeEnabled(true); })
    {
        if (want("transpose"))
        {
            mixer.SetGlobalTransposeEnabled(true);
            applied.push_back("transpose");
        }
    }

    if constexpr (requires { mixer.SetGlobalEQEnabled(true); })
    {
        if (want("eq"))
        {
            mixer.SetGlobalEQEnabled(true);
            applied.push_back("eq");
        }
    }

    if constexpr (requires { mixer.SetGlobalDoublerEnabled(true); })
    {
        if (want("doubler"))
        {
            mixer.SetGlobalDoublerEnabled(true);
            applied.push_back("doubler");
        }
    }

    return applied;
}

struct ControllerRender
{
    Render render;
    std::string activePreset;
    std::vector<std::string> globalChain;
};

/// One stimulus through a controller with a profile of its own, which is what makes the
/// settings the app's defaults: nothing has been stored yet.
inline ControllerRender RenderController(const ChainBuild& chain, const json& spec, const Stimulus& stim,
                                         double sampleRate, int block, const fs::path& profile)
{
    std::error_code ec;
    fs::remove_all(profile, ec);
    fs::create_directories(profile / "bundled", ec);
    SetSettingsEnvRoot(profile);

    ControllerRender result;
    {
        SnapshotHost host(profile, sampleRate, block);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(sampleRate, block);
        controller.HandleUIMessage(json{{"type", "loadPreset"},
                                        {"presetId", chain.preset.id},
                                        {"preset", json::parse(guitarfx::PresetStorage::SerializeToJson(chain.preset))}}
                                       .dump());

        const auto& active = controller.GetActivePreset();
        result.activePreset = active ? active->id : std::string{};
        result.globalChain = ApplyGlobalChain(controller.GetMixer(), spec.value("globalChain", json::object()));
        controller.OnIdle();

        // Let the load's fade-in finish and any previous preset's tail die away.
        const auto process = [&](float** in, float** out, int n) { return controller.ProcessAudio(in, out, n); };
        const std::vector<float> tenth(static_cast<std::size_t>(sampleRate * 0.1), 0.0f);

        for (int i = 0; i < kChainPrimeTenths; ++i)
        {
            (void)RenderBlocks(tenth, block, process);
            controller.OnIdle(); // as the editor's timer would
        }

        result.render = RenderBlocks(stim.samples, block, process);
    }

    fs::remove_all(profile, ec);
    return result;
}
} // namespace audio_snapshot
