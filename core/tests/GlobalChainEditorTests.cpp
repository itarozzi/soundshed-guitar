#include "dsp/GlobalChainEditor.h"

#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/PickupMatchEffect.h"

#include <algorithm>

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

/// The node after the input in a linear chain, or "" when the input feeds nothing.
std::string AfterInput(const SignalGraph& graph)
{
    for (const auto& edge : graph.edges)
    {
        if (edge.from == "__input__")
        {
            return edge.to;
        }
    }

    return {};
}

/**
 * Pickup Match sits straight after the input, bypassed: in a new chain, and in a chain saved
 * before it existed, which keeps its own nodes and settings.
 */
bool AddsBypassedPickupMatchAfterInput()
{
    RegisterAllEffects();

    GlobalSignalChainConfig fresh;
    GlobalChainEditor::NormalizeConfig(fresh);
    const auto* pickup = fresh.preChainGraph.FindNode("global_pickup");

    if (!pickup || pickup->enabled || pickup->type != EffectGuids::kPickupMatch ||
        AfterInput(fresh.preChainGraph) != "global_pickup" || pickup->params.count("target") == 0)
    {
        std::cerr << "A new global chain has no bypassed Pickup Match after its input\n";
        return false;
    }

    // A chain saved before: input -> gate -> transpose -> output, with a gate setting.
    GlobalSignalChainConfig stored;
    GlobalChainEditor::NormalizeConfig(stored);
    auto& graph = stored.preChainGraph;
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                                     [](const GraphNode& n) { return n.id == "global_pickup"; }),
                      graph.nodes.end());
    graph.edges = {
        {"__input__", "global_gate"}, {"global_gate", "global_transpose"}, {"global_transpose", "__output__"}};
    graph.FindNode("global_gate")->params["threshold"] = -51.0;

    GlobalChainEditor::NormalizeConfig(stored);
    const auto* gate = stored.preChainGraph.FindNode("global_gate");
    const bool gateFollows = std::any_of(graph.edges.begin(), graph.edges.end(), [](const auto& e) {
        return e.from == "global_pickup" && e.to == "global_gate";
    });

    if (AfterInput(graph) != "global_pickup" || !gateFollows || !gate || gate->params.at("threshold") != -51.0 ||
        graph.edges.size() != 4)
    {
        std::cerr << "A stored chain was not given Pickup Match after its input, intact otherwise\n";
        return false;
    }

    GlobalChainEditor::NormalizeConfig(stored);

    if (std::count_if(graph.nodes.begin(), graph.nodes.end(),
                      [](const GraphNode& n) { return n.type == EffectGuids::kPickupMatch; }) != 1)
    {
        std::cerr << "Normalizing again added a second Pickup Match\n";
        return false;
    }

    SignalGraphExecutor pre;
    SignalGraphExecutor post;
    GlobalChainEditor editor(stored, pre, post);
    editor.SetPickupMatchEnabled(true);
    const bool known = editor.SetPickupMatchParam("target", 9999.0);
    const bool unknown = editor.SetPickupMatchParam("notAParam", 1.0);
    const auto* edited = stored.preChainGraph.FindNode("global_pickup");

    if (!edited->enabled || !known || unknown || edited->params.at("target") != pickup_match::kLastPickup ||
        edited->params.count("notAParam") != 0)
    {
        std::cerr << "Pickup Match edits did not land on the global node as the effect holds them\n";
        return false;
    }

    return true;
}

int main()
{
    if (!CanonicalizesStoredParamAliases() || !AddsBypassedPickupMatchAfterInput())
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
    // The shelves take a Q too: the EQ panel offers it, and it used to be dropped.
    editor.SetEQBandQ(0, 0.9);
    editor.SetEQBandQ(3, 1.2);
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
        eq->params.at("highMidGain") != 1.75 || eq->params.at("highMidQ") != 1.4 || eq->params.count("lowQ") == 0 ||
        eq->params.at("lowQ") != 0.9 || eq->params.count("highQ") == 0 || eq->params.at("highQ") != 1.2 ||
        !doubler->enabled || doubler->params.at("time") != 100.0 || doubler->params.at("mix") != 0.0 ||
        config.inputGain != -3.0 || config.outputGain != -6.0206 || std::abs(masterGain - 0.5) > 1e-4)
    {
        std::cerr << "Global chain edit rules changed\n";
        return 1;
    }
    return 0;
}
