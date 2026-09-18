/**
 * @file HostRestoreThreadTests.cpp
 * @brief A host may restore state, or change program, on any thread; only the message thread may
 * apply either.
 *
 * DeserializeState replaces what the message thread owns and changes without a lock (the working
 * copy, settings, automation, the mixer's slots), and loads presets and hosted plugins, which
 * belong to it as well. JUCE's AU, AAX and LV2 wrappers restore on whatever thread the host used,
 * and AU and LV2 change program there too. So a call from anywhere else is handed to the message
 * thread (HostStateRelay) and waited for; one the message thread does not start on in time is
 * left queued and applied later, in the order the host asked.
 *
 * The host here posts RunOnMainThread() to a queue that this test's main thread, the message
 * thread, pumps between the things it does. A stand-in hosted plugin records which thread builds
 * and configures it, and reports the latency its preset gives it, so that restoring one preset
 * over the other changes the chain's latency and the controller tells the host. The host takes a
 * lock to hear that, and the thread that restores holds it throughout, as a host thread calling
 * in can: a notification made while that thread waits on the message thread would deadlock, and
 * here waits out a timeout instead and is counted.
 *  - Restored from another thread while the message thread is free: applied there before the
 *    call returns, and the host hears about it only once the caller has been let go.
 *  - While the message thread is busy past the wait: the call returns unapplied, a save meanwhile
 *    answers with the state being restored, and the restore lands, and is reported to the host,
 *    when the message thread gets to it.
 *  - The message thread applies a queued restore before it saves or restores on its own account.
 *  - A program change from another thread goes the same way; the program getters need no handoff.
 *  - Under churn (presets reloaded, a slot added and removed, restores and saves on the message
 *    thread, busy spells, an audio thread processing), a host thread restoring and changing
 *    program throughout never has a plugin built or configured off the message thread, and
 *    every restore it left queued is applied and reported.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(_WIN32) && defined(_DEBUG)
    #include <crtdbg.h>
#endif

#include "MessageThreadTestHost.h"
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
using test::AudioThread;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kPresetA = "host-restore-thread-a";
constexpr const char* kPresetB = "host-restore-thread-b";
constexpr const char* kSlotC = "host-restore-thread-c";
constexpr const char* kPluginNodeId = "plugin";
constexpr const char* kLatencyKey = "standInLatency";
constexpr int kLatencyB = 64; // preset A's plugin reports none

constexpr auto kOffThreadWait = HostStateRelay::kDefaultOffThreadWait;
// How long the message thread stays busy to make a call give up on it: past the wait, with
// room for a call made a little after the busy spell began.
constexpr auto kBusySpell = kOffThreadWait + 250ms;
// Slack for a timed wait that wakes a little early or late.
constexpr auto kTimerSlack = 50ms;
// Upper bound on any wait; a hang becomes a failure, not a stuck ctest.
constexpr auto kTimeout = 10s;
// How long a host notification waits for the restoring thread to let go of the host's lock.
// Longer than any wait the relay makes, so only a notification that would never get it runs out.
constexpr auto kHostLockPatience = kOffThreadWait * 3;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

// ── Stand-in hosted plugin ───────────────────────────────────────────────────

std::thread::id gMessageThread; // written before the controller exists

bool OnMessageThread()
{
    return std::this_thread::get_id() == gMessageThread;
}

struct Probe
{
    std::atomic<int> built{0};               // stand-ins constructed
    std::atomic<int> builtOffThread{0};      // ... anywhere but on the message thread
    std::atomic<int> configuredOffThread{0}; // SetConfig anywhere but on the message thread
};

Probe gProbe;

class HostedPluginStandIn final : public EffectProcessor
{
  public:
    HostedPluginStandIn()
    {
        gProbe.built.fetch_add(1, std::memory_order_acq_rel);

        if (!OnMessageThread())
        {
            gProbe.builtOffThread.fetch_add(1, std::memory_order_acq_rel);
        }
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

    void SetConfig(const std::string& key, const std::string& value) override
    {
        if (!OnMessageThread())
        {
            gProbe.configuredOffThread.fetch_add(1, std::memory_order_acq_rel);
        }

        if (key == kLatencyKey)
        {
            mLatency.store(std::atoi(value.c_str()), std::memory_order_release);
        }
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mLatency.load(std::memory_order_acquire);
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
    std::atomic<int> mLatency{0};
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

/// Records what the controller tells the host, and takes the host's lock to hear it.
class TestHost final : public test::PumpedTestHost
{
  public:
    explicit TestHost(fs::path userDataPath)
        : PumpedTestHost(std::move(userDataPath), gMessageThread, kSampleRate, kBlock)
    {
    }

    void NotifyStateChanged() override
    {
        TakeHostLock();
        stateChangeReports.fetch_add(1, std::memory_order_acq_rel);
    }

    void NotifyLatencyChanged(int) override
    {
        TakeHostLock();
        latencyReports.fetch_add(1, std::memory_order_acq_rel);
    }

    void NotifyDeferredStateRestored() override
    {
        deferredRestoresReported.fetch_add(1, std::memory_order_acq_rel);

        if (!OnMessageThread())
        {
            deferredReportsOffThread.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    /// The calling thread holds the host's lock until ReleaseHostLock().
    void HoldHostLock()
    {
        mHostLockHolder.store(std::this_thread::get_id(), std::memory_order_release);
    }

    void ReleaseHostLock()
    {
        mHostLockHolder.store(std::thread::id{}, std::memory_order_release);
    }

    std::atomic<int> stateChangeReports{0};
    std::atomic<int> latencyReports{0};
    std::atomic<int> blockedHostCalls{0}; // gave up on the host's lock: would have deadlocked
    std::atomic<int> deferredRestoresReported{0};
    std::atomic<int> deferredReportsOffThread{0};

  private:
    /// A host hears from the plugin under the lock it holds while it restores. Its holder may
    /// call in again; any other thread waits for it.
    void TakeHostLock()
    {
        const auto deadline = Clock::now() + kHostLockPatience;

        for (;;)
        {
            const auto holder = mHostLockHolder.load(std::memory_order_acquire);

            if (holder == std::thread::id{} || holder == std::this_thread::get_id())
            {
                return;
            }

            if (Clock::now() >= deadline)
            {
                blockedHostCalls.fetch_add(1, std::memory_order_acq_rel);
                return;
            }

            std::this_thread::sleep_for(100us);
        }
    }

    std::atomic<std::thread::id> mHostLockHolder{};
};

/// input -> hosted stand-in -> output, the stand-in reporting `latency`. `variant` goes into the
/// name, so each reload is a different preset.
Preset BuildPreset(const std::string& id, int variant, int latency)
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
    plugin.config[kLatencyKey] = std::to_string(latency);

    preset.graph.nodes = {in, out, plugin};
    preset.graph.edges = {{"__input__", kPluginNodeId, 0, 0, 1.0}, {kPluginNodeId, "__output__", 0, 0, 1.0}};
    return preset;
}

int LatencyOf(const std::string& id)
{
    return id == kPresetB ? kLatencyB : 0;
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
    Send(controller,
         {{"type", "loadPreset"}, {"presetId", id}, {"preset", PresetJson(BuildPreset(id, variant, LatencyOf(id)))}});
}

/// The focused preset's id in a host state blob, or "<none>".
std::string PresetIdIn(const std::string& state)
{
    const auto json = nlohmann::json::parse(state, nullptr, false);
    return json.is_object() ? json.value("presetId", std::string{"<none>"}) : std::string{"<none>"};
}

/// What the message thread would save now. Applies anything queued first, as a save does.
std::string SavedPresetId(PluginController& controller)
{
    return PresetIdIn(controller.SerializeState());
}

// ── Other threads ────────────────────────────────────────────────────────────

template <typename Result> struct Timed
{
    bool returned = false;
    Result result{};
    std::chrono::milliseconds took{0};
};

/// Runs `call` on a host thread of its own, holding the host's lock, while this, the message
/// thread, pumps the queue (free) or does not (busy). Waits for it either way.
template <typename Fn> auto FromHostThread(TestHost& host, bool messageThreadFree, Fn call)
{
    using Result = decltype(call());

    auto future = std::async(std::launch::async, [&host, call = std::move(call)] {
        host.HoldHostLock();
        const auto start = Clock::now();
        Result result = call();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        host.ReleaseHostLock();
        return std::make_pair(std::move(result), took);
    });

    Timed<Result> timed;
    const auto deadline = Clock::now() + kTimeout;

    while (future.wait_for(1ms) != std::future_status::ready)
    {
        if (Clock::now() >= deadline)
        {
            // The future's destructor would wait on regardless; the watchdog ends it.
            return timed;
        }

        if (messageThreadFree)
        {
            host.Pump();
        }
    }

    auto [result, took] = future.get();
    timed.returned = true;
    timed.result = std::move(result);
    timed.took = took;
    return timed;
}

std::string Ms(std::chrono::milliseconds duration)
{
    return std::to_string(duration.count()) + " ms";
}

bool NothingOffMessageThread(const std::string& label)
{
    return Check(gProbe.builtOffThread.load() == 0 && gProbe.configuredOffThread.load() == 0,
                 label + "no plugin was built or configured off the message thread (" +
                     std::to_string(gProbe.builtOffThread.load()) + " built, " +
                     std::to_string(gProbe.configuredOffThread.load()) + " configured)");
}

// ── Tests ────────────────────────────────────────────────────────────────────

/// A free message thread applies the restore before the call returns, and the host hears of the
/// latency change only once the restoring thread has let go of its lock.
bool TestFreeMessageThreadRestores(PluginController& controller, TestHost& host, const std::string& savedA)
{
    // The working copy is on B, whose plugin reports latency; A's does not.
    const std::string label = "free message thread: ";
    const int builtBefore = gProbe.built.load();
    const int latencyReportsBefore = host.latencyReports.load();

    const auto restore = FromHostThread(host, true, [&] { return controller.DeserializeState(savedA); });

    if (!Check(restore.returned, label + "returned"))
    {
        return false;
    }

    bool passed = Check(restore.result, label + "applied before the call returned");
    passed = Check(restore.took < kOffThreadWait, label + "inside the wait (" + Ms(restore.took) + ")") && passed;
    passed = Check(SavedPresetId(controller) == kPresetA, label + "the restored preset is the working copy") && passed;
    passed = Check(gProbe.built.load() > builtBefore, label + "the restore built its plugin") && passed;
    passed = NothingOffMessageThread(label) && passed;
    passed =
        Check(host.latencyReports.load() > latencyReportsBefore, label + "the host heard the latency change") && passed;
    passed =
        Check(host.blockedHostCalls.load() == 0, label + "only once the restoring thread had been let go") && passed;
    passed = Check(host.deferredRestoresReported.load() == 0, label + "not reported as a deferred restore") && passed;
    return passed;
}

/// A message thread busy past the wait: the call returns unapplied, a save answers with the
/// state being restored, and the restore lands, and is reported, when the message thread is free.
bool TestBusyMessageThreadDefers(PluginController& controller, TestHost& host, const std::string& savedB)
{
    // The working copy is on A since the previous test.
    const std::string label = "busy message thread: ";
    const int builtBefore = gProbe.built.load();
    const int reportedBefore = host.deferredRestoresReported.load();

    const auto restore = FromHostThread(host, false, [&] { return controller.DeserializeState(savedB); });

    if (!Check(restore.returned, label + "returned"))
    {
        return false;
    }

    bool passed = Check(!restore.result, label + "returned unapplied");
    passed = Check(restore.took >= kOffThreadWait - kTimerSlack && restore.took < kOffThreadWait + 2s,
                   label + "after the wait (" + Ms(restore.took) + ")") &&
             passed;
    passed = Check(gProbe.built.load() == builtBefore, label + "nothing was built meanwhile") && passed;

    const auto save = FromHostThread(host, false, [&] { return controller.SerializeState(); });
    passed =
        Check(save.returned && PresetIdIn(save.result) == kPresetB,
              label + "a save meanwhile answers with the state being restored (" + PresetIdIn(save.result) + ")") &&
        passed;

    host.Pump();
    passed = Check(gProbe.built.load() > builtBefore, label + "applied once the message thread got to it") && passed;
    passed = Check(SavedPresetId(controller) == kPresetB, label + "the restored preset is the working copy") && passed;
    passed = Check(host.deferredRestoresReported.load() == reportedBefore + 1,
                   label + "the host was told once it had landed") &&
             passed;
    passed = Check(host.deferredReportsOffThread.load() == 0, label + "on the message thread") && passed;
    passed = NothingOffMessageThread(label) && passed;
    passed = Check(host.blockedHostCalls.load() == 0, label + "no host call waited on the restoring thread") && passed;
    return passed;
}

/// Restores the host queued are applied before the message thread saves or restores on its own
/// account, and only once.
bool TestQueuedRestoresKeepOrder(PluginController& controller, TestHost& host, const std::string& savedA,
                                 const std::string& savedB)
{
    // The working copy is on B since the previous test.
    const std::string label = "order: ";
    const int reportedBefore = host.deferredRestoresReported.load();

    auto restore = FromHostThread(host, false, [&] { return controller.DeserializeState(savedA); });
    bool passed = Check(restore.returned && !restore.result, label + "a restore of A left queued");
    passed =
        Check(SavedPresetId(controller) == kPresetA, label + "a save on the message thread applies it first") && passed;

    restore = FromHostThread(host, false, [&] { return controller.DeserializeState(savedB); });
    passed = Check(restore.returned && !restore.result, label + "a restore of B left queued") && passed;
    (void)controller.DeserializeState(savedA);
    passed = Check(SavedPresetId(controller) == kPresetA,
                   label + "a restore of A on the message thread lands after the queued one") &&
             passed;
    passed = Check(host.deferredRestoresReported.load() == reportedBefore + 2,
                   label + "both queued restores were applied and reported") &&
             passed;

    const int builtBefore = gProbe.built.load();
    host.Pump();
    passed = Check(gProbe.built.load() == builtBefore, label + "their posted tasks find nothing left to do") && passed;
    return passed;
}

/// Program changes: the getters answer on any thread; a change is handed to the message thread
/// like a restore, and left queued while it is busy.
bool TestProgramChanges(PluginController& controller, TestHost& host)
{
    const std::string label = "program change: ";

    for (const auto* id : {kPresetA, kPresetB})
    {
        const auto preset = BuildPreset(id, 100, LatencyOf(id));
        Send(controller,
             {{"type", "savePreset"}, {"presetId", id}, {"name", preset.name}, {"preset", PresetJson(preset)}});
    }

    const auto slots = nlohmann::json::array({{{"presetId", kPresetA}}, {{"presetId", kPresetB}}});
    Send(controller, {{"type", "setSetlists"},
                      {"setlists", nlohmann::json::array({{{"id", "set"}, {"name", "Set"}, {"slots", slots}}})},
                      {"activeSetlistId", "set"},
                      {"cursorIndex", 0}});
    // Saving made B the working copy; a program step onto it would only move the cursor.
    LoadPreset(controller, kPresetA, 200);
    host.Pump();

    // A busy message thread: the getters must not need it.
    const auto length = FromHostThread(host, false, [&] { return controller.GetSetlistLength(); });
    const auto name = FromHostThread(host, false, [&] { return controller.GetSetlistSlotPresetId(1); });
    bool passed = Check(length.result == 2 && name.result == kPresetB && length.took + name.took < kOffThreadWait,
                        label + "the program list reads from a host thread without the message thread");

    const int builtBefore = gProbe.built.load();
    const int stateReportsBefore = host.stateChangeReports.load();
    auto change = FromHostThread(host, true, [&] {
        controller.ApplySetlistPresetByIndex(1);
        return controller.GetSetlistCursorIndex();
    });
    passed = Check(change.returned && change.result == 1, label + "applied before the call returned") && passed;
    passed = Check(SavedPresetId(controller) == kPresetB, label + "the program's preset is the working copy") && passed;
    passed = Check(gProbe.built.load() > builtBefore, label + "its plugin was built") && passed;
    passed = NothingOffMessageThread(label) && passed;
    passed =
        Check(host.stateChangeReports.load() > stateReportsBefore, label + "the host heard of the change") && passed;
    passed = Check(host.blockedHostCalls.load() == 0, label + "once the calling thread had been let go") && passed;

    change = FromHostThread(host, false, [&] {
        controller.ApplySetlistPresetByIndex(0);
        return controller.GetSetlistCursorIndex();
    });
    passed = Check(change.returned && change.result == 1 && change.took >= kOffThreadWait - kTimerSlack,
                   label + "left queued while the message thread was busy (" + Ms(change.took) + ")") &&
             passed;
    host.Pump();
    passed = Check(controller.GetSetlistCursorIndex() == 0 && SavedPresetId(controller) == kPresetA,
                   label + "applied once the message thread got to it") &&
             passed;
    return passed;
}

/// A host thread restores and changes program throughout, while the message thread reloads
/// presets, adds and removes a slot, restores and saves on its own account, and now and then
/// stays busy past the wait.
bool TestRestoringUnderChurn(PluginController& controller, TestHost& host, AudioThread& audio,
                             const std::string& savedA, const std::string& savedB)
{
    constexpr int kRounds = 16;
    const std::string label = "churn: ";
    const int reportedBefore = host.deferredRestoresReported.load();
    std::atomic<bool> stop{false};
    std::atomic<int> applied{0};
    std::atomic<int> deferred{0};
    std::atomic<int> programChanges{0};

    // Without the host's lock: calling again every couple of milliseconds, this thread would hold
    // it nearly all the time, and the message thread's own notifications would sit out the
    // relay's wait on nearly every call. The tests above cover the lock one call at a time.
    std::thread restorer([&] {
        for (int i = 0; !stop.load(std::memory_order_acquire); ++i)
        {
            if (i % 5 == 4)
            {
                controller.ApplySetlistPresetByIndex(i % 2);
                programChanges.fetch_add(1, std::memory_order_acq_rel);
            }
            else
            {
                (controller.DeserializeState(i % 2 == 0 ? savedA : savedB) ? applied : deferred)
                    .fetch_add(1, std::memory_order_acq_rel);
            }

            std::this_thread::sleep_for(2ms);
        }
    });

    const auto step = [&](const std::function<void()>& action) {
        action();
        host.Pump();
    };

    int busySpells = 0;
    bool audioKeptUp = true;

    for (int round = 0; round < kRounds; ++round)
    {
        step([&] { LoadPreset(controller, round % 2 == 0 ? kPresetA : kPresetB, round + 1); });
        step([&] {
            Send(controller, {{"type", "addActivePreset"},
                              {"presetId", kSlotC},
                              {"preset", PresetJson(BuildPreset(kSlotC, round, 0))}});
        });
        step([&] { controller.OnIdle(); });
        step([&] { Send(controller, {{"type", "removeActivePreset"}, {"presetId", kSlotC}}); });
        // Collects the plugins retired so far, whose fades have finished, and destroys them.
        step([&] { controller.OnIdle(); });

        if (round % 4 == 1)
        {
            step([&] { (void)controller.SerializeState(); });
        }

        if (round % 6 == 5)
        {
            step([&] { (void)controller.DeserializeState(round % 2 == 0 ? savedA : savedB); });
        }

        if (round % 4 == 3)
        {
            std::this_thread::sleep_for(kBusySpell);
            ++busySpells;
            host.Pump();
        }

        audioKeptUp = audio.WaitForBlocks(1) && audioKeptUp;
    }

    stop.store(true, std::memory_order_release);

    // The restorer may be waiting on the message thread for its last call.
    auto joined = std::async(std::launch::async, [&restorer] { restorer.join(); });

    while (joined.wait_for(1ms) != std::future_status::ready)
    {
        host.Pump();
    }

    host.Pump();

    bool passed =
        Check(applied.load() >= kRounds, label + "the host thread restored " + std::to_string(applied.load()) +
                                             " times on a free message thread and changed program " +
                                             std::to_string(programChanges.load()) + " times");
    passed = Check(deferred.load() >= 1 && deferred.load() <= busySpells * 2,
                   label + std::to_string(deferred.load()) + " restores were left queued over " +
                       std::to_string(busySpells) + " busy spells") &&
             passed;
    passed = Check(host.deferredRestoresReported.load() - reportedBefore == deferred.load(),
                   label + "every one of them was applied and reported") &&
             passed;
    passed = Check(host.deferredReportsOffThread.load() == 0, label + "reported on the message thread") && passed;
    passed = NothingOffMessageThread(label) && passed;
    passed = Check(audioKeptUp, label + "audio kept running") && passed;

    // Settled: the last word is a host thread's restore, applied there and then.
    const auto last = FromHostThread(host, true, [&] { return controller.DeserializeState(savedB); });
    passed = Check(last.returned && last.result && SavedPresetId(controller) == kPresetB,
                   label + "afterwards a restore from a host thread is the working copy") &&
             passed;
    return passed;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-host-restore-thread-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    bool passed = true;

    gMessageThread = std::this_thread::get_id();

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        host.Pump();

        AudioThread audio(controller, kBlock); // stopped before the controller goes

        if (!Check(audio.WaitForBlocks(20), "audio thread is processing the mixer"))
        {
            passed = false;
        }
        else
        {
            LoadPreset(controller, kPresetA, 0);
            host.Pump();
            const auto savedA = controller.SerializeState();
            LoadPreset(controller, kPresetB, 0);
            host.Pump();
            const auto savedB = controller.SerializeState();

            // The idle broadcast reports the latency B's plugin brings, so that restoring A,
            // which has none, is a change the host has to hear about. It waits for an editor.
            const int latencyReportsBefore = host.latencyReports.load();
            controller.OnWebContentLoaded();
            controller.OnIdle();
            host.Pump();
            passed =
                Check(host.latencyReports.load() > latencyReportsBefore, "the host was told B's latency") && passed;

            passed = TestFreeMessageThreadRestores(controller, host, savedA) && passed;
            passed = TestBusyMessageThreadDefers(controller, host, savedB) && passed;
            passed = TestQueuedRestoresKeepOrder(controller, host, savedA, savedB) && passed;
            passed = TestProgramChanges(controller, host) && passed;
            passed = TestRestoringUnderChurn(controller, host, audio, savedA, savedB) && passed;
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

    // A change nobody applies, or a deadlock, would otherwise hang ctest until its own timeout.
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
    std::cout << (passed ? "HostRestoreThreadTests PASSED\n" : "HostRestoreThreadTests FAILED\n");
    return passed ? 0 : 1;
}
