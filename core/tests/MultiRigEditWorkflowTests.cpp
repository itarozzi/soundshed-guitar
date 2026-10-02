/**
 * @file MultiRigEditWorkflowTests.cpp
 * @brief Editing one rig of a Multi-Rig mix leaves the other rigs playing, with their edits.
 *
 * A structural edit to the focused rig (adding, moving or removing a node) used to rebuild it
 * through ApplyPreset(), the "change preset" swap, which retires every live instance: the other
 * rigs faded out and their unsaved edits were thrown away with them. And knob, bypass and
 * automation edits reached the working copy but not its mixer slot's cached JSON, which is what
 * a rig-tab switch reloads and what a host save stores for a rig that is not focused. Driven
 * through HandleUIMessage, as the web UI drives it:
 *  - unsaved knob and bypass edits, and automation folded in with the editor closed, reach the
 *    host's save once their rig loses focus;
 *  - a structural edit rebuilds only the focused rig's slot: the other keeps its instance (the
 *    same processors), its mix settings, the Multi-Rig trim and its slot JSON, and the rebuilt
 *    rig keeps its own mix level and pan;
 *  - switching rig tabs brings back each rig with its edits;
 *  - with one rig left, a structural edit still goes through the ordinary swap.
 */

#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "automation/AutomationTypes.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr const char* kRigA = "multi-rig-edit-a";
constexpr const char* kRigB = "multi-rig-edit-b";
// An expression pedal on rig B's gain node. Rig A has no gain node, so it can only land there.
constexpr int kGainCc = 25;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

GraphNode Node(const std::string& id, const std::string& type, std::map<std::string, double> params = {})
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.params = std::move(params);
    return node;
}

/// input -> each node in turn -> output.
Preset BuildRig(const std::string& id, const std::vector<GraphNode>& chain)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.version = 2;
    preset.category = "Test";
    preset.graph.nodes = {Node("__input__", kNodeTypeInput), Node("__output__", kNodeTypeOutput)};
    std::string previous = "__input__";

    for (const auto& node : chain)
    {
        preset.graph.nodes.push_back(node);
        preset.graph.edges.push_back({previous, node.id, 0, 0, 1.0});
        previous = node.id;
    }

    preset.graph.edges.push_back({previous, "__output__", 0, 0, 1.0});
    NormalizePresetScenes(preset);
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
}

class Harness
{
  public:
    explicit Harness(const fs::path& sandbox)
        : host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock), controller(host)
    {
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        host.Pump();
    }

    void Send(const nlohmann::json& message)
    {
        controller.HandleUIMessage(message.dump());
        host.Pump();
    }

    /// Runs audio until every instance an edit retired has faded out and gone.
    void Settle()
    {
        std::vector<float> left(kBlock), right(kBlock);
        float* channels[] = {left.data(), right.data()};
        constexpr int kMaxBlocks = static_cast<int>(10.0 * kSampleRate / kBlock);

        for (int i = 0; i < kMaxBlocks && controller.GetMixer().GetRetiringPresetCount() > 0; ++i)
        {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            (void)controller.ProcessAudio(channels, channels, kBlock);
        }

        host.Pump();
    }

    [[nodiscard]] std::vector<std::string> ActiveIds() const
    {
        auto ids = controller.GetMixer().GetActivePresetIds();
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    [[nodiscard]] bool BothRigsLive() const
    {
        return ActiveIds() == std::vector<std::string>{kRigA, kRigB};
    }

    [[nodiscard]] const EffectProcessor* Processor(const std::string& rig, const std::string& nodeId) const
    {
        return controller.GetMixer().GetNodeProcessor(rig, nodeId);
    }

    /// What a host save stores for a rig that is not focused, or empty when it stores nothing.
    [[nodiscard]] std::string SavedSlotJson(const std::string& rig) const
    {
        const auto state = nlohmann::json::parse(controller.SerializeState());
        const auto data = state.value("mixer", nlohmann::json::object()).value("presetData", nlohmann::json::object());
        return data.contains(rig) ? data[rig].dump() : std::string{};
    }

    /// The first node of `type` in the working copy, or empty.
    [[nodiscard]] std::string WorkingNodeOfType(const std::string& type) const
    {
        const auto& active = controller.GetActivePreset();

        if (!active)
        {
            return {};
        }

        const auto& nodes = active->graph.nodes;
        const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& n) { return n.type == type; });
        return it == nodes.end() ? std::string{} : it->id;
    }

    [[nodiscard]] std::string FocusedId() const
    {
        const auto& active = controller.GetActivePreset();
        return active ? active->id : std::string{"<none>"};
    }

    test::PumpedTestHost host;
    PluginController controller;
};

/// "gainDb=… rate=… on|bypassed" for the nodes rig B's edits touch, in `graph`.
std::string RigBEdits(const SignalGraph& graph)
{
    const auto param = [&](const std::string& nodeId, const std::string& key) {
        const auto* node = graph.FindNode(nodeId);

        if (!node)
        {
            return nodeId + " missing";
        }

        const auto it = node->params.find(key);
        return key + "=" + (it == node->params.end() ? std::string{"none"} : std::to_string(it->second));
    };
    const auto* tremolo = graph.FindNode("trem-b");
    return param("gain-b", "gainDb") + " " + param("trem-b", "rate") + " " +
           (tremolo && !tremolo->enabled ? "bypassed" : "on");
}

/// The same, from a saved slot's JSON. A preset with scenes is saved as its scenes, and an
/// unfocused rig plays its first.
std::string RigBEditsIn(const std::string& slotJson)
{
    auto preset = slotJson.empty() ? std::nullopt : PresetStorage::DeserializeFromJson(slotJson);

    if (!preset)
    {
        return "no saved slot";
    }

    NormalizePresetScenes(*preset);
    return RigBEdits(preset->scenes.empty() ? preset->graph : preset->scenes.front().graph);
}

void MapNodeSlot(PluginController& controller, const std::string& slotId, const std::string& address, int cc)
{
    MidiControlMap map;
    map.eventType = MidiControlMap::EventType::CC;
    map.channel = -1;
    map.controller = cc;
    (void)controller.GetAutomationSlots().SetCustomSlot(slotId, slotId, address, std::nullopt, map, std::nullopt);
}

bool Run(const fs::path& sandbox)
{
    Harness h(sandbox);
    auto& controller = h.controller;

    const Preset rigA = BuildRig(kRigA, {Node("chorus-a", EffectGuids::kChorus)});
    const Preset rigB = BuildRig(kRigB, {Node("gain-b", EffectGuids::kGain, {{"gainDb", -6.0}}),
                                         Node("trem-b", EffectGuids::kTremolo, {{"rate", 4.0}})});

    h.Send({{"type", "loadPreset"}, {"presetId", kRigA}, {"preset", PresetJson(rigA)}});
    h.Send({{"type", "addActivePreset"}, {"presetId", kRigB}, {"name", kRigB}, {"preset", PresetJson(rigB)}});
    h.Send({{"type", "setPresetMix"}, {"presetId", kRigB}, {"value", 0.5}});
    h.Send({{"type", "setPresetMix"}, {"presetId", kRigA}, {"value", 0.8}});
    h.Send({{"type", "setPresetPan"}, {"presetId", kRigA}, {"value", -0.4}});
    h.Send({{"type", "setMixGain"}, {"gainDb", -3.0}});
    h.Settle();

    bool passed = Check(h.BothRigsLive() && h.FocusedId() == kRigA, "setup: two rigs in the mix, rig A focused");

    // ── Unsaved edits on rig B reach its slot once it loses focus ────────────────
    h.Send({{"type", "focusMixerPreset"}, {"presetId", kRigB}});
    passed = Check(h.FocusedId() == kRigB, "focus: rig B is the working copy") && passed;
    h.Send({{"type", "updateSignalPathNodeParam"},
            {"presetId", kRigB},
            {"nodeId", "trem-b"},
            {"paramKey", "rate"},
            {"value", 7.5}});
    h.Send({{"type", "updateSignalPathNodeBypass"}, {"presetId", kRigB}, {"nodeId", "trem-b"}, {"enabled", false}});

    // A pedal on the gain node with the editor closed: the audio pass applies it, the timer's
    // drain folds it into the working copy.
    MapNodeSlot(controller, "custom.gain", "node.gain.gainDb", kGainCc);
    controller.EnqueueMidi(MidiEvent{0xB0, static_cast<std::uint8_t>(kGainCc), 127, 0});
    controller.ProcessQueuedMidi();
    h.host.Pump();
    controller.DrainControlSurfaceRequests();

    const std::string rigBEdited = "gainDb=24.000000 rate=7.500000 bypassed";
    passed = Check(RigBEdits(controller.GetActivePreset()->graph) == rigBEdited,
                   "edits: the knob, the bypass and the pedal are in rig B's working copy (" +
                       RigBEdits(controller.GetActivePreset()->graph) + ")") &&
             passed;

    h.Send({{"type", "focusMixerPreset"}, {"presetId", kRigA}});
    const std::string rigBSaved = h.SavedSlotJson(kRigB);
    passed =
        Check(RigBEditsIn(rigBSaved) == rigBEdited,
              "save: a host save of the unfocused rig B carries its unsaved edits (" + RigBEditsIn(rigBSaved) + ")") &&
        passed;

    // ── A structural edit to rig A leaves rig B alone ───────────────────────────
    const EffectProcessor* rigBGain = h.Processor(kRigB, "gain-b");
    const auto checkEditStayedInRigA = [&](const std::string& edit) {
        const auto config = controller.GetMixer().GetPresetConfig(kRigB);
        bool ok = Check(h.BothRigsLive(), edit + ": both rigs are still in the mix");
        ok = Check(rigBGain != nullptr && h.Processor(kRigB, "gain-b") == rigBGain,
                   edit + ": rig B keeps its running instance, not a rebuilt or faded one") &&
             ok;
        ok = Check(config && config->mix == 0.5 && controller.GetMixer().GetMixGainDb() == -3.0,
                   edit + ": rig B's mix level and the Multi-Rig trim are unchanged") &&
             ok;
        ok = Check(h.SavedSlotJson(kRigB) == rigBSaved, edit + ": rig B's slot JSON is unchanged") && ok;

        // The rebuilt slot takes over the running one's mix settings rather than the defaults.
        const auto rigAConfig = controller.GetMixer().GetPresetConfig(kRigA);
        ok = Check(rigAConfig && rigAConfig->mix == 0.8 && rigAConfig->pan == -0.4,
                   edit + ": rig A, rebuilt, keeps its own mix level and pan") &&
             ok;
        return ok;
    };

    h.Send({{"type", "addSignalPathNode"}, {"effectType", EffectGuids::kPhaser}, {"insertAfter", "chorus-a"}});
    const std::string phaser = h.WorkingNodeOfType(EffectGuids::kPhaser);
    passed = Check(!phaser.empty() && h.Processor(kRigA, phaser) != nullptr,
                   "add: rig A's slot runs the node added to it") &&
             passed;
    passed = checkEditStayedInRigA("add") && passed;
    h.Settle();
    passed = checkEditStayedInRigA("add, once the old rig A has faded") && passed;

    h.Send({{"type", "reorderSignalPathNode"},
            {"nodeId", phaser},
            {"edge", {{"from", "__input__"}, {"to", "chorus-a"}, {"fromPort", 0}, {"toPort", 0}}}});
    const auto* moved = controller.GetActivePreset()->graph.FindNode(phaser);
    const bool movedFirst =
        moved &&
        std::any_of(controller.GetActivePreset()->graph.edges.begin(), controller.GetActivePreset()->graph.edges.end(),
                    [&](const GraphEdge& e) { return e.from == "__input__" && e.to == phaser; });
    passed = Check(movedFirst, "move: the node now comes first in rig A") && passed;
    passed = checkEditStayedInRigA("move") && passed;

    // ── Tab switches bring each rig back with its edits ─────────────────────────
    h.Send({{"type", "focusMixerPreset"}, {"presetId", kRigB}});
    passed = Check(h.FocusedId() == kRigB && RigBEdits(controller.GetActivePreset()->graph) == rigBEdited,
                   "tabs: rig B comes back with its knob, bypass and pedal edits (" +
                       RigBEdits(controller.GetActivePreset()->graph) + ")") &&
             passed;
    h.Send({{"type", "focusMixerPreset"}, {"presetId", kRigA}});
    passed = Check(h.FocusedId() == kRigA && h.WorkingNodeOfType(EffectGuids::kPhaser) == phaser,
                   "tabs: rig A comes back with the node added to it") &&
             passed;

    h.Send({{"type", "deleteSignalPathNode"}, {"nodeId", phaser}});
    passed = Check(h.WorkingNodeOfType(EffectGuids::kPhaser).empty() && h.Processor(kRigA, phaser) == nullptr,
                   "remove: the node is gone from rig A and its slot") &&
             passed;
    passed = checkEditStayedInRigA("remove") && passed;

    // ── And the other way round: an edit to rig B leaves rig A alone ────────────
    const EffectProcessor* rigAChorus = h.Processor(kRigA, "chorus-a");
    const std::string rigASaved = [&] {
        h.Send({{"type", "focusMixerPreset"}, {"presetId", kRigB}});
        return h.SavedSlotJson(kRigA);
    }();
    h.Send({{"type", "deleteSignalPathNode"}, {"nodeId", "trem-b"}});
    passed = Check(h.BothRigsLive() && rigAChorus != nullptr && h.Processor(kRigA, "chorus-a") == rigAChorus &&
                       !rigASaved.empty() && h.SavedSlotJson(kRigA) == rigASaved,
                   "remove on rig B: rig A keeps its instance and its slot JSON") &&
             passed;
    passed = Check(h.Processor(kRigB, "trem-b") == nullptr, "remove on rig B: its slot drops the node") && passed;
    const auto rigBConfig = controller.GetMixer().GetPresetConfig(kRigB);
    passed = Check(rigBConfig && rigBConfig->mix == 0.5, "remove on rig B: it keeps its own mix level") && passed;

    // ── With one rig left, a structural edit is the ordinary swap ───────────────
    h.Send({{"type", "removeActivePreset"}, {"presetId", kRigA}});
    h.Send({{"type", "addSignalPathNode"}, {"effectType", EffectGuids::kPhaser}, {"insertAfter", "gain-b"}});
    const std::string lonePhaser = h.WorkingNodeOfType(EffectGuids::kPhaser);
    h.Settle();
    passed = Check(h.ActiveIds() == std::vector<std::string>{kRigB} && !lonePhaser.empty() &&
                       h.Processor(kRigB, lonePhaser) != nullptr,
                   "one rig: the edit rebuilds it and nothing else is left in the mix") &&
             passed;

    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;

    // Its own folder: a fixed name collides with another run of the suite.
    const fs::path sandbox =
        fs::temp_directory_path() / ("guitarfx-multi-rig-edit-tests-" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    bool passed = false;
    {
        passed = Run(sandbox);
    }

    fs::remove_all(sandbox, ec);
    std::cout << (passed ? "MultiRigEditWorkflowTests PASSED\n" : "MultiRigEditWorkflowTests FAILED\n");
    return passed ? 0 : 1;
}
