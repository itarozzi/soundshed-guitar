// GlobalChainEngine: when the global pre/post executors are rebuilt, staged and swapped.

#include "dsp/GlobalChainEditor.h"
#include "dsp/GlobalChainEngine.h"

#include <iostream>
#include <map>
#include <string>

using namespace guitarfx;

namespace
{
int gFailures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

GlobalSignalChainConfig NormalizedDefault()
{
    GlobalSignalChainConfig config;
    GlobalChainEditor::NormalizeConfig(config);
    return config;
}

bool HasSeededDefault(const SignalGraphExecutor& executor)
{
    const auto& defaults = executor.GetNodeTypeConfigDefaults();
    const auto type = defaults.find("test.type");
    return type != defaults.end() && type->second.count("quality") == 1 && type->second.at("quality") == "high";
}
} // namespace

int main()
{
    const std::map<std::string, std::map<std::string, std::string>> typeDefaults{{"test.type", {{"quality", "high"}}}};

    ExecutorSetup unprepared;
    unprepared.nodeTypeConfigDefaults = &typeDefaults;

    ExecutorSetup prepared = unprepared;
    prepared.prepared = true;
    prepared.sampleRate = 48000.0;
    prepared.maxBlockSize = 64;

    DspReaper reaper;
    reaper.Start();
    GlobalChainEngine engine(reaper);
    engine.Adopt(NormalizedDefault()); // the mixer only ever hands it a normalised config

    // A fresh engine owes a build, but only once the stream format is known.
    Check(!engine.EnsureUpToDate(unprepared), "nothing is built before the mixer is prepared");
    Check(engine.EnsureUpToDate(prepared), "the first prepared pass builds both chains");
    Check(!engine.EnsureUpToDate(prepared), "a built chain is not rebuilt without a change");
    Check(!engine.Pre().GetNodeTypes().empty() && !engine.Post().GetNodeTypes().empty(), "both chains have nodes");
    Check(HasSeededDefault(engine.Pre()) && HasSeededDefault(engine.Post()),
          "a rebuild seeds this instance's node-type defaults");

    // Adopting the running graphs again is the common case and must not cost a rebuild.
    Check(!engine.Adopt(engine.Config()), "an identical config does not ask for a rebuild");
    Check(!engine.EnsureUpToDate(prepared), "and nothing is rebuilt");

    auto changed = engine.Config();
    auto* gate = changed.preChainGraph.FindNode("global_gate");
    Check(gate != nullptr, "the normalised pre-chain has a gate");

    if (gate != nullptr)
    {
        gate->enabled = !gate->enabled;
    }

    Check(engine.Adopt(changed), "a changed graph asks for a rebuild");
    Check(engine.EnsureUpToDate(prepared), "and gets one");

    // The staged swap: nothing is built for an unchanged config, but the config still commits.
    Check(!engine.PrepareSwap(engine.Config(), prepared), "an unchanged config stages no executors");
    Check(engine.CommitSwap(), "a staged config commits even without new executors");
    Check(!engine.CommitSwap(), "a second commit has nothing to install");

    auto swapped = NormalizedDefault();
    swapped.outputGain = -3.0;
    Check(engine.PrepareSwap(swapped, prepared), "a changed graph stages new executors");
    Check(engine.CommitSwap(), "the staged executors commit");
    Check(engine.Config().outputGain == -3.0, "the committed config is the staged one");
    Check(HasSeededDefault(engine.Pre()) && HasSeededDefault(engine.Post()),
          "staged executors carry the node-type defaults too");
    Check(!engine.EnsureUpToDate(prepared), "a committed swap leaves nothing to rebuild");

    Check(!engine.PrepareSwap(swapped, unprepared), "nothing is staged before the mixer is prepared");
    engine.CommitSwap();

    reaper.Stop();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "GlobalChainEngineTests passed\n";
    return 0;
}
