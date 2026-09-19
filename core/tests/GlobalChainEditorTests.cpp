#include "dsp/GlobalChainEditor.h"

#include <cmath>
#include <iostream>

using namespace guitarfx;

int main()
{
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
