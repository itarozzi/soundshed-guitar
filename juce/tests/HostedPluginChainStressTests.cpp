// How many hosted plugins a chain holds before something gives. Drives a real PluginController
// the way the app does: audio runs paced on its own thread the whole time, and plugins are added
// one at a time through the UI's own addSignalPathNode message, so every add rebuilds the chain
// and instantiates every hosted plugin in it again. Covers a single series chain, a 4-way split
// filled branch by branch, and a preset that arrives already holding N plugins.
//
// Each step prints the live plugin count, how long the add took, the process's memory and what
// the audio thread saw since the last step (blocks, blocks the DSP lock refused, non-finite
// output, the longest block). A crash ends the process and so fails the run.
//
// Not registered with ctest: it loads real plugins from this machine and takes minutes.
//
// Usage: HostedPluginChainStressTests [--max N] [--block B] [--scenario series|ui|split|preset|all]
//                                     [--settle-ms MS] [plugin.vst3 ...]
// With no plugins named, GUITARFX_STRESS_PLUGINS (';'-separated) is used, else every .vst3 in
// the Kilohearts folder. Plugins are reused round-robin when N is larger than the list.

#include "JuceHostedPluginEffect.h"

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "presets/PresetStorage.h"
#include "util/PathEncoding.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if JUCE_WINDOWS
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr auto kSkipCode = 77;
constexpr auto kFailCode = 1;
constexpr auto kSampleRate = 48000.0;

int gBlockSize = 128;
int gMaxPlugins = 24;
int gSettleMs = 400;

class TestHost final : public guitarfx::IPluginHost
{
public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath)) {}

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        // Only errors matter here; the rest is state broadcasts.
        if (jsonMessage.find("\"error\"") != std::string::npos && jsonMessage.size() < 2000)
            lastErrors.push_back(jsonMessage);
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

    // As the app's own host does.
    void RunOnMainThread(std::function<void()> fn) override { juce::MessageManager::callAsync(std::move(fn)); }

    [[nodiscard]] bool IsMessageThread() const override
    {
        const auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        return mm == nullptr || mm->isThisTheMessageThread();
    }

    // The standalone app, so multi-core processing is on as it is there by default, and a split's
    // branches run on helper threads once a level carries enough work.
    [[nodiscard]] bool IsStandalone() const override { return true; }

    [[nodiscard]] fs::path GetUserDataPath() const override { return mUserDataPath; }
    [[nodiscard]] fs::path GetBundledAssetsPath() const override { return mUserDataPath; }
    [[nodiscard]] double GetSampleRate() const override { return kSampleRate; }
    [[nodiscard]] int GetBlockSize() const override { return gBlockSize; }
    void NotifyStateChanged() override {}

    std::vector<std::string> lastErrors;

private:
    fs::path mUserDataPath;
};

/// The host's audio callback: paced to the block's real duration, with a sine in.
class AudioThread
{
public:
    explicit AudioThread(guitarfx::PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~AudioThread()
    {
        mQuit = true;
        mThread.join();
    }

    struct Stats
    {
        long long blocks = 0;
        long long refused = 0;
        long long nonFinite = 0;
        long long overDeadline = 0;
        double longestMs = 0.0;
    };

    Stats TakeStats()
    {
        Stats s;
        s.blocks = mBlocks.exchange(0);
        s.refused = mRefused.exchange(0);
        s.nonFinite = mNonFinite.exchange(0);
        s.overDeadline = mOverDeadline.exchange(0);
        s.longestMs = static_cast<double>(mLongestUs.exchange(0)) / 1000.0;
        return s;
    }

private:
    void Run()
    {
        std::vector<float> inL(static_cast<size_t>(gBlockSize)), inR(inL.size()), outL(inL.size()), outR(inL.size());
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        const auto period = std::chrono::nanoseconds(static_cast<long long>(1.0e9 * gBlockSize / kSampleRate));
        const auto deadlineUs = std::chrono::duration_cast<std::chrono::microseconds>(period).count();
        double phase = 0.0;
        auto next = Clock::now();

        while (!mQuit)
        {
            for (int i = 0; i < gBlockSize; ++i)
            {
                const float v = 0.25f * static_cast<float>(std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 110.0 / kSampleRate;
                inL[static_cast<size_t>(i)] = v;
                inR[static_cast<size_t>(i)] = v;
            }

            const auto start = Clock::now();
            const bool ran = mController.ProcessAudio(inputs, outputs, gBlockSize);
            const auto tookUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();

            ++mBlocks;
            if (!ran)
                ++mRefused;
            if (tookUs > deadlineUs)
                ++mOverDeadline;

            long long prev = mLongestUs.load();
            while (tookUs > prev && !mLongestUs.compare_exchange_weak(prev, tookUs)) {}

            for (int i = 0; i < gBlockSize; ++i)
            {
                if (!std::isfinite(outL[static_cast<size_t>(i)]) || !std::isfinite(outR[static_cast<size_t>(i)]))
                {
                    ++mNonFinite;
                    break;
                }
            }

            next += period;
            const auto now = Clock::now();
            if (next < now)
                next = now;
            std::this_thread::sleep_until(next);
        }
    }

    guitarfx::PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mQuit{false};
    std::atomic<long long> mBlocks{0}, mRefused{0}, mNonFinite{0}, mOverDeadline{0}, mLongestUs{0};
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

double ProcessMemoryMb()
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

int ProcessThreadCount()
{
#if JUCE_WINDOWS
    // Cheap enough per step: a toolhelp snapshot of this process's threads.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int count = 0;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID == pid)
                ++count;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
#else
    return 0;
#endif
}

/// The app's idle loop, with the message loop run in between: async captures, callAsync work
/// and the reaper's main-thread queue all happen here.
void Pump(guitarfx::PluginController& controller, int ms)
{
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(16);
        controller.OnIdle();
    }
}

void SendUi(guitarfx::PluginController& controller, const nlohmann::json& message)
{
    controller.HandleUIMessage(message.dump());
}

guitarfx::GraphNode MakeIoNode(const char* id, const char* type)
{
    guitarfx::GraphNode node;
    node.id = id;
    node.type = type;
    return node;
}

guitarfx::GraphNode MakePluginNode(const std::string& id, const fs::path& plugin)
{
    guitarfx::GraphNode node;
    node.id = id;
    node.type = guitarfx::EffectGuids::kPluginHost;
    node.category = "utility";
    node.label = plugin.stem().string();
    guitarfx::ResourceRef resource;
    resource.resourceType = "plugin";
    resource.filePath = plugin;
    node.resources.push_back(resource);
    return node;
}

void LoadPreset(guitarfx::PluginController& controller, guitarfx::Preset preset)
{
    preset.version = 2;
    preset.category = "Test";
    guitarfx::NormalizePresetScenes(preset);
    SendUi(controller, {{"type", "loadPreset"},
                        {"presetId", preset.id},
                        {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset))}});
}

guitarfx::Preset EmptyPreset(const std::string& id)
{
    guitarfx::Preset preset;
    preset.id = id;
    preset.name = id;
    preset.graph.nodes = {MakeIoNode("__input__", guitarfx::kNodeTypeInput),
                          MakeIoNode("__output__", guitarfx::kNodeTypeOutput)};
    preset.graph.edges = {{"__input__", "__output__", 0, 0, 1.0}};
    return preset;
}

struct ChainCount
{
    int hosts = 0;
    int loaded = 0;
    std::vector<std::string> errors;
};

ChainCount CountHostedPlugins(guitarfx::PluginController& controller)
{
    ChainCount count;
    const auto& preset = controller.GetActivePreset();
    if (!preset)
        return count;

    for (const auto& node : preset->graph.nodes)
    {
        if (node.type != guitarfx::EffectGuids::kPluginHost)
            continue;
        ++count.hosts;
        auto* effect = dynamic_cast<guitarfx::JuceHostedPluginEffect*>(
            controller.GetMixer().GetNodeProcessor(preset->id, node.id));
        if (effect != nullptr && effect->HasResource())
            ++count.loaded;
        else if (effect != nullptr)
            count.errors.push_back(node.label + ": " + effect->GetConfig("lastError"));
        else
            count.errors.push_back(node.label + ": no processor");
    }
    return count;
}

std::set<std::string> NodeIds(guitarfx::PluginController& controller)
{
    std::set<std::string> ids;
    if (const auto& preset = controller.GetActivePreset())
        for (const auto& node : preset->graph.nodes)
            ids.insert(node.id);
    return ids;
}

std::string NewNodeId(const std::set<std::string>& before, guitarfx::PluginController& controller)
{
    for (const auto& id : NodeIds(controller))
        if (!before.count(id))
            return id;
    return {};
}

void PrintHeader(const std::string& scenario)
{
    std::cout << "\n== " << scenario << " (block " << gBlockSize << ", " << kSampleRate / 1000.0 << " kHz) ==\n"
              << "  step  plugins loaded   add ms   mem MB  threads   blocks refused  over  longest ms  nonfinite  plugin\n";
}

/// One line per step. False when the step left the chain in a state worth stopping on.
bool PrintStep(int step, guitarfx::PluginController& controller, AudioThread& audio, double addMs,
               const std::string& pluginName)
{
    const auto count = CountHostedPlugins(controller);
    const auto stats = audio.TakeStats();
    std::cout << std::setw(6) << step << std::setw(9) << count.hosts << std::setw(7) << count.loaded << std::fixed
              << std::setprecision(0) << std::setw(9) << addMs << std::setw(9) << ProcessMemoryMb() << std::setw(9)
              << ProcessThreadCount() << std::setw(9) << stats.blocks << std::setw(8) << stats.refused << std::setw(6)
              << stats.overDeadline << std::setprecision(2) << std::setw(12) << stats.longestMs << std::setw(11)
              << stats.nonFinite << "  " << pluginName << std::endl;

    for (const auto& error : count.errors)
        std::cout << "        not loaded: " << error << std::endl;

    return stats.nonFinite == 0;
}

std::string PluginName(const fs::path& plugin)
{
    return plugin.stem().string();
}

bool RunSeries(guitarfx::PluginController& controller, AudioThread& audio, const std::vector<fs::path>& plugins)
{
    PrintHeader("series: add one at a time");
    LoadPreset(controller, EmptyPreset("stress-series"));
    Pump(controller, gSettleMs);
    audio.TakeStats();

    std::string tail = "__input__";
    bool ok = true;
    for (int i = 0; i < gMaxPlugins; ++i)
    {
        const auto& plugin = plugins[static_cast<size_t>(i) % plugins.size()];
        const auto before = NodeIds(controller);
        const auto start = Clock::now();
        SendUi(controller, {{"type", "addSignalPathNode"},
                            {"effectType", guitarfx::EffectGuids::kPluginHost},
                            {"insertAfter", tail},
                            {"resources", nlohmann::json::array({{{"resourceType", "plugin"},
                                                                   {"filePath", guitarfx::util::PathToUtf8(plugin)}}})}});
        const double addMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto added = NewNodeId(before, controller);
        if (added.empty())
        {
            std::cout << "  add " << (i + 1) << " did not add a node" << std::endl;
            return false;
        }
        tail = added;
        Pump(controller, gSettleMs);
        ok = PrintStep(i + 1, controller, audio, addMs, PluginName(plugin)) && ok;
    }
    return ok;
}

/// The UI's own way in: an empty Plugin Host node is added, then its plugin is picked with the
/// editor opening after the load, and that editor is still open when the next one is added.
bool RunUiFlow(guitarfx::PluginController& controller, AudioThread& audio, const std::vector<fs::path>& plugins)
{
    PrintHeader("ui: add an empty Plugin Host, pick its plugin, editor left open");
    LoadPreset(controller, EmptyPreset("stress-ui"));
    Pump(controller, gSettleMs);
    audio.TakeStats();

    std::string tail = "__input__";
    bool ok = true;
    for (int i = 0; i < gMaxPlugins; ++i)
    {
        const auto& plugin = plugins[static_cast<size_t>(i) % plugins.size()];
        const auto before = NodeIds(controller);
        const auto start = Clock::now();
        SendUi(controller, {{"type", "addSignalPathNode"},
                            {"effectType", guitarfx::EffectGuids::kPluginHost},
                            {"insertAfter", tail}});
        const auto added = NewNodeId(before, controller);
        if (added.empty())
        {
            std::cout << "  add " << (i + 1) << " did not add a node" << std::endl;
            return false;
        }
        tail = added;
        Pump(controller, 50);
        SendUi(controller, {{"type", "updateNodeResource"},
                            {"nodeId", added},
                            {"resourceType", "plugin"},
                            {"resourceId", ""},
                            {"filePath", guitarfx::util::PathToUtf8(plugin)},
                            {"resourceIndex", 0},
                            {"openPluginEditorAfterLoad", true}});
        const double addMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        Pump(controller, gSettleMs);
        ok = PrintStep(i + 1, controller, audio, addMs, PluginName(plugin)) && ok;
    }
    return ok;
}

bool RunSplit(guitarfx::PluginController& controller, AudioThread& audio, const std::vector<fs::path>& plugins)
{
    constexpr int kBranches = 4;
    PrintHeader("split: 4 branches, filled round-robin");
    LoadPreset(controller, EmptyPreset("stress-split"));
    Pump(controller, gSettleMs);

    auto before = NodeIds(controller);
    SendUi(controller, {{"type", "addSignalPathNode"}, {"effectType", guitarfx::EffectGuids::kSplitter},
                        {"insertAfter", "__input__"}});
    std::string splitId, mixId;
    if (const auto& preset = controller.GetActivePreset())
        for (const auto& node : preset->graph.nodes)
        {
            if (before.count(node.id))
                continue;
            if (node.type == guitarfx::EffectGuids::kSplitter)
                splitId = node.id;
            else if (node.type == guitarfx::EffectGuids::kMixer)
                mixId = node.id;
        }
    if (splitId.empty() || mixId.empty())
    {
        std::cout << "  the splitter was not added" << std::endl;
        return false;
    }
    SendUi(controller, {{"type", "setSplitBranchCount"}, {"splitterId", splitId}, {"count", kBranches}});
    Pump(controller, gSettleMs);
    audio.TakeStats();

    // Each branch's last edge into the mixer: a plugin goes in on it, and then leads it.
    struct BranchTail
    {
        std::string from;
        int fromPort;
        int toPort;
    };
    std::vector<BranchTail> tails;
    if (const auto& preset = controller.GetActivePreset())
        for (const auto& edge : preset->graph.edges)
            if (edge.from == splitId && edge.to == mixId)
                tails.push_back({edge.from, edge.fromPort, edge.toPort});
    if (static_cast<int>(tails.size()) != kBranches)
    {
        std::cout << "  expected " << kBranches << " branches, found " << tails.size() << std::endl;
        return false;
    }

    bool ok = true;
    for (int i = 0; i < gMaxPlugins; ++i)
    {
        auto& tail = tails[static_cast<size_t>(i % kBranches)];
        const auto& plugin = plugins[static_cast<size_t>(i) % plugins.size()];
        before = NodeIds(controller);
        const auto start = Clock::now();
        SendUi(controller, {{"type", "addSignalPathNode"},
                            {"effectType", guitarfx::EffectGuids::kPluginHost},
                            {"edge", {{"from", tail.from}, {"to", mixId}, {"fromPort", tail.fromPort}, {"toPort", tail.toPort}}},
                            {"resources", nlohmann::json::array({{{"resourceType", "plugin"},
                                                                   {"filePath", guitarfx::util::PathToUtf8(plugin)}}})}});
        const double addMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto added = NewNodeId(before, controller);
        if (added.empty())
        {
            std::cout << "  add " << (i + 1) << " did not add a node" << std::endl;
            return false;
        }
        tail = {added, 0, tail.toPort};
        Pump(controller, gSettleMs);
        ok = PrintStep(i + 1, controller, audio, addMs,
                       "branch " + std::to_string(i % kBranches + 1) + ": " + PluginName(plugin)) && ok;
    }
    return ok;
}

guitarfx::Preset SeriesPresetOf(const std::string& id, const std::vector<fs::path>& plugins, int n)
{
    auto preset = EmptyPreset(id);
    preset.graph.edges.clear();
    std::string prev = "__input__";
    for (int i = 0; i < n; ++i)
    {
        const std::string nodeId = "plugin-" + std::to_string(i);
        preset.graph.nodes.push_back(MakePluginNode(nodeId, plugins[static_cast<size_t>(i) % plugins.size()]));
        preset.graph.edges.push_back({prev, nodeId, 0, 0, 1.0});
        prev = nodeId;
    }
    preset.graph.edges.push_back({prev, "__output__", 0, 0, 1.0});
    return preset;
}

bool RunPresetLoads(guitarfx::PluginController& controller, AudioThread& audio, const std::vector<fs::path>& plugins)
{
    PrintHeader("preset: load a series chain of N at once, then switch between two");
    bool ok = true;
    for (int n = 4; n <= gMaxPlugins; n += 4)
    {
        const auto start = Clock::now();
        LoadPreset(controller, SeriesPresetOf("stress-preset-" + std::to_string(n), plugins, n));
        const double loadMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        Pump(controller, gSettleMs);
        ok = PrintStep(n, controller, audio, loadMs, "load of " + std::to_string(n)) && ok;
    }

    // Back and forth between two full presets: every switch builds one and retires the other.
    for (int i = 0; i < 6; ++i)
    {
        const int n = (i % 2 == 0) ? gMaxPlugins : gMaxPlugins - 4;
        const auto start = Clock::now();
        LoadPreset(controller, SeriesPresetOf("stress-preset-" + std::to_string(n), plugins, n));
        const double loadMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        Pump(controller, gSettleMs);
        ok = PrintStep(n, controller, audio, loadMs, "switch " + std::to_string(i + 1)) && ok;
    }
    return ok;
}

std::vector<fs::path> DefaultPlugins()
{
    std::vector<fs::path> plugins;
    if (const char* env = std::getenv("GUITARFX_STRESS_PLUGINS"))
    {
        std::string list = env;
        size_t pos = 0;
        while (pos <= list.size())
        {
            const auto end = std::min(list.find(';', pos), list.size());
            if (end > pos)
                plugins.emplace_back(list.substr(pos, end - pos));
            pos = end + 1;
        }
        return plugins;
    }

    const fs::path kilohearts = R"(C:\Program Files\Common Files\VST3\Kilohearts)";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kilohearts, ec))
        if (entry.path().extension() == ".vst3")
            plugins.push_back(entry.path());
    std::sort(plugins.begin(), plugins.end());
    return plugins;
}
} // namespace

int main(int argc, char** argv)
{
    std::string scenario = "all";
    std::vector<fs::path> plugins;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--max" && i + 1 < argc)
            gMaxPlugins = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--block" && i + 1 < argc)
            gBlockSize = std::max(16, std::atoi(argv[++i]));
        else if (arg == "--settle-ms" && i + 1 < argc)
            gSettleMs = std::max(0, std::atoi(argv[++i]));
        else if (arg == "--scenario" && i + 1 < argc)
            scenario = argv[++i];
        else
            plugins.emplace_back(arg);
    }
    if (plugins.empty())
        plugins = DefaultPlugins();

    plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [](const fs::path& p) { return !fs::exists(p); }),
                  plugins.end());
    if (plugins.empty())
    {
        std::cout << "No plugins to host; name some, or set GUITARFX_STRESS_PLUGINS" << std::endl;
        return kSkipCode;
    }

    std::cout << "Hosting up to " << gMaxPlugins << " plugins from " << plugins.size() << ":" << std::endl;
    for (const auto& p : plugins)
        std::cout << "  " << guitarfx::util::PathToUtf8(p) << std::endl;

    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    guitarfx::RegisterJuceHostedPluginEffect();

    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-hosted-plugin-tests" / "chain-stress";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    bool ok = true;
    {
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, gBlockSize);
        Pump(controller, 200);

        {
            AudioThread audio(controller);
            if (scenario == "series" || scenario == "all")
                ok = RunSeries(controller, audio, plugins) && ok;
            if (scenario == "ui" || scenario == "all")
                ok = RunUiFlow(controller, audio, plugins) && ok;
            if (scenario == "split" || scenario == "all")
                ok = RunSplit(controller, audio, plugins) && ok;
            if (scenario == "preset" || scenario == "all")
                ok = RunPresetLoads(controller, audio, plugins) && ok;

            LoadPreset(controller, EmptyPreset("stress-empty"));
            Pump(controller, gSettleMs);
            PrintStep(0, controller, audio, 0.0, "back to an empty chain");
        }
        Pump(controller, 200);
        std::cout << "Tearing down" << std::endl;
    }

    std::cout << (ok ? "Chain stress run finished clean" : "Chain stress run saw non-finite output") << std::endl;
    return ok ? 0 : kFailCode;
}
