/**
 * @file HostStateThreadTests.cpp
 * @brief A host may ask for state on any thread; only the message thread may build it.
 *
 * SerializeState reads the working copy, the mixer slot cache, settings and automation, all of
 * which the message thread changes without a lock. It also asks each hosted plugin for its live
 * state after finding it under the DSP lock, which is only safe where the plugin cannot be
 * retired and destroyed in between: on the message thread. JUCE's AAX, AU and LV2 wrappers call
 * getStateInformation on whatever thread the host used, so a call from anywhere else is handed
 * to the message thread (HostStateRelay), and answered from the last blob built there if the
 * message thread does not start on it in time.
 *
 * The host here posts RunOnMainThread() to a queue that this test's main thread, the message
 * thread, pumps between the things it does. A stand-in hosted plugin records which thread asks
 * it for its state, and whether it was already destroyed. An audio thread runs throughout.
 *  - Asked from another thread while the message thread is free, the answer is built there,
 *    with the plugin's live state.
 *  - Asked while the message thread is busy for longer than the wait, the answer is the last
 *    blob built there, and the request is dropped unbuilt when the message thread reaches it.
 *  - That fallback follows the working copy: the idle refresh and a restore both update it,
 *    without asking any plugin.
 *  - Under churn (presets reloaded, a slot added and removed and its plugin destroyed, host state
 *    restored, the message thread going busy), a host thread asking the whole time only ever
 *    gets well-formed state, and no plugin is ever asked off the message thread.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32) && defined(_DEBUG)
    #include <crtdbg.h>
#endif

#include "IPluginHost.h"
#include "PluginController.h"
#include "controller/HostStateRelay.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kPresetA = "host-state-thread-a";
constexpr const char* kPresetB = "host-state-thread-b";
constexpr const char* kSlotC = "host-state-thread-c";
constexpr const char* kPluginNodeId = "plugin";
constexpr const char* kStateKey = "pluginStateBase64";
constexpr const char* kStoredState = "c3RvcmVkLXN0YXRl";
constexpr const char* kLiveState = "bGl2ZS1zdGF0ZQ==";

constexpr auto kOffThreadWait = HostStateRelay::kDefaultOffThreadWait;
// How long the message thread stays busy to make a request give up on it: past the wait, with
// room for a request posted a little after the busy spell began.
constexpr auto kBusySpell = kOffThreadWait + 250ms;
// Slack for a timed wait that wakes a little early or late.
constexpr auto kTimerSlack = 50ms;
// Upper bound on any wait; a hang becomes a failure, not a stuck ctest.
constexpr auto kTimeout = 10s;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

bool WaitForAdvance(const std::atomic<int>& counter, int from, int by, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;

    while (counter.load(std::memory_order_acquire) - from < by)
    {
        if (Clock::now() >= deadline)
        {
            return false;
        }

        std::this_thread::sleep_for(100us);
    }

    return true;
}

// ── Stand-in hosted plugin ───────────────────────────────────────────────────

std::thread::id gMessageThread; // written before the controller exists

bool OnMessageThread()
{
    return std::this_thread::get_id() == gMessageThread;
}

struct Probe
{
    std::atomic<int> stateReads{0};          // a stand-in asked for its live state
    std::atomic<int> offThreadStateReads{0}; // ... anywhere but on the message thread
    std::atomic<int> deadStateReads{0};      // ... after it had been destroyed
    std::atomic<int> destroyed{0};
};

Probe gProbe;

class HostedPluginStandIn final : public EffectProcessor
{
  public:
    ~HostedPluginStandIn() override
    {
        mAlive.store(0, std::memory_order_release);
        gProbe.destroyed.fetch_add(1, std::memory_order_acq_rel);
    }

    void Prepare(double, int) override
    {
    }

    void Reset() override
    {
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
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

    /// Always answers with the "live" chunk, never the one the preset stored, so a blob built
    /// from the running plugin is told apart from one built from the working copy.
    [[nodiscard]] std::string GetConfig(const std::string& key) const override
    {
        if (key != kStateKey)
        {
            return {};
        }

        gProbe.stateReads.fetch_add(1, std::memory_order_acq_rel);

        if (!OnMessageThread())
        {
            gProbe.offThreadStateReads.fetch_add(1, std::memory_order_acq_rel);
        }

        // Best effort: a read after the destructor is undefined, but the Debug heap scribbles.
        if (mAlive.load(std::memory_order_acquire) != kAliveMark)
        {
            gProbe.deadStateReads.fetch_add(1, std::memory_order_acq_rel);
        }

        return kLiveState;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return EffectGuids::kPluginHost;
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

  private:
    static constexpr std::uint32_t kAliveMark = 0x5EEDF00Du;
    std::atomic<std::uint32_t> mAlive{kAliveMark};
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

    /// Posts, as JUCE's callAsync does, even from the message thread itself.
    void RunOnMainThread(std::function<void()> fn) override
    {
        const std::lock_guard<std::mutex> lock(mQueueMutex);
        mQueue.push_back(std::move(fn));
    }

    [[nodiscard]] bool IsMessageThread() const override
    {
        return OnMessageThread();
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

    /// Message thread: runs everything posted so far, and returns how many that was.
    int Pump()
    {
        std::deque<std::function<void()>> tasks;
        {
            const std::lock_guard<std::mutex> lock(mQueueMutex);
            tasks.swap(mQueue);
        }

        for (auto& task : tasks)
        {
            task();
        }

        return static_cast<int>(tasks.size());
    }

  private:
    fs::path mUserDataPath;
    std::mutex mQueueMutex;
    std::deque<std::function<void()>> mQueue;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

/// input -> hosted stand-in -> output, the plugin's saved chunk being kStoredState. `variant`
/// goes into the name, so each reload is a different preset.
Preset BuildPreset(const std::string& id, int variant)
{
    Preset preset;
    preset.id = id;
    preset.name = id + " " + std::to_string(variant);
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
    plugin.config[kStateKey] = kStoredState;

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

void LoadPreset(PluginController& controller, const std::string& id, int variant)
{
    Send(controller, {{"type", "loadPreset"}, {"presetId", id}, {"preset", PresetJson(BuildPreset(id, variant))}});
}

/// The plugin state the focused preset in a host state answer carries, or "<missing>".
std::string PluginStateIn(const nlohmann::json& state)
{
    const auto presetJson = state.is_object() ? state.value("preset", nlohmann::json::object()) : nlohmann::json{};
    const auto preset = PresetStorage::DeserializeFromJson(presetJson.dump());
    const auto* node = preset ? preset->graph.FindNode(kPluginNodeId) : nullptr;

    if (!node || !node->config.count(kStateKey))
    {
        return "<missing>";
    }

    return node->config.at(kStateKey);
}

/// The focused preset's id in a host state answer, or "<none>".
std::string PresetIdIn(const nlohmann::json& state)
{
    return state.is_object() ? state.value("presetId", std::string{"<none>"}) : std::string{"<none>"};
}

/// Well-formed host state: parses, and carries the blocks a restore reads.
bool IsWellFormedState(const nlohmann::json& state)
{
    return state.is_object() && state.value("version", 0) == 1 && state.contains("preset") && state.contains("mixer") &&
           state["mixer"].is_object() && state["mixer"].contains("activePresetIds");
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
        mStop.store(true, std::memory_order_release);
        mThread.join();
    }

    /// Blocks until the mixer has processed `blocks` more blocks; false on timeout.
    [[nodiscard]] bool WaitForBlocks(int blocks) const
    {
        return WaitForAdvance(mProcessed, mProcessed.load(std::memory_order_acquire), blocks, kTimeout);
    }

  private:
    void Run()
    {
        std::vector<float> inL(kBlock, 0.05f), inR(kBlock, 0.05f), outL(kBlock), outR(kBlock);

        while (!mStop.load(std::memory_order_acquire))
        {
            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};

            if (mController.ProcessAudio(inputs, outputs, kBlock))
            {
                mProcessed.fetch_add(1, std::memory_order_acq_rel);
            }

            const auto resume = Clock::now() + 200us;

            while (Clock::now() < resume)
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

// ── Asking from another thread ───────────────────────────────────────────────

struct Answer
{
    bool arrived = false;
    nlohmann::json state;
    std::chrono::milliseconds took{0};
};

/// Asks for state from a thread of its own while this, the message thread, pumps the queue
/// (free) or does not (busy). Waits for the answer either way.
Answer AskFromOtherThread(PluginController& controller, TestHost& host, bool messageThreadFree)
{
    auto future = std::async(std::launch::async, [&controller] {
        const auto start = Clock::now();
        auto text = controller.SerializeState();
        return std::make_pair(std::move(text),
                              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start));
    });

    const auto deadline = Clock::now() + kTimeout;

    while (future.wait_for(1ms) != std::future_status::ready)
    {
        if (Clock::now() >= deadline)
        {
            // The future's destructor would wait on regardless; the watchdog ends it.
            return {};
        }

        if (messageThreadFree)
        {
            host.Pump();
        }
    }

    auto [text, took] = future.get();
    return {true, nlohmann::json::parse(text, nullptr, false), took};
}

/// A host's save or autosave thread: asks for state over and over until stopped, and keeps
/// count of what came back.
class HostThread
{
  public:
    explicit HostThread(PluginController& controller) : mController(controller)
    {
        mThread = std::thread([this] { Run(); });
    }

    ~HostThread()
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

    std::atomic<int> answers{0};
    std::atomic<int> gaveUp{0}; // took the whole wait: answered from the remembered blob
    std::atomic<int> malformed{0};

  private:
    void Run()
    {
        while (!mStop.load(std::memory_order_acquire))
        {
            const auto start = Clock::now();
            const auto text = mController.SerializeState();
            const auto took = Clock::now() - start;
            answers.fetch_add(1, std::memory_order_acq_rel);

            if (took >= kOffThreadWait - kTimerSlack)
            {
                gaveUp.fetch_add(1, std::memory_order_acq_rel);
            }

            if (!IsWellFormedState(nlohmann::json::parse(text, nullptr, false)))
            {
                malformed.fetch_add(1, std::memory_order_acq_rel);
            }

            std::this_thread::sleep_for(1ms);
        }
    }

    PluginController& mController;
    std::thread mThread;
    std::atomic<bool> mStop{false};
};

// ── Tests ────────────────────────────────────────────────────────────────────

/// A free message thread builds the answer, live, and the caller does not sit out the wait.
bool TestFreeMessageThreadBuildsLive(PluginController& controller, TestHost& host, nlohmann::json& saved)
{
    LoadPreset(controller, kPresetA, 0);
    host.Pump();

    const int readsBefore = gProbe.stateReads.load();
    const auto answer = AskFromOtherThread(controller, host, true);
    const std::string label = "free message thread: ";

    if (!Check(answer.arrived, label + "answered"))
    {
        return false;
    }

    bool passed = Check(IsWellFormedState(answer.state), label + "well-formed state");
    passed = Check(answer.took < kOffThreadWait,
                   label + "answered inside the wait (" + std::to_string(answer.took.count()) + " ms)") &&
             passed;
    passed = Check(PresetIdIn(answer.state) == kPresetA, label + "carries the preset just loaded") && passed;
    passed = Check(PluginStateIn(answer.state) == kLiveState, label + "with the plugin's live state") && passed;
    passed = Check(gProbe.stateReads.load() > readsBefore, label + "the plugin was asked") && passed;
    passed = Check(gProbe.offThreadStateReads.load() == 0, label + "on the message thread") && passed;
    saved = answer.state;
    return passed;
}

/// A message thread busy past the wait: the answer is the last blob built there, and the
/// abandoned request builds nothing when the message thread gets to it.
bool TestBusyMessageThreadFallsBack(PluginController& controller, TestHost& host)
{
    // The last blob built is the previous test's: preset A. Loading B changes the working
    // copy without building anything.
    LoadPreset(controller, kPresetB, 0);
    host.Pump();

    const int readsBefore = gProbe.stateReads.load();
    const auto answer = AskFromOtherThread(controller, host, false);
    const std::string label = "busy message thread: ";

    if (!Check(answer.arrived, label + "answered"))
    {
        return false;
    }

    bool passed = Check(IsWellFormedState(answer.state), label + "well-formed state");
    passed = Check(answer.took >= kOffThreadWait - kTimerSlack && answer.took < kOffThreadWait + 2s,
                   label + "gave up after the wait (" + std::to_string(answer.took.count()) + " ms)") &&
             passed;
    passed = Check(PresetIdIn(answer.state) == kPresetA,
                   label + "answered with the last blob the message thread built (" + PresetIdIn(answer.state) + ")") &&
             passed;
    passed = Check(gProbe.stateReads.load() == readsBefore, label + "no plugin was asked meanwhile") && passed;

    const int ran = host.Pump();
    passed = Check(ran >= 1, label + "the abandoned request reached the message thread") && passed;
    passed = Check(gProbe.stateReads.load() == readsBefore, label + "and was dropped without building") && passed;
    return passed;
}

/// The fallback follows the working copy: the idle refresh picks up a change once the refresh
/// interval has passed, and a restore is remembered at once. Neither asks any plugin.
bool TestFallbackFollowsWorkingCopy(PluginController& controller, TestHost& host, const nlohmann::json& savedA)
{
    // The working copy is on B since the previous test; the blob still says A.
    const int readsBefore = gProbe.stateReads.load();
    const auto until = Clock::now() + HostStateRelay::kDefaultRefreshInterval + 250ms;

    while (Clock::now() < until)
    {
        host.Pump();
        controller.OnIdle();
        std::this_thread::sleep_for(10ms);
    }

    bool passed = Check(gProbe.stateReads.load() == readsBefore, "idle refresh: no plugin was asked");

    auto answer = AskFromOtherThread(controller, host, false);
    host.Pump();
    passed = Check(answer.arrived && PresetIdIn(answer.state) == kPresetB,
                   "idle refresh: a busy message thread's answer carries the working copy's preset (" +
                       PresetIdIn(answer.state) + ")") &&
             passed;
    passed = Check(PluginStateIn(answer.state) == kStoredState,
                   "idle refresh: with the plugin state the working copy holds") &&
             passed;

    controller.DeserializeState(savedA.dump());
    host.Pump();
    answer = AskFromOtherThread(controller, host, false);
    host.Pump();
    passed = Check(answer.arrived && PresetIdIn(answer.state) == kPresetA,
                   "restore: remembered straight away, with no idle tick (" + PresetIdIn(answer.state) + ")") &&
             passed;
    passed = Check(gProbe.stateReads.load() == readsBefore, "restore: no plugin was asked") && passed;
    return passed;
}

/// A host thread asks throughout while the message thread reloads presets, adds and removes a
/// slot whose plugin is then destroyed, restores host state, saves on its own account, and now
/// and then stays busy past the wait.
bool TestAskingUnderChurn(PluginController& controller, TestHost& host, AudioThread& audio,
                          const nlohmann::json& savedA)
{
    constexpr int kRounds = 24;
    const int destroyedBefore = gProbe.destroyed.load();
    const int readsBefore = gProbe.stateReads.load();
    const auto savedText = savedA.dump();
    int busySpells = 0;
    bool audioKeptUp = true;

    HostThread asker(controller);

    const auto step = [&](const std::function<void()>& action) {
        action();
        host.Pump();
    };

    for (int round = 0; round < kRounds; ++round)
    {
        step([&] { LoadPreset(controller, round % 2 == 0 ? kPresetA : kPresetB, round + 1); });
        step([&] {
            Send(controller, {{"type", "addActivePreset"},
                              {"presetId", kSlotC},
                              {"preset", PresetJson(BuildPreset(kSlotC, round))}});
        });
        step([&] { controller.OnIdle(); });
        step([&] { Send(controller, {{"type", "removeActivePreset"}, {"presetId", kSlotC}}); });
        // Collects the plugins retired so far, whose fades have finished, and destroys them.
        step([&] { controller.OnIdle(); });

        if (round % 6 == 5)
        {
            step([&] { controller.DeserializeState(savedText); });
        }

        if (round % 8 == 3)
        {
            // A host saving on the message thread.
            step([&] { (void)controller.SerializeState(); });
        }

        if (round % 8 == 7)
        {
            std::this_thread::sleep_for(kBusySpell);
            ++busySpells;
            host.Pump();
        }

        audioKeptUp = audio.WaitForBlocks(1) && audioKeptUp;
    }

    asker.Stop();
    host.Pump();

    const int answers = asker.answers.load();
    const int gaveUp = asker.gaveUp.load();
    const int destroyed = gProbe.destroyed.load() - destroyedBefore;

    bool passed = Check(answers >= kRounds, "churn: the host thread was answered " + std::to_string(answers) +
                                                " times (" + std::to_string(gaveUp) + " from the remembered blob)");
    passed = Check(asker.malformed.load() == 0,
                   "churn: every answer was well-formed (" + std::to_string(asker.malformed.load()) + " not)") &&
             passed;
    passed = Check(answers - gaveUp >= kRounds, "churn: the message thread built most answers") && passed;
    passed = Check(gaveUp >= 1 && gaveUp <= busySpells + 1,
                   "churn: busy spells were answered from the remembered blob (" + std::to_string(gaveUp) + " for " +
                       std::to_string(busySpells) + " spells)") &&
             passed;
    passed = Check(gProbe.stateReads.load() > readsBefore, "churn: live builds asked the plugins") && passed;
    passed = Check(gProbe.offThreadStateReads.load() == 0,
                   "churn: no plugin was asked for its state off the message thread (" +
                       std::to_string(gProbe.offThreadStateReads.load()) + " times)") &&
             passed;
    passed = Check(gProbe.deadStateReads.load() == 0, "churn: no plugin was asked after it was destroyed") && passed;
    passed = Check(destroyed >= kRounds, "churn: plugins were destroyed while the host thread was asking (" +
                                             std::to_string(destroyed) + ")") &&
             passed;
    passed = Check(audioKeptUp, "churn: audio kept running") && passed;
    return passed;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-host-state-thread-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);
    bool passed = true;

    gMessageThread = std::this_thread::get_id();

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        host.Pump();

        AudioThread audio(controller); // stopped before the controller goes

        if (!Check(audio.WaitForBlocks(20), "audio thread is processing the mixer"))
        {
            passed = false;
        }
        else
        {
            nlohmann::json savedA;
            passed = TestFreeMessageThreadBuildsLive(controller, host, savedA) && passed;
            passed = TestBusyMessageThreadFallsBack(controller, host) && passed;
            passed = TestFallbackFollowsWorkingCopy(controller, host, savedA) && passed;
            passed = TestAskingUnderChurn(controller, host, audio, savedA) && passed;
        }

        host.Pump();
    }

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    // A race this test exists to catch can abort the process; show how far it got, and send
    // a Debug CRT assertion to stderr rather than to a dialog nobody will click.
    std::cout << std::unitbuf;
#if defined(_WIN32) && defined(_DEBUG)

    for (const int report : {_CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }

#endif

    // A request nobody answers, or a deadlock, would otherwise hang ctest until its own timeout.
    std::atomic<bool> finished{false};
    std::thread watchdog([&finished] {
        const auto deadline = Clock::now() + 180s;

        while (!finished.load())
        {
            if (Clock::now() >= deadline)
            {
                std::cout << "[FAIL] watchdog: still running after 180 s, most likely deadlocked\n";
                std::_Exit(3);
            }

            std::this_thread::sleep_for(100ms);
        }
    });

    RegisterAllEffects();
    RegisterStandIn();

    const bool passed = Run();
    finished.store(true);
    watchdog.join();
    std::cout << (passed ? "HostStateThreadTests PASSED\n" : "HostStateThreadTests FAILED\n");
    return passed ? 0 : 1;
}
