/**
 * PluginControllerLooper.cpp - the looper's render graphs, built on the message thread for its
 * worker to play: a preset as the live rig plays it (BuildPresetLevelJob, for the player) with the
 * global pre- and post-chains in force, all detached from the live mixer and never under the DSP
 * lock. controller/LooperRender.h has the design; the looper's wiring is CreateLooper, in
 * PluginControllerRiffs.cpp, since its projects live in the riff library.
 */

#include "PluginController.h"

#include "controller/LooperRender.h"
#include "controller/PresetLevelMeter.h"
#include "dsp/GlobalChainEditor.h"

namespace guitarfx
{
std::unique_ptr<looper_render::Graphs> PluginController::BuildLooperRenderGraphs(const std::string& presetId,
                                                                                 const std::string& sceneId,
                                                                                 std::string& error)
{
    nlohmann::json request = {{"presetId", presetId.empty() ? mActivePresetId : presetId}};

    if (!sceneId.empty() || presetId.empty())
    {
        request["sceneId"] = sceneId.empty() ? mActiveSceneId : sceneId;
    }

    nlohmann::json failure = nlohmann::json::object();
    auto job = BuildPresetLevelJob(request, failure, true);

    if (!job || !job->executor)
    {
        error = failure.value("message", std::string("The preset cannot be rendered offline"));
        return nullptr;
    }

    // The global chains as the live input goes through them, built as GlobalChainEngine builds its own.
    const GlobalSignalChainConfig& chains = mPresetMixer.GetGlobalChainConfig();
    const auto build = [&](const SignalGraph& graph, ChannelLayout layout, const double* inputTrimDb) {
        auto executor = std::make_unique<SignalGraphExecutor>();
        executor->SetResourceLibrary(&mResourceLibrary);
        executor->SetInputLayout(layout);
        executor->SetDualMono(false);
        executor->SetGraph(graph);

        if (inputTrimDb != nullptr)
        {
            executor->SetInputTrim(*inputTrimDb);
        }

        executor->SetSignalDiagnosticsEnabled(false);
        executor->SetParallelLevelsEnabled(false);
        executor->Prepare(job->sampleRate, job->blockSize);
        executor->SetTempo(GetEffectiveTempoBpm());
        return executor;
    };

    auto graphs = std::make_unique<looper_render::Graphs>();
    graphs->pre =
        build(GlobalChainEditor::LivePreChain(chains.preChainGraph), ChannelLayout::Stereo, &chains.inputGain);
    graphs->preset = std::move(job->executor);
    graphs->post = build(chains.postChainGraph, ChannelLayout::Stereo, nullptr);

    if (graphs->pre->AnyNodeRequiresMainThreadLoad() || graphs->post->AnyNodeRequiresMainThreadLoad())
    {
        error = "The global chain has a third-party plugin, which cannot be rendered offline";
        return nullptr;
    }

    graphs->presetId = job->presetId;
    graphs->sceneId = job->sceneId;
    const auto preset = LoadPresetById(job->presetId);
    graphs->presetName = preset ? preset->name : (mActivePreset ? mActivePreset->name : std::string{});
    graphs->slotGainLeft = job->slotGainLeft;
    graphs->slotGainRight = job->slotGainRight;
    return graphs;
}
} // namespace guitarfx
