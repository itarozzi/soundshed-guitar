#include "dsp/GlobalChainEditor.h"

#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"

#include <cmath>
#include <iostream>

using namespace guitarfx;

/**
 * A stored chain carrying an older parameter spelling has to come back canonical.
 * The executor folds its own copy, so the engine plays the right value either way;
 * it is the config that the UI is sent and the settings blob written from, and a
 * panel reading "attack" out of a node holding only "attackMs" shows a default
 * instead of the setting and then writes that default back over it.
 */
bool CanonicalizesStoredParamAliases()
{
    RegisterAllEffects();

    GlobalSignalChainConfig config;
    GlobalChainEditor::NormalizeConfig(config);

    auto* gate = config.preChainGraph.FindNode("global_gate");

    if (!gate)
    {
        std::cerr << "No global gate to canonicalize\n";
        return false;
    }

    gate->params.erase("attack");
    gate->params["attackMs"] = 7.5;

    GlobalChainEditor::NormalizeConfig(config);
    gate = config.preChainGraph.FindNode("global_gate");

    if (!gate || gate->params.count("attackMs") != 0 || gate->params.at("attack") != 7.5)
    {
        std::cerr << "Stored gate parameter alias was not folded onto the canonical id\n";
        return false;
    }

    return true;
}

int main()
{
    if (!CanonicalizesStoredParamAliases())
    {
        return 1;
    }

    GlobalSignalChainConfig config;
    GlobalChainEditor::NormalizeConfig(config);
    if (!config.preChainGraph.FindNode("global_gate") || !config.preChainGraph.FindNode("global_transpose") ||
        !config.postChainGraph.FindNode("global_eq") || !config.postChainGraph.FindNode("global_doubler"))
    {
        std::cerr << "Malformed global chain was not normalized\n";
        return 1;
    }

    SignalGraphExecutor pre;
    SignalGraphExecutor post;
    GlobalChainEditor editor(config, pre, post);
    editor.SetGateEnabled(true);
    editor.SetGateThreshold(-48.0);
    editor.SetGateAttack(9.0);
    editor.SetTranspose(20);
    editor.SetEQEnabled(true);
    editor.SetEQBandGain(2, 1.75);
    editor.SetEQBandQ(2, 1.4);
    editor.SetDoublerEnabled(true);
    editor.SetDoublerDelay(200.0);
    editor.SetDoublerMix(-1.0);
    editor.SetInputGain(-3.0);
    const double masterGain = editor.SetOutputGain(-6.0206);

    const auto* gate = config.preChainGraph.FindNode("global_gate");
    const auto* transpose = config.preChainGraph.FindNode("global_transpose");
    const auto* eq = config.postChainGraph.FindNode("global_eq");
    const auto* doubler = config.postChainGraph.FindNode("global_doubler");
    if (!gate->enabled || gate->params.at("threshold") != -48.0 || gate->params.at("attack") != 9.0 ||
        !transpose->enabled || transpose->params.at("semitones") != 12.0 || !eq->enabled ||
        eq->params.at("highMidGain") != 1.75 || eq->params.at("highMidQ") != 1.4 || !doubler->enabled ||
        doubler->params.at("time") != 100.0 || doubler->params.at("mix") != 0.0 || config.inputGain != -3.0 ||
        config.outputGain != -6.0206 || std::abs(masterGain - 0.5) > 1e-4)
    {
        std::cerr << "Global chain edit rules changed\n";
        return 1;
    }
    return 0;
}
