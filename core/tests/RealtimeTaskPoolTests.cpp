/**
 * @file RealtimeTaskPoolTests.cpp
 * @brief The worker pool behind the mixer's preset fan-out, the graph executor's parallel
 *        levels and the dual-lane executor.
 *
 * The regression this guards: a worker loaded the task count once when it woke, then kept
 * claiming with a bare fetch_add. Stalled past the end of that dispatch, it came back into
 * the next, smaller one, ran an index beyond its count (a stale work item: in the mixer a
 * preset instance that might already be retired) and counted it done, so the audio thread
 * could stop waiting while real work was still running. These tests stall workers before
 * their claims, through the pool's test hook, while dispatches shrink, and check that every
 * index of every dispatch runs exactly once and inside its own Run(), and that the mixer
 * and executor still match serial processing exactly.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dsp/MultiPresetMixer.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

namespace
{
using guitarfx::rtparallel::DualLaneExecutor;
using guitarfx::rtparallel::RealtimeTaskPool;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

std::atomic<int> gClaimAttempts{0};
std::atomic<int> gStalls{0};

void SpinFor(std::chrono::microseconds duration)
{
    const auto until = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < until)
    {
    }
}

// The worker claim hook: stalls about a third of claim attempts for 20-420 us, long enough
// to outlast the rest of the dispatch the worker woke for. That is the window the old pools
// left open. A spin rather than a sleep, which on Windows would be a whole timer tick.
void StallSomeClaims()
{
    thread_local std::minstd_rand rng(
        static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    gClaimAttempts.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t roll = rng();

    if (roll % 3 == 0)
    {
        gStalls.fetch_add(1, std::memory_order_relaxed);
        SpinFor(std::chrono::microseconds(20 + static_cast<int>(roll % 401)));
    }
}

struct StallWorkers
{
    StallWorkers()
    {
        RealtimeTaskPool::SetWorkerClaimHookForTesting(&StallSomeClaims);
    }

    ~StallWorkers()
    {
        RealtimeTaskPool::SetWorkerClaimHookForTesting(nullptr);
    }

    StallWorkers(const StallWorkers&) = delete;
    StallWorkers& operator=(const StallWorkers&) = delete;
};

/// Whether a denormal result flushes to zero on the calling thread.
bool FlushesDenormals()
{
    volatile float tiny = 1.0e-30f;
    volatile float scale = 1.0e-10f;
    const float product = tiny * scale; // 1e-40, below the smallest normal float
    return product == 0.0f;
}

void FillNoise(std::minstd_rand& rng, std::vector<float>& left, std::vector<float>& right)
{
    std::uniform_real_distribution<float> sample(-0.5f, 0.5f);

    for (std::size_t i = 0; i < left.size(); ++i)
    {
        left[i] = sample(rng);
        right[i] = sample(rng);
    }
}

float MaxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    float worst = 0.0f;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }

    return worst;
}

float Peak(const std::vector<float>& buffer)
{
    float peak = 0.0f;

    for (float sample : buffer)
    {
        peak = std::max(peak, std::abs(sample));
    }

    return peak;
}

// ── The pool on its own ──────────────────────────────────────────────────────

struct PoolTally
{
    std::thread::id dispatcher;
    std::atomic<int> outOfRange{0};
    std::atomic<int> afterReturn{0};
    std::atomic<int> onWorkers{0};
    std::atomic<int> workersNotFlushing{0};
};

// One per dispatch, all kept to the end of the test: a task from a stale claim in a broken
// pool then lands on live memory, where it is counted, rather than on a freed context.
struct Dispatch
{
    int count = 0;
    std::array<std::atomic<int>, 8> runs{};
    std::atomic<bool> returned{false};
    PoolTally* tally = nullptr;
};

void RunDispatchTask(void* context, int index)
{
    auto& dispatch = *static_cast<Dispatch*>(context);
    auto& tally = *dispatch.tally;

    if (dispatch.returned.load(std::memory_order_acquire))
    {
        tally.afterReturn.fetch_add(1);
    }

    if (index < 0 || index >= dispatch.count)
    {
        tally.outOfRange.fetch_add(1);
        return;
    }

    if (std::this_thread::get_id() != tally.dispatcher)
    {
        tally.onWorkers.fetch_add(1);

        if (!FlushesDenormals())
        {
            tally.workersNotFlushing.fetch_add(1);
        }
    }

    SpinFor(std::chrono::microseconds(5 + 3 * index));
    dispatch.runs[static_cast<std::size_t>(index)].fetch_add(1);

    if (dispatch.returned.load(std::memory_order_acquire))
    {
        tally.afterReturn.fetch_add(1);
    }
}

bool TestNoStaleClaimsWhileDispatchesShrink()
{
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 0, 4);

    if (workers < 1)
    {
        std::cout << "  (skipped: one hardware thread, so no workers)\n";
        return true;
    }

    std::cout << "  calling thread flushes denormals: " << (FlushesDenormals() ? "yes" : "no")
              << " (the workers must, either way)\n";

    RealtimeTaskPool pool;
    pool.Start(workers);

    constexpr int kDispatches = 4000;
    PoolTally tally;
    tally.dispatcher = std::this_thread::get_id();
    std::vector<std::unique_ptr<Dispatch>> dispatches;
    dispatches.reserve(kDispatches);
    int incompleteOnReturn = 0;
    const int stallsBefore = gStalls.load();

    {
        StallWorkers stall;

        for (int d = 0; d < kDispatches; ++d)
        {
            auto& dispatch = *dispatches.emplace_back(std::make_unique<Dispatch>());
            dispatch.count = 8 - (d % 8); // 8, 7, ... 1: every step but the wrap shrinks
            dispatch.tally = &tally;

            pool.Run(&RunDispatchTask, &dispatch, dispatch.count);

            for (int i = 0; i < dispatch.count; ++i)
            {
                if (dispatch.runs[static_cast<std::size_t>(i)].load() != 1)
                {
                    ++incompleteOnReturn;
                }
            }

            dispatch.returned.store(true, std::memory_order_release);
        }

        // Joins the workers, so any straggler has finished before the recount below.
        pool.Stop();
    }

    int wrongRunCount = 0;

    for (const auto& dispatch : dispatches)
    {
        for (int i = 0; i < dispatch->count; ++i)
        {
            if (dispatch->runs[static_cast<std::size_t>(i)].load() != 1)
            {
                ++wrongRunCount;
            }
        }
    }

    const int stalls = gStalls.load() - stallsBefore;
    std::cout << "  " << kDispatches << " dispatches, " << workers << " workers: " << tally.onWorkers.load()
              << " tasks ran on workers, " << stalls << " claim stalls\n";

    bool ok = true;
    const auto expectZero = [&ok](const char* what, int value) {
        if (value != 0)
        {
            std::cerr << "  " << what << ": " << value << "\n";
            ok = false;
        }
    };

    expectZero("tasks run with an index beyond their dispatch's count", tally.outOfRange.load());
    expectZero("tasks still running after their Run() returned", tally.afterReturn.load());
    expectZero("indices not run exactly once by the time Run() returned", incompleteOnReturn);
    expectZero("indices not run exactly once in the end", wrongRunCount);
    expectZero("tasks on workers without flush-to-zero", tally.workersNotFlushing.load());

    if (tally.onWorkers.load() == 0 || stalls == 0)
    {
        std::cerr << "  the workers never ran a task or never stalled, so nothing was tested\n";
        ok = false;
    }

    return ok;
}

// ── The dual-lane executor ───────────────────────────────────────────────────

bool TestDualLaneStartsFromPrepareOnly()
{
    auto& lanes = DualLaneExecutor::Instance();
    bool ok = true;

    // Nothing in this process has prepared an effect that uses it, so Run() must decline
    // rather than start the helper itself.
    int ran = 0;
    const bool ranBeforeStart = lanes.Run([&ran] { ++ran; }, [&ran] { ++ran; });

    if (ranBeforeStart || ran != 0 || lanes.IsAvailable())
    {
        std::cerr << "  Run() ran or started the helper before EnsureStarted()\n";
        ok = false;
    }

    DualLaneExecutor::EnsureStarted();
    const bool helperExpected = guitarfx::rtparallel::kParallelDspSupported && std::thread::hardware_concurrency() >= 2;

    if (lanes.IsAvailable() != helperExpected)
    {
        std::cerr << "  EnsureStarted() left the helper " << (lanes.IsAvailable() ? "running" : "stopped") << "\n";
        return false;
    }

    if (!helperExpected)
    {
        return ok;
    }

    constexpr int kRuns = 2000;
    const auto caller = std::this_thread::get_id();
    std::atomic<int> mainLane{0};
    std::atomic<int> workerLane{0};
    std::atomic<int> onHelper{0};
    std::atomic<int> helperNotFlushing{0};
    int declined = 0;

    {
        StallWorkers stall;

        for (int i = 0; i < kRuns; ++i)
        {
            const bool ranBoth = lanes.Run(
                [&] {
                    workerLane.fetch_add(1);

                    if (std::this_thread::get_id() != caller)
                    {
                        onHelper.fetch_add(1);
                        helperNotFlushing.fetch_add(FlushesDenormals() ? 0 : 1);
                    }
                },
                [&] {
                    SpinFor(std::chrono::microseconds(50));
                    mainLane.fetch_add(1);
                });

            declined += ranBoth ? 0 : 1;
        }
    }

    std::cout << "  " << kRuns << " runs, " << onHelper.load() << " worker lanes on the helper\n";

    if (declined != 0 || mainLane.load() != kRuns || workerLane.load() != kRuns)
    {
        std::cerr << "  declined " << declined << ", main lane ran " << mainLane.load() << ", worker lane ran "
                  << workerLane.load() << " of " << kRuns << "\n";
        ok = false;
    }

    if (onHelper.load() == 0 || helperNotFlushing.load() != 0)
    {
        std::cerr << "  the helper ran " << onHelper.load() << " lanes, " << helperNotFlushing.load()
                  << " of them without flush-to-zero\n";
        ok = false;
    }

    return ok;
}

// ── The graph executor ───────────────────────────────────────────────────────

guitarfx::GraphNode MakeDelayNode(const std::string& id, double timeMs, double feedback)
{
    guitarfx::GraphNode node{id, "delay_digital", "delay", "Delay", true};
    node.params["time"] = timeMs;
    node.params["feedback"] = feedback;
    node.params["mix"] = 0.5;
    return node;
}

// in -> six delays -> three mixers -> three delays -> mixer -> out. The six and the three
// delays are the parallel levels, so every block dispatches six nodes and then three; the
// mixers between them are too light to fan out.
guitarfx::SignalGraph MakeShrinkingLevelsGraph()
{
    using namespace guitarfx;
    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});

    for (int i = 0; i < 6; ++i)
    {
        const std::string wide = "wide" + std::to_string(i);
        const std::string pair = "pair" + std::to_string(i / 2);
        graph.nodes.push_back(MakeDelayNode(wide, 3.0 + 1.5 * i, 0.6));
        graph.edges.push_back({"in", wide, 0, 0, 1.0});
        graph.edges.push_back({wide, pair, 0, 0, 1.0});
    }

    for (int i = 0; i < 3; ++i)
    {
        const std::string pair = "pair" + std::to_string(i);
        const std::string narrow = "narrow" + std::to_string(i);
        graph.nodes.push_back({pair, kNodeTypeMixer, "", "Mixer", true});
        graph.nodes.push_back(MakeDelayNode(narrow, 5.0 + 2.0 * i, 0.5));
        graph.edges.push_back({pair, narrow, 0, 0, 1.0});
        graph.edges.push_back({narrow, "sum", 0, 0, 1.0});
    }

    graph.nodes.push_back({"sum", kNodeTypeMixer, "", "Mixer", true});
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"sum", "out", 0, 0, 1.0});
    return graph;
}

bool TestExecutorMatchesSerialWhileLevelsShrink()
{
    using namespace guitarfx;
    const SignalGraph graph = MakeShrinkingLevelsGraph();

    SignalGraphExecutor parallel;
    SignalGraphExecutor serial;
    parallel.SetGraph(graph);
    serial.SetGraph(graph);
    parallel.Prepare(kSampleRate, kBlock);
    serial.Prepare(kSampleRate, kBlock);
    serial.SetParallelLevelsEnabled(false);

    std::minstd_rand rng(1234);
    std::vector<float> inL(kBlock), inR(kBlock);
    std::vector<float> parL(kBlock), parR(kBlock), serL(kBlock), serR(kBlock);
    float* inputs[2] = {inL.data(), inR.data()};
    float* parallelOut[2] = {parL.data(), parR.data()};
    float* serialOut[2] = {serL.data(), serR.data()};

    constexpr int kBlocks = 400;
    float worst = 0.0f;
    float peak = 0.0f;
    const int attemptsBefore = gClaimAttempts.load();

    {
        StallWorkers stall;

        for (int block = 0; block < kBlocks; ++block)
        {
            FillNoise(rng, inL, inR);
            // Each executor gets its own copy: the input node reads it in place.
            std::vector<float> copyL = inL, copyR = inR;
            float* serialIn[2] = {copyL.data(), copyR.data()};
            parallel.Process(inputs, parallelOut, kBlock);
            serial.Process(serialIn, serialOut, kBlock);
            worst = std::max({worst, MaxDifference(parL, serL), MaxDifference(parR, serR)});
            peak = std::max(peak, Peak(parL));
        }
    }

    const int attempts = gClaimAttempts.load() - attemptsBefore;
    std::cout << "  " << kBlocks << " blocks: largest parallel/serial difference " << worst << ", peak " << peak << ", "
              << attempts << " worker claim attempts\n";

    bool ok = true;

    if (worst > 1.0e-6f)
    {
        std::cerr << "  the parallel levels diverged from serial processing\n";
        ok = false;
    }

    if (peak <= 0.0f || attempts == 0)
    {
        std::cerr << "  the graph was silent or never reached the level workers, so nothing was tested\n";
        ok = false;
    }

    return ok;
}

// ── The mixer ────────────────────────────────────────────────────────────────

guitarfx::Preset MakeDelayPreset(const std::string& id, double timeMs)
{
    using namespace guitarfx;
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    preset.graph.nodes.push_back(MakeDelayNode("echo", timeMs, 0.6));
    preset.graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    preset.graph.edges.push_back({"in", "echo", 0, 0, 1.0});
    preset.graph.edges.push_back({"echo", "out", 0, 0, 1.0});
    return preset;
}

bool TestMixerMatchesSerialWhilePresetsDrop()
{
    using namespace guitarfx;
    ResourceLibrary library;
    MultiPresetMixer parallel;
    MultiPresetMixer serial;
    constexpr int kRigs = 6;

    for (MultiPresetMixer* mixer : {&parallel, &serial})
    {
        mixer->SetResourceLibrary(&library);
        mixer->Prepare(kSampleRate, kBlock);

        for (int i = 0; i < kRigs; ++i)
        {
            const std::string id = "rig" + std::to_string(i);

            if (!mixer->AddActivePreset(MakeDelayPreset(id, 2.0 + i), id, id))
            {
                std::cerr << "  could not add " << id << "\n";
                return false;
            }
        }
    }

    parallel.SetMultiThreadedProcessingEnabled(true);
    serial.SetMultiThreadedProcessingEnabled(false);

    std::minstd_rand rng(99);
    std::vector<float> inL(kBlock), inR(kBlock);
    std::vector<float> parL(kBlock), parR(kBlock), serL(kBlock), serR(kBlock);
    float* parallelOut[2] = {parL.data(), parR.data()};
    float* serialOut[2] = {serL.data(), serR.data()};

    constexpr int kBlocks = 400;
    float worst = 0.0f;
    float peak = 0.0f;
    const int attemptsBefore = gClaimAttempts.load();

    {
        StallWorkers stall;

        for (int block = 0; block < kBlocks; ++block)
        {
            // Six presets, then three: the fan-out shrinks every other block.
            const bool thin = (block % 2) == 1;

            for (int i = kRigs / 2; i < kRigs; ++i)
            {
                parallel.SetPresetMute("rig" + std::to_string(i), thin);
                serial.SetPresetMute("rig" + std::to_string(i), thin);
            }

            FillNoise(rng, inL, inR);
            std::vector<float> copyL = inL, copyR = inR;
            float* parallelIn[2] = {inL.data(), inR.data()};
            float* serialIn[2] = {copyL.data(), copyR.data()};
            parallel.Process(parallelIn, parallelOut, kBlock);
            serial.Process(serialIn, serialOut, kBlock);
            worst = std::max({worst, MaxDifference(parL, serL), MaxDifference(parR, serR)});
            peak = std::max(peak, Peak(parL));
        }
    }

    const int attempts = gClaimAttempts.load() - attemptsBefore;
    std::cout << "  " << kBlocks << " blocks: largest parallel/serial difference " << worst << ", peak " << peak << ", "
              << attempts << " worker claim attempts\n";

    bool ok = true;

    if (worst > 1.0e-6f)
    {
        std::cerr << "  the preset fan-out diverged from serial processing\n";
        ok = false;
    }

    if (peak <= 0.0f || attempts == 0)
    {
        std::cerr << "  the mix was silent or never reached the preset workers, so nothing was tested\n";
        ok = false;
    }

    return ok;
}
} // namespace

int main()
{
    std::cout << "========================================\n";
    std::cout << "RealtimeTaskPool Tests\n";
    std::cout << "========================================\n";

    int failed = 0;
    const auto run = [&failed](const char* name, bool (*test)()) {
        std::cout << name << "\n";
        const bool ok = test();
        std::cout << (ok ? "  PASS\n" : "  FAIL\n");
        failed += ok ? 0 : 1;
    };

    // First, before anything in this process can have prepared an effect that starts it.
    run("Dual-lane helper starts from Prepare, never from Run", TestDualLaneStartsFromPrepareOnly);
    // Before this thread flushes denormals, so the workers are seen to set it themselves.
    run("Pool: no stale claims while dispatches shrink under stalled workers", TestNoStaleClaimsWhileDispatchesShrink);

    // As the host's audio callback does (juce::ScopedNoDenormals), so serial processing on
    // this thread and parallel processing on the workers run under the same float mode.
    guitarfx::rtparallel::FlushDenormalsOnThisThread();
    guitarfx::RegisterAllEffects();

    run("Executor: parallel levels match serial while they shrink", TestExecutorMatchesSerialWhileLevelsShrink);
    run("Mixer: preset fan-out matches serial while presets drop out", TestMixerMatchesSerialWhilePresetsDrop);

    std::cout << (failed == 0 ? "All tests passed\n" : "Some tests FAILED\n");
    return failed == 0 ? 0 : 1;
}
