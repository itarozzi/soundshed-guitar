/**
 * NAM stereo dual-lane benchmark
 *
 * The trade a stereo NAM amp makes when it hands the right channel's model to the dual-lane
 * helper (rtparallel::DualLaneExecutor) beside the left on the calling thread, against both on
 * the calling thread; rtparallel::StereoLaneGate decides it in the app. Two models called the way
 * the NAM amp effect calls them, then the effect itself in a stereo graph, both at 48 kHz over
 * block sizes 16-512, on the fused WaveNet pass and on the NAM core.
 *
 * Paced runs sleep out each block's period, as a host's audio thread does between callbacks, so
 * the helper is asleep when each block starts; back-to-back runs keep it warm. docs/signal-chain.md
 * (Threads and Silence) has the numbers this produced.
 *
 * Usage: NAMDualLaneBenchmark [--direct | --effect] [--fused | --core] [--paced | --back-to-back]
 *                             [--blocks 64,128,...] [--placement] [model.nam ...]
 *   With no model, a standard (16/8) and an A2 model from testdata. --direct or --effect runs
 *   only that half; --fused or --core one engine; --paced or --back-to-back one pacing.
 *   --placement (Windows) adds a line per row: how often the helper ran on another physical
 *   core, on the calling thread's SMT sibling or on its very CPU, each case's mean block time,
 *   and the CPUs both threads ran on most.
 */

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
    #endif
#else
    #include <ctime>
#endif

#include "dsp/EffectRegistry.h"
#include "dsp/NamModelCache.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include "NAM/activations.h"
#include "NAM/dsp.h"

#include "CpuPlacement.h"

namespace fs = std::filesystem;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// The NAM amp effect (amp_nam and amp_nam_optimized are aliases of it).
constexpr const char* kAmpType = "amp_nam";

const char* EngineName(bool fused)
{
    return fused ? "fused WaveNet" : "NAM core";
}

void GenerateGuitarLikeSignal(std::vector<float>& buffer, double sampleRate)
{
    // Generate a guitar-like signal with multiple harmonics
    std::random_device rd;
    std::mt19937 gen(42); // Fixed seed for reproducibility
    std::uniform_real_distribution<float> noise(-0.01f, 0.01f);

    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
        double t = static_cast<double>(i) / sampleRate;
        // Fundamental (low E, ~82 Hz) + harmonics
        double signal = 0.0;
        signal += 0.5 * std::sin(2.0 * kPi * 82.0 * t);
        signal += 0.3 * std::sin(2.0 * kPi * 164.0 * t);
        signal += 0.15 * std::sin(2.0 * kPi * 246.0 * t);
        signal += 0.08 * std::sin(2.0 * kPi * 328.0 * t);
        signal += noise(gen);                         // Add slight noise
        buffer[i] = static_cast<float>(signal * 0.3); // Keep amplitude reasonable
    }
}

guitarfx::Preset MakeNamPreset(const fs::path& modelPath, const char* ampType)
{
    guitarfx::Preset preset;
    preset.id = "benchmark";
    preset.name = "benchmark";
    preset.version = 2;

    guitarfx::GraphNode input;
    input.id = "input";
    input.type = guitarfx::kNodeTypeInput;
    input.category = "routing";

    guitarfx::GraphNode amp;
    amp.id = "amp";
    amp.type = ampType;
    amp.category = "amp";
    amp.enabled = true;
    guitarfx::ResourceRef ref;
    ref.resourceType = "nam";
    ref.filePath = modelPath;
    amp.resources.push_back(ref);

    guitarfx::GraphNode output;
    output.id = "output";
    output.type = guitarfx::kNodeTypeOutput;
    output.category = "routing";

    preset.graph.nodes = {input, amp, output};
    preset.graph.edges = {{input.id, amp.id, 0, 0, 1.0}, {amp.id, output.id, 0, 0, 1.0}};

    return preset;
}

// Process CPU time (every thread, the helper included), in microseconds.
double ProcessCpuUs()
{
#if defined(_WIN32)
    static const double cyclesPerUs = [] {
        ULONG64 startCycles = 0;
        ULONG64 endCycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &startCycles);
        const auto start = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(200))
        {
        }

        QueryThreadCycleTime(GetCurrentThread(), &endCycles);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        return static_cast<double>(endCycles - startCycles) / us;
    }();

    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return static_cast<double>(cycles) / cyclesPerUs;
#else
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) / 1e3;
#endif
}

// --placement: which logical CPU the calling thread and the helper ran each dual block on.
bool gPlacement = false;

using guitarfx::test::CpuTopology;
using guitarfx::test::CurrentCpu;

// The effect pass, where the lanes are the effect's own: the claim hook runs on the helper
// before each claim attempt, so it sees the helper's CPU, and it runs twice in a block where
// the helper took the right lane (the claim, then finding nothing left) but once where the
// calling thread had already taken it.
std::atomic<int> gHelperCpu{-1};
std::atomic<int> gHelperClaimAttempts{0};

void RecordHelperCpu()
{
    int expected = -1;
    gHelperCpu.compare_exchange_strong(expected, CurrentCpu(), std::memory_order_relaxed);
    gHelperClaimAttempts.fetch_add(1, std::memory_order_relaxed);
}

// Sleeps out what is left of a block period.
class BlockPacer
{
  public:
    BlockPacer()
    {
#if defined(_WIN32)
        mTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    }

    ~BlockPacer()
    {
#if defined(_WIN32)
        if (mTimer)
        {
            CloseHandle(mTimer);
        }
#endif
    }

    BlockPacer(const BlockPacer&) = delete;
    BlockPacer& operator=(const BlockPacer&) = delete;

    void WaitOut(std::chrono::steady_clock::time_point blockStart, double periodUs)
    {
        const double leftUs =
            periodUs - std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - blockStart).count();

        if (leftUs <= 0.0)
        {
            return;
        }

#if defined(_WIN32)
        if (mTimer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(leftUs * 10.0); // relative, in 100 ns units
            SetWaitableTimer(mTimer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(mTimer, INFINITE);
            return;
        }
#endif
        std::this_thread::sleep_for(std::chrono::duration<double, std::micro>(leftUs));
    }

  private:
#if defined(_WIN32)
    HANDLE mTimer = nullptr;
#endif
};

struct LaneStats
{
    std::vector<double> blockUs;
    double cpuUs = 0.0;
    int blocks = 0;
    int helperRanRight = 0;
    std::vector<double> helperStartUs; // dispatch to the helper starting the right lane
    // --placement, one per dual block: the calling thread's CPU at dispatch, and the helper's
    // when it took the right lane (-1 if it did not).
    std::vector<int> callerCpu;
    std::vector<int> helperCpu;

    [[nodiscard]] double Mean() const
    {
        return blockUs.empty() ? 0.0 : std::accumulate(blockUs.begin(), blockUs.end(), 0.0) / blockUs.size();
    }

    [[nodiscard]] double CpuPerBlock() const
    {
        return blocks > 0 ? cpuUs / blocks : 0.0;
    }
};

double Percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }

    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

// Runs `blocks` blocks of `processBlock` into `stats`, pacing them to real time when asked.
template <typename Fn>
void TimeBlocks(Fn&& processBlock, int blocks, double periodUs, bool paced, BlockPacer& pacer, LaneStats& stats)
{
    const double cpuStart = ProcessCpuUs();

    for (int b = 0; b < blocks; ++b)
    {
        const auto start = std::chrono::steady_clock::now();
        processBlock();
        const auto end = std::chrono::steady_clock::now();
        stats.blockUs.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        if (paced)
        {
            pacer.WaitOut(start, periodUs);
        }
    }

    stats.cpuUs += ProcessCpuUs() - cpuStart;
    stats.blocks += blocks;
}

// Blocks per measurement: about three seconds of audio, and never fewer than 600.
int SweepBlockCount(int blockSize, double sampleRate)
{
    return std::max(600, static_cast<int>(3.0 * sampleRate / blockSize));
}

void PrintLaneHeader()
{
    std::cout
        << "  block | serial mean    p99 |   dual mean    p99 | wall  | cpu serial   dual | helper ran  start p50\n";
}

// helperTracked: the dual rows came from SweepModelDirect, which records who ran the right lane.
void PrintLaneRow(int blockSize, const LaneStats& serial, const LaneStats& dual, bool helperTracked)
{
    const double gain = 100.0 * (serial.Mean() - dual.Mean()) / serial.Mean();
    std::cout << "  " << std::setw(5) << blockSize << " | " << std::fixed << std::setprecision(1) << std::setw(11)
              << serial.Mean() << std::setw(7) << Percentile(serial.blockUs, 0.99) << " | " << std::setw(11)
              << dual.Mean() << std::setw(7) << Percentile(dual.blockUs, 0.99) << " | " << std::showpos << std::setw(4)
              << std::setprecision(0) << gain << "%" << std::noshowpos << " | " << std::setw(10) << std::setprecision(1)
              << serial.CpuPerBlock() << std::setw(7) << dual.CpuPerBlock() << " | ";

    if (helperTracked)
    {
        std::cout << std::setw(9) << std::setprecision(0)
                  << (dual.blocks > 0 ? 100.0 * dual.helperRanRight / dual.blocks : 0.0) << "%" << std::setw(10)
                  << std::setprecision(1) << Percentile(dual.helperStartUs, 0.5) << "\n";
    }
    else
    {
        std::cout << std::setw(10) << "-" << std::setw(10) << "-" << "\n";
    }
}

// --placement: where the helper ran against the calling thread, and the dual block's mean
// wall time for each case.
void PrintPlacementRow(const LaneStats& dual)
{
    static const CpuTopology topology;

    if (dual.callerCpu.size() != dual.blockUs.size() || dual.blockUs.empty())
    {
        return;
    }

    enum
    {
        kOtherCore,
        kSibling,
        kSameCpu,
        kCallerOnly,
        kKinds
    };

    static const char* const kNames[kKinds] = {"other core", "SMT sibling", "same CPU", "caller only"};
    int count[kKinds] = {};
    double sumUs[kKinds] = {};
    std::vector<int> callerHistogram;
    std::vector<int> helperHistogram;

    for (std::size_t i = 0; i < dual.blockUs.size(); ++i)
    {
        const int caller = dual.callerCpu[i];
        const int helper = dual.helperCpu[i];
        const int kind = helper < 0                                           ? kCallerOnly
                         : helper == caller                                   ? kSameCpu
                         : topology.CoreOf(helper) == topology.CoreOf(caller) ? kSibling
                                                                              : kOtherCore;
        count[kind]++;
        sumUs[kind] += dual.blockUs[i];

        for (const auto& [cpu, histogram] : {std::pair{caller, &callerHistogram}, std::pair{helper, &helperHistogram}})
        {
            if (cpu >= 0)
            {
                histogram->resize(std::max(histogram->size(), static_cast<std::size_t>(cpu) + 1), 0);
                (*histogram)[static_cast<std::size_t>(cpu)]++;
            }
        }
    }

    const auto top = [](const std::vector<int>& histogram) {
        std::string text;
        std::vector<int> order(histogram.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return histogram[a] > histogram[b]; });
        const int total = std::accumulate(histogram.begin(), histogram.end(), 0);

        for (std::size_t i = 0; i < order.size() && i < 3 && histogram[order[i]] > 0; ++i)
        {
            text += (i ? " " : "") + std::to_string(order[i]) + ":" +
                    std::to_string(static_cast<int>(std::lround(100.0 * histogram[order[i]] / total))) + "%";
        }

        return text;
    };

    std::cout << "         placement:";

    for (int kind = 0; kind < kKinds; ++kind)
    {
        if (count[kind] > 0)
        {
            std::cout << " " << kNames[kind] << " " << std::fixed << std::setprecision(0)
                      << 100.0 * count[kind] / static_cast<double>(dual.blockUs.size()) << "% (" << std::setprecision(1)
                      << sumUs[kind] / count[kind] << " µs);";
        }
    }

    std::cout << " caller cpu " << top(callerHistogram) << ", helper cpu " << top(helperHistogram) << "\n";
}

// Two model instances, one per channel, called the way the NAM amp effect's ProcessModels does.
void SweepModelDirect(const fs::path& modelPath, bool fused, const std::vector<int>& blockSizes, bool paced,
                      BlockPacer& pacer)
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kRounds = 4; // serial and dual alternate, so drift lands on both

    std::cout << "\n"
              << modelPath.filename().string() << ", " << EngineName(fused) << ", direct, "
              << (paced ? "paced" : "back to back") << " (µs per stereo block)\n";
    PrintLaneHeader();

    guitarfx::nammodelcache::SetFusedEngineEnabled(fused);
    auto modelLeft = guitarfx::nammodelcache::GetModel(modelPath);
    auto modelRight = guitarfx::nammodelcache::GetModel(modelPath);
    guitarfx::nammodelcache::SetFusedEngineEnabled(true);

    if (!modelLeft || !modelRight)
    {
        std::cerr << "ERROR: Failed to load model: " << modelPath << "\n";
        return;
    }

    auto& lanes = guitarfx::rtparallel::DualLaneExecutor::Instance();
    const int maxBlock = *std::max_element(blockSizes.begin(), blockSizes.end());
    std::vector<float> signal(static_cast<std::size_t>(maxBlock));
    GenerateGuitarLikeSignal(signal, kSampleRate);
    std::vector<NAM_SAMPLE> inL(signal.begin(), signal.end());
    std::vector<NAM_SAMPLE> inR(signal.begin(), signal.end());
    std::vector<NAM_SAMPLE> outL(signal.size());
    std::vector<NAM_SAMPLE> outR(signal.size());
    NAM_SAMPLE* inLPtr = inL.data();
    NAM_SAMPLE* inRPtr = inR.data();
    NAM_SAMPLE* outLPtr = outL.data();
    NAM_SAMPLE* outRPtr = outR.data();

    for (const int blockSize : blockSizes)
    {
        modelLeft->ResetAndPrewarm(kSampleRate, blockSize);
        modelRight->ResetAndPrewarm(kSampleRate, blockSize);

        const auto left = [&]() { modelLeft->process(&inLPtr, &outLPtr, blockSize); };
        const auto right = [&]() { modelRight->process(&inRPtr, &outRPtr, blockSize); };
        const auto serialBlock = [&]() {
            left();
            right();
        };

        LaneStats* dualStats = nullptr;
        const auto mainThread = std::this_thread::get_id();
        std::chrono::steady_clock::time_point dispatched;
        int helperCpu = -1;
        const auto dualBlock = [&]() {
            const int callerCpu = gPlacement ? CurrentCpu() : -1;
            helperCpu = -1;
            dispatched = std::chrono::steady_clock::now();
            const bool ran = lanes.Run(
                [&]() {
                    if (std::this_thread::get_id() != mainThread)
                    {
                        if (gPlacement)
                        {
                            helperCpu = CurrentCpu();
                        }

                        dualStats->helperRanRight++;
                        dualStats->helperStartUs.push_back(
                            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - dispatched)
                                .count());
                    }

                    right();
                },
                left);

            if (!ran)
            {
                serialBlock();
            }

            if (gPlacement)
            {
                dualStats->callerCpu.push_back(callerCpu);
                dualStats->helperCpu.push_back(helperCpu);
            }
        };

        const double periodUs = 1e6 * blockSize / kSampleRate;
        const int blocks = SweepBlockCount(blockSize, kSampleRate);
        LaneStats serial;
        LaneStats dual;
        dualStats = &dual;

        for (int warm = 0; warm < 50; ++warm)
        {
            serialBlock();
            dualBlock();
        }

        // Reserved, so the helper never allocates while it is being timed.
        dual = LaneStats{};
        dual.helperStartUs.reserve(static_cast<std::size_t>(blocks));
        dual.blockUs.reserve(static_cast<std::size_t>(blocks));
        dual.callerCpu.reserve(static_cast<std::size_t>(blocks));
        dual.helperCpu.reserve(static_cast<std::size_t>(blocks));
        serial.blockUs.reserve(static_cast<std::size_t>(blocks));

        for (int round = 0; round < kRounds; ++round)
        {
            TimeBlocks(serialBlock, blocks / kRounds, periodUs, paced, pacer, serial);
            TimeBlocks(dualBlock, blocks / kRounds, periodUs, paced, pacer, dual);
        }

        PrintLaneRow(blockSize, serial, dual, true);

        if (gPlacement)
        {
            PrintPlacementRow(dual);
        }
    }
}

// The NAM amp effect in a stereo graph, as the app runs it: its own rule decides (the gate, or
// the block size before it), against ScopedSharedHelpersRefused, which keeps every block on the
// calling thread.
void SweepEffect(const fs::path& resourcesDir, const fs::path& modelPath, bool fused,
                 const std::vector<int>& blockSizes, bool paced, BlockPacer& pacer)
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kRounds = 4;

    std::cout << "\n"
              << modelPath.filename().string() << ", " << EngineName(fused) << ", " << kAmpType << " effect, "
              << (paced ? "paced" : "back to back") << " (serial = helpers refused, dual = the effect's own choice)\n";
    PrintLaneHeader();

    guitarfx::RegisterAllEffects();
    auto resourceLibrary = std::make_unique<guitarfx::ResourceLibrary>();
    resourceLibrary->LoadFromDirectory(resourcesDir);

    for (const int blockSize : blockSizes)
    {
        auto executor = std::make_unique<guitarfx::SignalGraphExecutor>();
        executor->SetResourceLibrary(resourceLibrary.get());
        guitarfx::nammodelcache::SetFusedEngineEnabled(fused);
        executor->SetGraph(MakeNamPreset(modelPath, kAmpType).graph);
        executor->Prepare(kSampleRate, blockSize);
        guitarfx::nammodelcache::SetFusedEngineEnabled(true);

        std::vector<float> inputL(static_cast<std::size_t>(blockSize));
        GenerateGuitarLikeSignal(inputL, kSampleRate);
        std::vector<float> inputR = inputL;
        std::vector<float> outputL(inputL.size());
        std::vector<float> outputR(inputL.size());
        float* inputs[2] = {inputL.data(), inputR.data()};
        float* outputs[2] = {outputL.data(), outputR.data()};

        LaneStats* dualStats = nullptr;
        const auto ownChoice = [&]() {
            if (!gPlacement)
            {
                executor->Process(inputs, outputs, blockSize);
                return;
            }

            const int callerCpu = CurrentCpu();
            gHelperCpu.store(-1, std::memory_order_relaxed);
            gHelperClaimAttempts.store(0, std::memory_order_relaxed);
            executor->Process(inputs, outputs, blockSize);
            const bool helperRan = gHelperClaimAttempts.load(std::memory_order_relaxed) >= 2;

            if (dualStats)
            {
                dualStats->callerCpu.push_back(callerCpu);
                dualStats->helperCpu.push_back(helperRan ? gHelperCpu.load(std::memory_order_relaxed) : -1);
            }
        };
        const auto refused = [&]() {
            const guitarfx::rtparallel::ScopedSharedHelpersRefused noHelpers;
            executor->Process(inputs, outputs, blockSize);
        };

        for (int warm = 0; warm < 50; ++warm)
        {
            refused();
            ownChoice();
        }

        const double periodUs = 1e6 * blockSize / kSampleRate;
        const int blocks = SweepBlockCount(blockSize, kSampleRate);
        LaneStats serial;
        LaneStats dual;
        dual.callerCpu.reserve(static_cast<std::size_t>(blocks));
        dual.helperCpu.reserve(static_cast<std::size_t>(blocks));
        dualStats = &dual;

        for (int round = 0; round < kRounds; ++round)
        {
            TimeBlocks(refused, blocks / kRounds, periodUs, paced, pacer, serial);
            TimeBlocks(ownChoice, blocks / kRounds, periodUs, paced, pacer, dual);
        }

        PrintLaneRow(blockSize, serial, dual, false);

        if (gPlacement)
        {
            PrintPlacementRow(dual);
        }
    }
}

struct SweepOptions
{
    bool direct = true;
    bool effect = true;
    std::vector<bool> engines = {true, false}; // fused, NAM core
    std::vector<bool> pacing = {true, false};  // paced, back to back
    std::vector<int> blockSizes = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
};

void RunDualLaneSweep(const fs::path& resourcesDir, const std::vector<fs::path>& models, const SweepOptions& options)
{
    const std::vector<int>& blockSizes = options.blockSizes;
    const bool direct = options.direct;
    const bool effect = options.effect;

    if (gPlacement)
    {
        guitarfx::rtparallel::RealtimeTaskPool::SetWorkerClaimHookForTesting(&RecordHelperCpu);
    }

    // As a host's audio thread runs, and as the helper itself does.
    guitarfx::rtparallel::RaisePriorityOfThisThreadForAudio();
    const auto lease = guitarfx::rtparallel::DualLaneExecutor::Acquire();

    if (!guitarfx::rtparallel::DualLaneExecutor::Instance().IsAvailable())
    {
        std::cerr << "ERROR: the dual-lane helper is not available on this platform\n";
        return;
    }

    BlockPacer pacer;

    std::cout << "\nStereo dual-lane sweep, 48 kHz. wall: how much sooner the dual block finishes. cpu: process\n"
                 "CPU per block, both threads. helper ran: blocks where the helper took the right lane in time\n"
                 "(otherwise the calling thread ran both); start: dispatch to the helper starting it.\n";

    for (const auto& model : models)
    {
        for (const bool fused : options.engines)
        {
            for (const bool paced : options.pacing)
            {
                if (direct)
                {
                    SweepModelDirect(model, fused, blockSizes, paced, pacer);
                }

                if (effect)
                {
                    SweepEffect(resourcesDir, model, fused, blockSizes, paced, pacer);
                }
            }
        }
    }
}
} // namespace

int main(int argc, char* argv[])
{
#ifndef GUITARFX_TEST_RESOURCES_DIR
    #error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

    try
    {
        const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
        const fs::path amps = resourcesDir / "assets" / "amps" / "Guitar";
        SweepOptions options;
        std::vector<fs::path> models;

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];

            if (arg == "--direct")
            {
                options.effect = false;
            }
            else if (arg == "--effect")
            {
                options.direct = false;
            }
            else if (arg == "--placement")
            {
                gPlacement = true;
            }
            else if (arg == "--fused" || arg == "--core")
            {
                options.engines = {arg == "--fused"};
            }
            else if (arg == "--paced" || arg == "--back-to-back")
            {
                options.pacing = {arg == "--paced"};
            }
            else if (arg == "--blocks" && i + 1 < argc)
            {
                options.blockSizes.clear();
                std::stringstream list(argv[++i]);

                for (std::string size; std::getline(list, size, ',');)
                {
                    options.blockSizes.push_back(std::stoi(size));
                }
            }
            else
            {
                models.emplace_back(arg);
            }
        }

        if (models.empty())
        {
            models = {amps / "TimR" / "JCM800 2203 1985" / "JCM800 Hi P6 B8 M4 T7 G6.nam",
                      amps / "A2" / "Boosted-6505+-A2-Chugs-FullRig.nam"};
        }

        // As the app runs the NAM core (NamModelCache switches it on).
        nam::activations::Activation::enable_fast_tanh();

        RunDualLaneSweep(resourcesDir, models, options);
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}
