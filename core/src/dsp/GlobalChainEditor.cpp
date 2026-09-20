#include "dsp/GlobalChainEditor.h"

#include "dsp/EffectGuids.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace guitarfx
{
namespace
{
bool GraphHasNodeType(const SignalGraph& graph, const std::string& type)
{
    for (const auto& node : graph.nodes)
    {
        if (node.type == type)
        {
            return true;
        }
    }
    return false;
}

GraphNode* FindNodeByIdOrType(SignalGraph& graph, const char* id, const char* type)
{
    if (auto* node = graph.FindNode(id))
    {
        return node;
    }
    for (auto& node : graph.nodes)
    {
        if (node.type == type)
        {
            return &node;
        }
    }
    return nullptr;
}
} // namespace

void GlobalChainEditor::NormalizeConfig(GlobalSignalChainConfig& config)
{
    if ((config.preChainGraph.nodes.empty() && config.preChainGraph.edges.empty()) ||
        !GraphHasNodeType(config.preChainGraph, EffectGuids::kDynamicsGate) ||
        !GraphHasNodeType(config.preChainGraph, EffectGuids::kTranspose))
    {
        config.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }

    if ((config.postChainGraph.nodes.empty() && config.postChainGraph.edges.empty()) ||
        !GraphHasNodeType(config.postChainGraph, EffectGuids::kEqParametric) ||
        !GraphHasNodeType(config.postChainGraph, EffectGuids::kDelayDoubler))
    {
        config.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }

    // The executor folds older parameter spellings on its own copy of the graph, which
    // leaves this one -- the config the UI is sent and the settings blob is written from
    // -- still holding "attackMs" where the registry says "attack". Anything reading a
    // value back out of the config then misses what the engine is actually running, so
    // fold them here too, on the copy everything but the audio thread reads.
    for (auto& node : config.preChainGraph.nodes)
    {
        CanonicalizeNodeParams(node);
    }

    for (auto& node : config.postChainGraph.nodes)
    {
        CanonicalizeNodeParams(node);
    }
}

GraphNode* GlobalChainEditor::FindPreNode(const char* id, const char* type)
{
    if (mConfig.preChainGraph.nodes.empty() && mConfig.preChainGraph.edges.empty())
    {
        mConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    return FindNodeByIdOrType(mConfig.preChainGraph, id, type);
}

GraphNode* GlobalChainEditor::FindPostNode(const char* id, const char* type)
{
    if (mConfig.postChainGraph.nodes.empty() && mConfig.postChainGraph.edges.empty())
    {
        mConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    return FindNodeByIdOrType(mConfig.postChainGraph, id, type);
}

void GlobalChainEditor::SetGateEnabled(bool enabled)
{
    if (auto* node = FindPreNode("global_gate", EffectGuids::kDynamicsGate))
    {
        node->enabled = enabled;
        mPre.SetNodeEnabled(node->id, enabled);
    }
}

void GlobalChainEditor::SetGateParam(const char* key, double value)
{
    if (auto* node = FindPreNode("global_gate", EffectGuids::kDynamicsGate))
    {
        node->params[key] = value;
        mPre.SetNodeParam(node->id, key, value);
    }
}

void GlobalChainEditor::SetGateThreshold(double thresholdDb)
{
    SetGateParam("threshold", thresholdDb);
}

void GlobalChainEditor::SetGateAttack(double attackMs)
{
    SetGateParam("attack", attackMs);
}

void GlobalChainEditor::SetGateHold(double holdMs)
{
    SetGateParam("hold", holdMs);
}

void GlobalChainEditor::SetGateRelease(double releaseMs)
{
    SetGateParam("release", releaseMs);
}

void GlobalChainEditor::SetGateHysteresis(double hysteresisDb)
{
    SetGateParam("hysteresis", hysteresisDb);
}

void GlobalChainEditor::SetGateRange(double rangeDb)
{
    SetGateParam("range", rangeDb);
}

void GlobalChainEditor::SetGateStereoLink(bool linked)
{
    // Stored as a number because that is what a graph node's param map holds; the
    // effect reads it back with the same >= 0.5 test the registry's labels imply.
    SetGateParam("stereoLink", linked ? 1.0 : 0.0);
}

void GlobalChainEditor::SetTransposeEnabled(bool enabled)
{
    if (auto* node = FindPreNode("global_transpose", EffectGuids::kTranspose))
    {
        node->enabled = enabled;
        mPre.SetNodeEnabled(node->id, enabled);
    }
}

void GlobalChainEditor::SetTranspose(int semitones)
{
    const double value = static_cast<double>(std::clamp(semitones, -12, 12));
    const bool enabled = value != 0.0;
    if (auto* node = FindPreNode("global_transpose", EffectGuids::kTranspose))
    {
        node->enabled = enabled;
        node->params["semitones"] = value;
        mPre.SetNodeEnabled(node->id, enabled);
        mPre.SetNodeParam(node->id, "semitones", value);
    }
}

void GlobalChainEditor::SetEQEnabled(bool enabled)
{
    if (auto* node = FindPostNode("global_eq", EffectGuids::kEqParametric))
    {
        node->enabled = enabled;
        mPost.SetNodeEnabled(node->id, enabled);
    }
}

void GlobalChainEditor::SetEQBandGain(int band, double dB)
{
    static const char* kParamNames[] = {"lowGain", "lowMidGain", "highMidGain", "highGain"};
    if (band < 0 || band > 3)
    {
        return;
    }
    if (auto* node = FindPostNode("global_eq", EffectGuids::kEqParametric))
    {
        node->params[kParamNames[band]] = dB;
        mPost.SetNodeParam(node->id, kParamNames[band], dB);
    }
}

void GlobalChainEditor::SetEQBandFrequency(int band, double freq)
{
    static const char* kParamNames[] = {"lowFreq", "lowMidFreq", "highMidFreq", "highFreq"};
    if (band < 0 || band > 3)
    {
        return;
    }
    if (auto* node = FindPostNode("global_eq", EffectGuids::kEqParametric))
    {
        node->params[kParamNames[band]] = freq;
        mPost.SetNodeParam(node->id, kParamNames[band], freq);
    }
}

void GlobalChainEditor::SetEQBandQ(int band, double q)
{
    static const char* kParamNames[] = {"", "lowMidQ", "highMidQ", ""};
    if (band < 1 || band > 2)
    {
        return;
    }
    if (auto* node = FindPostNode("global_eq", EffectGuids::kEqParametric))
    {
        node->params[kParamNames[band]] = q;
        mPost.SetNodeParam(node->id, kParamNames[band], q);
    }
}

void GlobalChainEditor::SetDoublerEnabled(bool enabled)
{
    if (auto* node = FindPostNode("global_doubler", EffectGuids::kDelayDoubler))
    {
        node->enabled = enabled;
        mPost.SetNodeEnabled(node->id, enabled);
    }
}

void GlobalChainEditor::SetDoublerDelay(double delayMs)
{
    const double clamped = std::clamp(delayMs, 0.5, 100.0);
    if (auto* node = FindPostNode("global_doubler", EffectGuids::kDelayDoubler))
    {
        node->params["time"] = clamped;
        mPost.SetNodeParam(node->id, "time", clamped);
    }
}

void GlobalChainEditor::SetDoublerMix(double mix)
{
    const double clamped = std::clamp(mix, 0.0, 1.0);
    if (auto* node = FindPostNode("global_doubler", EffectGuids::kDelayDoubler))
    {
        node->params["mix"] = clamped;
        mPost.SetNodeParam(node->id, "mix", clamped);
    }
}

void GlobalChainEditor::SetDoublerDetune(double cents)
{
    if (auto* node = FindPostNode("global_doubler", EffectGuids::kDelayDoubler))
    {
        node->params["detune"] = cents;
        mPost.SetNodeParam(node->id, "detune", cents);
    }
}

void GlobalChainEditor::SetInputGain(double dB)
{
    mConfig.inputGain = dB;
    mPre.SetInputTrim(dB);
}

double GlobalChainEditor::SetOutputGain(double dB)
{
    mConfig.outputGain = dB;
    return std::pow(10.0, dB / 20.0);
}
} // namespace guitarfx
