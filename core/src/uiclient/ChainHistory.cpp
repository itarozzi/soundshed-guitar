#include "uiclient/ChainHistory.h"

#include "util/PathEncoding.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace guitarfx::uiclient
{
namespace
{
/// Config the engine owns rather than the user: a hosted plugin's state, captured by the plugin
/// itself. Including it would make a plugin saving its state look like an edit.
bool IsEngineOwnedConfig(const std::string& key)
{
    return key == "pluginStateBase64" || key == "pluginStateBase64Length";
}

std::map<std::string, std::string> ComparableConfig(const std::map<std::string, std::string>& config)
{
    std::map<std::string, std::string> result;

    for (const auto& [key, value] : config)
    {
        if (!IsEngineOwnedConfig(key))
        {
            result.emplace(key, value);
        }
    }

    return result;
}

nlohmann::json ResourcesJson(const std::vector<ResourceRef>& resources)
{
    auto list = nlohmann::json::array();

    for (const auto& r : resources)
    {
        nlohmann::json ref = {{"resourceType", r.resourceType},
                              {"resourceId", r.resourceId},
                              {"filePath", util::PathToUtf8(r.filePath)},
                              {"embeddedId", r.embeddedId},
                              {"parameterId", r.parameterId},
                              {"parameters", r.parameters}};

        if (r.parameterValue)
        {
            ref["parameterValue"] = *r.parameterValue;
        }

        list.push_back(std::move(ref));
    }

    return list;
}

std::string EdgeKey(const GraphEdge& edge)
{
    return edge.from + ">" + edge.to + ":" + std::to_string(edge.fromPort) + "/" + std::to_string(edge.toPort) + "@" +
           nlohmann::json(edge.gain).dump();
}

std::vector<std::string> SortedEdgeKeys(const SignalGraph& graph)
{
    std::vector<std::string> keys;
    keys.reserve(graph.edges.size());

    for (const auto& edge : graph.edges)
    {
        keys.push_back(EdgeKey(edge));
    }

    std::sort(keys.begin(), keys.end());
    return keys;
}

bool SameTopology(const SignalGraph& from, const SignalGraph& to)
{
    if (from.nodes.size() != to.nodes.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < from.nodes.size(); ++i)
    {
        if (from.nodes[i].id != to.nodes[i].id || from.nodes[i].type != to.nodes[i].type)
        {
            return false;
        }
    }

    return SortedEdgeKeys(from) == SortedEdgeKeys(to);
}

template <typename Map> bool SameKeys(const Map& a, const Map& b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (const auto& [key, value] : a)
    {
        if (b.find(key) == b.end())
        {
            return false;
        }
    }

    return true;
}

std::string LabelFor(const GraphNode* node, const NodeLabelResolver& label)
{
    if (node == nullptr)
    {
        return "effect";
    }

    if (label)
    {
        return label(*node);
    }

    if (!node->title.empty())
    {
        return node->title;
    }

    return node->label.empty() ? node->type : node->label;
}
} // namespace

std::string GraphSignature(const SignalGraph& graph)
{
    auto nodes = nlohmann::json::array();

    for (const auto& node : graph.nodes)
    {
        nodes.push_back({{"id", node.id},
                         {"type", node.type},
                         {"label", node.label},
                         {"title", node.title},
                         {"bypassed", !node.enabled},
                         {"params", node.params},
                         {"config", ComparableConfig(node.config)},
                         {"resources", ResourcesJson(node.resources)}});
    }

    return nlohmann::json{{"nodes", std::move(nodes)}, {"edges", SortedEdgeKeys(graph)}}.dump();
}

ChainSnapshot MakeSnapshot(const SignalGraph& graph, std::string presetId, std::string sceneId, std::string label)
{
    ChainSnapshot snapshot;
    snapshot.presetId = std::move(presetId);
    snapshot.sceneId = std::move(sceneId);
    snapshot.graph = graph;
    snapshot.signature = GraphSignature(graph);
    snapshot.label = std::move(label);
    return snapshot;
}

std::optional<std::vector<NodeStateChange>> DiffNodeStates(const SignalGraph& from, const SignalGraph& to)
{
    if (!SameTopology(from, to))
    {
        return std::nullopt;
    }

    std::vector<NodeStateChange> changes;

    for (const auto& target : to.nodes)
    {
        const auto* previous = from.FindNode(target.id);

        if (previous == nullptr)
        {
            return std::nullopt;
        }

        if (previous->label != target.label || previous->title != target.title)
        {
            return std::nullopt;
        }

        if (ResourcesJson(previous->resources) != ResourcesJson(target.resources))
        {
            return std::nullopt;
        }

        const auto previousConfig = ComparableConfig(previous->config);
        const auto targetConfig = ComparableConfig(target.config);

        if (!SameKeys(previous->params, target.params) || !SameKeys(previousConfig, targetConfig))
        {
            return std::nullopt;
        }

        NodeStateChange change;
        change.nodeId = target.id;

        for (const auto& [key, value] : target.params)
        {
            if (previous->params.at(key) != value)
            {
                change.params.emplace_back(key, value);
            }
        }

        for (const auto& [key, value] : targetConfig)
        {
            if (previousConfig.at(key) != value)
            {
                change.config.emplace_back(key, value);
            }
        }

        if (previous->enabled != target.enabled)
        {
            change.bypassed = !target.enabled;
        }

        if (!change.params.empty() || !change.config.empty() || change.bypassed)
        {
            changes.push_back(std::move(change));
        }
    }

    return changes;
}

std::string DescribeGraphChange(const SignalGraph* from, const SignalGraph& to, const NodeLabelResolver& label)
{
    if (from == nullptr)
    {
        return "Chain edit";
    }

    std::set<std::string> fromIds, toIds;

    for (const auto& node : from->nodes)
    {
        fromIds.insert(node.id);
    }

    for (const auto& node : to.nodes)
    {
        toIds.insert(node.id);
    }

    for (const auto& node : to.nodes)
    {
        if (fromIds.count(node.id) == 0)
        {
            return "Add " + LabelFor(&node, label);
        }
    }

    for (const auto& node : from->nodes)
    {
        if (toIds.count(node.id) == 0)
        {
            return "Remove " + LabelFor(&node, label);
        }
    }

    for (const auto& node : to.nodes)
    {
        if (const auto* before = from->FindNode(node.id); before != nullptr && before->type != node.type)
        {
            return "Replace " + LabelFor(&node, label);
        }
    }

    for (std::size_t i = 0; i < from->nodes.size() && i < to.nodes.size(); ++i)
    {
        if (from->nodes[i].id != to.nodes[i].id)
        {
            return "Reorder chain";
        }
    }

    // A split gaining or losing branches: an edit to its edges, which no node state shows.
    const auto branches = [](const SignalGraph& graph, const std::string& id) {
        return std::count_if(graph.edges.begin(), graph.edges.end(),
                             [&id](const GraphEdge& e) { return e.from == id; });
    };

    for (const auto& node : to.nodes)
    {
        const auto before = branches(*from, node.id), after = branches(to, node.id);

        if (before > 1 && after > 1 && before != after)
        {
            return LabelFor(&node, label) + ": " + std::to_string(after) + " branches";
        }
    }

    for (const auto& node : to.nodes)
    {
        if (const auto* before = from->FindNode(node.id); before != nullptr && before->title != node.title)
        {
            return "Rename " + LabelFor(&node, label);
        }
    }

    const auto changes = DiffNodeStates(*from, to);

    if (!changes)
    {
        return "Chain edit";
    }

    if (changes->empty())
    {
        return "No change";
    }

    if (changes->size() > 1)
    {
        return "Edit " + std::to_string(changes->size()) + " effects";
    }

    const auto& first = changes->front();
    const auto name = LabelFor(to.FindNode(first.nodeId), label);

    if (first.bypassed)
    {
        return name + (*first.bypassed ? " off" : " on");
    }

    if (first.params.size() == 1)
    {
        return name + " " + first.params.front().first;
    }

    return name + " settings";
}

// ── The A/B pair of stacks ──────────────────────────────────────────────────────

void ChainHistory::Reset(const ChainSnapshot& seed)
{
    ChainSnapshot loaded = seed;
    loaded.label = "Loaded";
    mA = Stack{{loaded}, 0};
    mB = Stack{{loaded}, 0};
    mActive = Slot::A;
}

bool ChainHistory::IsSeeded() const
{
    return Live().index >= 0;
}

const ChainSnapshot* ChainHistory::Current() const
{
    const auto& stack = Live();
    return stack.index >= 0 ? &stack.entries[static_cast<std::size_t>(stack.index)] : nullptr;
}

void ChainHistory::PushTo(Stack& stack, ChainSnapshot snapshot)
{
    if (stack.index >= 0 && stack.entries[static_cast<std::size_t>(stack.index)].signature == snapshot.signature)
    {
        return;
    }

    stack.entries.resize(static_cast<std::size_t>(stack.index + 1));
    stack.entries.push_back(std::move(snapshot));

    if (stack.entries.size() > kChainHistoryLimit)
    {
        stack.entries.erase(stack.entries.begin());
    }

    stack.index = static_cast<int>(stack.entries.size()) - 1;
}

void ChainHistory::Push(ChainSnapshot snapshot)
{
    PushTo(Live(), std::move(snapshot));
}

void ChainHistory::AmendCurrent(ChainSnapshot snapshot)
{
    auto& stack = Live();

    if (stack.index < 0)
    {
        Push(std::move(snapshot));
        return;
    }

    auto& entry = stack.entries[static_cast<std::size_t>(stack.index)];
    snapshot.label = entry.label;
    entry = std::move(snapshot);
}

bool ChainHistory::CanUndo() const
{
    return Live().index > 0;
}

bool ChainHistory::CanRedo() const
{
    const auto& stack = Live();
    return stack.index >= 0 && stack.index < static_cast<int>(stack.entries.size()) - 1;
}

const ChainSnapshot* ChainHistory::Undo()
{
    if (!CanUndo())
    {
        return nullptr;
    }

    auto& stack = Live();
    --stack.index;
    return &stack.entries[static_cast<std::size_t>(stack.index)];
}

const ChainSnapshot* ChainHistory::Redo()
{
    if (!CanRedo())
    {
        return nullptr;
    }

    auto& stack = Live();
    ++stack.index;
    return &stack.entries[static_cast<std::size_t>(stack.index)];
}

std::string ChainHistory::UndoLabel() const
{
    // The edit undo would reverse: the one that produced the state under the cursor.
    return CanUndo() ? Live().entries[static_cast<std::size_t>(Live().index)].label : std::string();
}

std::string ChainHistory::RedoLabel() const
{
    return CanRedo() ? Live().entries[static_cast<std::size_t>(Live().index + 1)].label : std::string();
}

const ChainSnapshot* ChainHistory::SetActiveSlot(Slot slot)
{
    if (slot == mActive)
    {
        return nullptr;
    }

    mActive = slot;
    return Current();
}

bool ChainHistory::CopyActiveToOther()
{
    const auto* current = Current();

    if (current == nullptr)
    {
        return false;
    }

    ChainSnapshot copy = *current;
    copy.label = std::string("Copied from ") + (mActive == Slot::A ? "A" : "B");
    PushTo(mActive == Slot::A ? mB : mA, std::move(copy));
    return true;
}

int ChainHistory::EditCount(Slot slot) const
{
    const auto& stack = slot == Slot::A ? mA : mB;
    return std::max(0, stack.index);
}
} // namespace guitarfx::uiclient
