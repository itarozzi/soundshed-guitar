/**
 * @file NodeTelemetryHoldTests.cpp
 * @brief Holds the executor's per-node telemetry to finished blocks.
 *
 * The message thread reads each node's processing time and level meters whenever it likes, while
 * the audio thread is part-way through a block. A node that runs every block must never read as
 * "did not run" or silent because the read landed before it had run in the current block: the DSP
 * status readout blanked its time, and the peak meter and clip LED dropped out, whenever the
 * executor reset those values at the start of a block and the node only wrote its own later.
 *
 * A probe effect at the head of the chain reads the telemetry of the nodes after it, which have
 * not yet run in that block but ran in every block before.
 */

#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using guitarfx::GraphNode;
using guitarfx::SignalGraph;
using guitarfx::SignalGraphExecutor;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 64;
/// ~0.3 s: the level meters refresh at 30 Hz, once every 25 blocks here, so this covers eight.
constexpr int kBlocks = 200;
constexpr const char* kProbeType = "node_telemetry_test_mid_block_probe";
constexpr const char* kPassType = "node_telemetry_test_pass";

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << '\n';
        ++gFailures;
    }
}

/// A pass-through that calls a hook while the executor is part-way through its block.
class MidBlockProbe final : public guitarfx::PassthroughProcessor
{
  public:
    static inline std::function<void()> sHook;

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        RunHook();
        PassthroughProcessor::Process(inputs, outputs, numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        RunHook();
        PassthroughProcessor::ProcessMono(input, output, numSamples);
    }

  private:
    static void RunHook()
    {
        if (sHook)
        {
            sHook();
        }
    }
};

void RegisterTestTypes()
{
    guitarfx::EffectTypeInfo probe;
    probe.type = kProbeType;
    probe.displayName = "Mid-block probe";
    probe.category = "utility";
    guitarfx::EffectRegistry::Instance().Register(probe.type, probe, [] { return std::make_unique<MidBlockProbe>(); });

    guitarfx::EffectTypeInfo pass;
    pass.type = kPassType;
    pass.displayName = "Pass-through";
    pass.category = "utility";
    guitarfx::EffectRegistry::Instance().Register(pass.type, pass,
                                                  [] { return std::make_unique<guitarfx::PassthroughProcessor>(); });
}

GraphNode Node(const std::string& id, const std::string& type)
{
    return GraphNode{id, type, "", id, true};
}

/// Input -> each type in turn -> output.
SignalGraph Chain(const std::vector<std::pair<std::string, std::string>>& stages)
{
    SignalGraph graph;
    graph.nodes.push_back(Node("in", guitarfx::kNodeTypeInput));
    std::string previous = "in";

    for (const auto& [id, type] : stages)
    {
        graph.nodes.push_back(Node(id, type));
        graph.edges.push_back({previous, id, 0, 0, 1.0});
        previous = id;
    }

    graph.nodes.push_back(Node("out", guitarfx::kNodeTypeOutput));
    graph.edges.push_back({previous, "out", 0, 0, 1.0});
    return graph;
}

/// What a reader catching the block part-way through saw of the nodes after the probe.
struct MidBlockReads
{
    int reads = 0;
    int timeMissing = 0;
    int peakZero = 0;
    int rmsZero = 0;
    int clipReads = 0;
    int clipZero = 0;
};

/// The probe first, then a pass-through fed a clipping signal, an amp and an EQ. Every node after
/// the probe runs every block.
void TestTelemetryHoldsThroughTheBlock()
{
    using namespace guitarfx::EffectGuids;

    SignalGraphExecutor executor;
    executor.SetSignalDiagnosticsEnabled(true);
    executor.SetGraph(Chain({{"probe", kProbeType}, {"pass", kPassType}, {"amp", kAmpBuiltin}, {"eq", kEqGraphic}}));
    executor.Prepare(kSampleRate, kBlock);

    MidBlockReads seen;
    int block = 0;
    MidBlockProbe::sHook = [&]() {
        if (block == 0)
        {
            return; // nothing has run yet
        }

        const auto stats = executor.GetPerformanceStats();

        for (const auto& level : executor.GetNodeSignalLevels())
        {
            if (level.nodeId != "pass" && level.nodeId != "amp" && level.nodeId != "eq")
            {
                continue;
            }

            ++seen.reads;
            seen.timeMissing += stats.nodeProcessingTimesUs.count(level.nodeId) == 0 ? 1 : 0;
            seen.peakZero += level.peak == 0.0 ? 1 : 0;
            seen.rmsZero += level.rms == 0.0 ? 1 : 0;

            if (level.nodeId == "pass")
            {
                ++seen.clipReads;
                seen.clipZero += level.clipCount == 0 ? 1 : 0;
            }
        }
    };

    // Above full scale, so the pass-through clips on every sample.
    std::vector<float> inL(kBlock, 1.5f), inR(kBlock, 1.5f), outL(kBlock), outR(kBlock);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};

    for (block = 0; block < kBlocks; ++block)
    {
        executor.Process(ins, outs, kBlock);
    }

    MidBlockProbe::sHook = nullptr;

    const auto ofReads = [](int bad, int reads) {
        return std::to_string(bad) + " of " + std::to_string(reads) + " reads did";
    };
    Check(seen.reads > 0, "the probe read the nodes after it");
    Check(seen.timeMissing == 0, "a node later in the chain never reads as not having run mid-block; " +
                                     ofReads(seen.timeMissing, seen.reads));
    Check(seen.peakZero == 0, "nor reads a zero peak; " + ofReads(seen.peakZero, seen.reads));
    Check(seen.rmsZero == 0, "nor a zero RMS; " + ofReads(seen.rmsZero, seen.reads));
    Check(seen.clipReads > 0 && seen.clipZero == 0,
          "and a clipping node never reads as not clipping; " + ofReads(seen.clipZero, seen.clipReads));
}

/// After a block, a node reads as what it did in that block: a bypassed node has no time, and a
/// node with no input has no time and no level.
void TestTelemetryAfterTheBlock()
{
    using namespace guitarfx::EffectGuids;

    SignalGraph graph = Chain({{"amp", kAmpBuiltin}, {"eq", kEqGraphic}});
    // Fed by nothing, so it never has input.
    graph.nodes.push_back(Node("orphan", kPassType));
    graph.edges.push_back({"orphan", "out", 0, 0, 1.0});

    SignalGraphExecutor executor;
    executor.SetSignalDiagnosticsEnabled(true);
    executor.SetGraph(graph);
    executor.Prepare(kSampleRate, kBlock);

    std::vector<float> inL(kBlock, 0.5f), inR(kBlock, 0.5f), outL(kBlock), outR(kBlock);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};

    const auto run = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
        {
            executor.Process(ins, outs, kBlock);
        }
    };

    const auto levelOf = [&](const std::string& nodeId) {
        for (const auto& level : executor.GetNodeSignalLevels())
        {
            if (level.nodeId == nodeId)
            {
                return level;
            }
        }

        return SignalGraphExecutor::NodeSignalLevel{};
    };

    run(1);
    auto stats = executor.GetPerformanceStats();
    Check(stats.nodeProcessingTimesUs.count("amp") == 1 && stats.nodeProcessingTimesUs.count("eq") == 1,
          "a node that ran reads its time after the block");
    Check(levelOf("eq").peak > 0.0, "and its level");
    Check(stats.nodeProcessingTimesUs.count("orphan") == 0, "a node with no input reads no time");
    Check(levelOf("orphan").peak == 0.0 && levelOf("orphan").clipCount == 0, "and no level");

    executor.SetNodeEnabled("eq", false);
    run(1);
    stats = executor.GetPerformanceStats();
    Check(stats.nodeProcessingTimesUs.count("eq") == 0, "a node bypassed reads no time after the next block");
    Check(stats.nodeProcessingTimesUs.count("amp") == 1, "while the node before it still does");

    executor.SetNodeEnabled("eq", true);
    run(1);
    stats = executor.GetPerformanceStats();
    Check(stats.nodeProcessingTimesUs.count("eq") == 1, "and switched back on, reads one again");

    executor.SetSignalDiagnosticsEnabled(false);
    run(1);
    Check(executor.GetPerformanceStats().nodeProcessingTimesUs.empty(), "with diagnostics off, no times at all");
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();
    RegisterTestTypes();

    TestTelemetryHoldsThroughTheBlock();
    TestTelemetryAfterTheBlock();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "NodeTelemetryHoldTests passed\n";
    return 0;
}
