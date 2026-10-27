// The chain strip: the active preset's signal chain drawn as the web UI draws it
// (core/ui/ts/signalPath.ts, css/signal-path/): the input, each effect as a card with its icon,
// the output, cables between them with a + on each to insert an effect there, and parallel
// branches stacked between a split and the mixer that joins them. When the chain is wider than
// the window it wraps onto more lines, joined by a cable that runs back under each line
// (signalPath/chainRow.ts), instead of scrolling sideways.
//
// A tap selects a card; the power button in its corner (shown on hover, and while it is off)
// bypasses it; a double tap opens the effect picker to replace it; a right click opens its menu.

#include "ui/Shell.h"

#include "ui/ChainWidgets.h"

#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace nanoq::ui
{
using guitarfx::uiclient::ChainItem;
using guitarfx::uiclient::kInputNodeId;
using guitarfx::uiclient::kOutputNodeId;

namespace
{
/// One unit the chain wraps by: a cable and what it leads into (as chainRow.ts's segments).
struct Segment
{
    std::vector<Ptr> parts;
    float width = 0.0f;
    float height = kCardHeight;
};

/// A node class's accent on the chain (css/signal-path/nodes.css, .signal-node.amp and the rest):
/// not the library's category colours, which are brighter.
el::color NodeAccent(const std::string& nodeClass)
{
    static const std::map<std::string, std::uint32_t> kAccents{
        {"amp", 0xf0a24c},        {"cab", 0xaeb9d0},   {"eq", 0x61b4d6},     {"dynamics", 0xcf7575},
        {"modulation", 0x8c79db}, {"delay", 0x64c2aa}, {"reverb", 0x7fa4ee}, {"utility", 0x97a1b4}};
    const auto it = kAccents.find(nodeClass);
    return Theme::Argb(0xff000000u | (it != kAccents.end() ? it->second : 0x97a1b4u));
}
} // namespace

std::string Shell::ChainSignature() const
{
    if (!mState.activePreset)
        return {};

    // The width decides where lines wrap, so a resize past a break redraws.
    std::string s = mSelectedNode + "|" + mState.activePresetId + "|" + mState.activeSceneId + "|" +
                    std::to_string(static_cast<int>(ViewSize().x / 16.0f)) + "x" +
                    std::to_string(mWide ? static_cast<int>(ViewSize().y / 24.0f) : 0);
    for (const auto& node : mState.activePreset->graph.nodes)
    {
        s += "|" + node.id + ":" + node.type + ":" + (node.enabled ? "1" : "0") + ":" +
             guitarfx::uiclient::NodeDisplayName(mState, node) + ":" + guitarfx::uiclient::NodeArchitectureBadge(mState, node);
    }
    for (const auto& edge : mState.activePreset->graph.edges)
        s += "|" + edge.from + ">" + edge.to + "#" + std::to_string(edge.fromPort);
    for (const auto& missing : mState.missingNodeResources)
        s += "|!" + missing;
    return s;
}

Ptr Shell::BuildChain()
{
    using namespace guitarfx::uiclient;

    mChainCards.clear();
    if (!mState.activePreset)
        return VGap(0);

    const auto& graph = mState.activePreset->graph;
    const auto layout = BuildChainLayout(graph);

    // The input and output are nodes of the graph (their trim is edited like a parameter); a
    // graph that lacks one still gets its card, under the layout's own id.
    auto boundaryId = [&graph](bool input) {
        for (const auto& node : graph.nodes)
            if (node.type == (input ? guitarfx::kNodeTypeInput : guitarfx::kNodeTypeOutput) ||
                node.id == (input ? kInputNodeId : kOutputNodeId))
                return node.id;
        return std::string(input ? kInputNodeId : kOutputNodeId);
    };
    const auto inputId = boundaryId(true);
    const auto outputId = boundaryId(false);

    auto isMissing = [this](const std::string& nodeId) {
        return std::find(mState.missingNodeResources.begin(), mState.missingNodeResources.end(), nodeId) !=
               mState.missingNodeResources.end();
    };

    // A card for a node of the graph; its width is returned too, for the wrap.
    auto makeCard = [this, &graph, &isMissing, &inputId, &outputId](const std::string& nodeId, float& width) -> Ptr {
        NodeCard::Look look;
        const auto* node = graph.FindNode(nodeId);
        const bool input = nodeId == inputId || (node != nullptr && IsBoundaryNode(*node) && node->type == guitarfx::kNodeTypeInput);
        const bool output = nodeId == outputId || (node != nullptr && IsBoundaryNode(*node) && node->type == guitarfx::kNodeTypeOutput);

        if (input || output)
        {
            look.name = input ? "Input" : "Output";
            look.icon = input ? "guitar" : "output";
            // --node-input and --node-output.
            look.tint = input ? mTheme.Success() : el::rgba(104, 136, 168, 255);
            look.width = kBoundaryWidth;
            look.canBypass = false;
            look.boundary = input ? NodeCard::Boundary::Input : NodeCard::Boundary::Output;
        }
        else if (node != nullptr)
        {
            const auto* type = mState.FindEffectType(node->type);
            const auto category = NodeCategory(mState, *node);
            look.name = NodeDisplayName(mState, *node);
            look.detail = type != nullptr ? type->name : std::string();
            // Renamed, a model or IR node names what it plays under the user's name (signalPath.ts).
            if (!node->title.empty() && type != nullptr && type->requiresResource)
                if (auto resource = NodeResourceName(mState, *node); !resource.empty())
                    look.detail = std::move(resource);
            if (look.detail == look.name)
                look.detail.clear();
            look.badge = NodeArchitectureBadge(mState, *node);
            look.icon = mPresentation.IconFor(node->type, category);
            look.tint = NodeAccent(mPresentation.NodeClass(category));
            look.enabled = node->enabled;
            look.missing = isMissing(nodeId);
            look.canBypass = !IsMixerType(node->type) && !IsSplitterType(node->type);
            if (type != nullptr && (type->requiresResource || !type->resourceType.empty()))
                look.thumbnail = mImages.Find(mPresentation.EquipmentImage(node->type, category));
            look.width = CardWidthFor(look.name, look.detail);
        }
        else
        {
            width = 0.0f;
            return HGap(0);
        }

        look.selected = nodeId == mSelectedNode;
        width = look.width;
        const bool canBypass = look.canBypass;
        auto card = std::make_shared<NodeCard>(mTheme, mIcons, std::move(look));
        card->onSelect = [this, nodeId] {
            SelectNode(nodeId);
            mChainFocused = true;
        };
        card->onMenu = [this, nodeId] { OpenNodeMenu(nodeId); };
        if (canBypass)
        {
            card->onToggle = [this, nodeId] {
                if (const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr)
                    mSession.Commands().SetNodeBypassed(nodeId, n->enabled);
            };
            card->onReplace = [this, nodeId] { OpenEffectPicker({}, nodeId); };
        }
        mChainCards.emplace_back(nodeId, card);
        return card;
    };

    // A cable's +: inserts on the cable's own edge of the graph, as the web UI's connectors do,
    // which is the only way into a parallel branch; after `from` when no edge is found.
    auto plus = [this, &graph](const std::string& from, const std::string& to, int fromPort = -1) -> std::function<void()> {
        for (const auto& edge : graph.edges)
            if (edge.from == from && edge.to == to && (fromPort < 0 || edge.fromPort == fromPort))
                return [this, edge] { OpenEffectPickerOnEdge(edge); };
        return [this, from] { OpenEffectPicker(from); };
    };

    // ── The segments: the input, then each cable with what it leads into ──
    std::vector<Segment> segments;
    {
        Segment input;
        input.parts.push_back(makeCard(inputId, input.width));
        segments.push_back(std::move(input));
    }

    std::string previous = inputId;
    bool afterParallel = false;
    for (const auto& item : layout.items)
    {
        Segment segment;
        // Into a parallel block's mixer the cable is drawing only: the branches are its inputs.
        segment.parts.push_back(std::make_shared<Connector>(
            mTheme, afterParallel ? std::function<void()>{}
                                  : plus(previous, item.kind == ChainItem::Kind::Node ? item.nodeId : item.splitNodeId)));
        segment.width = kConnectorWidth;
        const bool joinsBlock = afterParallel;
        afterParallel = item.kind == ChainItem::Kind::Parallel;

        if (item.kind == ChainItem::Kind::Node)
        {
            float w = 0.0f;
            segment.parts.push_back(makeCard(item.nodeId, w));
            segment.width += w;
            previous = item.nodeId;
            // The mixer stays on the line of the block it joins (chainRow.ts's segments).
            if (joinsBlock && !segments.empty())
            {
                auto& block = segments.back();
                block.parts.insert(block.parts.end(), segment.parts.begin(), segment.parts.end());
                block.width += segment.width;
            }
            else
            {
                segments.push_back(std::move(segment));
            }
            continue;
        }

        // A parallel block: the split, each branch as a lane, and the join; the mixer that
        // joins them is the next item.
        const int laneCount = static_cast<int>(std::max<std::size_t>(1, item.branches.size()));
        std::vector<Ptr> lanes;
        float lanesWidth = 0.0f;
        for (const auto& branch : item.branches)
        {
            std::vector<Ptr> lane;
            float laneWidth = 0.0f;
            std::string after = item.splitNodeId;
            for (const auto& id : branch.nodeIds)
            {
                lane.push_back(std::make_shared<Connector>(mTheme, after == item.splitNodeId ? plus(after, id, branch.port) : plus(after, id)));
                float w = 0.0f;
                lane.push_back(makeCard(id, w));
                laneWidth += kConnectorWidth + w;
                after = id;
            }
            // The branch runs on to the join, with a + to add at its end (or into it, empty).
            lane.push_back(std::make_shared<Connector>(
                mTheme, after == item.splitNodeId ? plus(after, item.joinNodeId, branch.port) : plus(after, item.joinNodeId), true));
            laneWidth += kConnectorWidth;
            lanesWidth = std::max(lanesWidth, std::max(laneWidth, 2.0f * kConnectorWidth));
            if (!lanes.empty())
                lanes.push_back(VGap(kLaneGap));
            lanes.push_back(FixedHeight(kCardHeight, Row(lane)));
        }

        const auto splitId = item.splitNodeId;
        segment.parts.push_back(std::make_shared<Rail>(mTheme, mIcons, laneCount, false, splitId == mSelectedNode,
                                                       [this, splitId] {
                                                           SelectNode(splitId);
                                                           mChainFocused = true;
                                                       }));
        segment.parts.push_back(FixedWidth(lanesWidth, Col(lanes)));
        segment.parts.push_back(std::make_shared<Rail>(mTheme, mIcons, laneCount, true, false, std::function<void()>{}));
        segment.width += kRailWidth * 2.0f + lanesWidth;
        segment.height = static_cast<float>(laneCount) * kCardHeight + static_cast<float>(laneCount - 1) * kLaneGap;
        // The mixer that joins the branches follows as a node item of its own.
        segments.push_back(std::move(segment));
    }

    {
        Segment output;
        output.parts.push_back(std::make_shared<Connector>(mTheme, plus(previous, outputId)));
        float w = 0.0f;
        output.parts.push_back(makeCard(outputId, w));
        output.width = kConnectorWidth + w;
        segments.push_back(std::move(output));
    }

    // ── Lines: as many segments as fit the width, never fewer than one; one line, scrolling
    // sideways, with the wrap switched off ──
    const float viewWidth = StageWidth();
    const float room = mWrapChain ? std::max(200.0f, viewWidth - 2.0f * kSidePad - 2.0f * kWireEndWidth) : 1e9f;

    struct Line
    {
        std::vector<const Segment*> segments;
        float width = 0.0f;
        float height = kCardHeight;
    };
    std::vector<Line> lines(1);
    for (const auto& segment : segments)
    {
        if (!lines.back().segments.empty() && lines.back().width + segment.width > room)
            lines.emplace_back();
        auto& line = lines.back();
        line.segments.push_back(&segment);
        line.width += segment.width;
        line.height = std::max(line.height, segment.height);
    }

    // The lines are centred as one block, as the web UI centres the chain in its stage; each line
    // starts at the same place, with room for the cable in on the left.
    float widest = 0.0f;
    for (const auto& line : lines)
        widest = std::max(widest, line.width);
    const float indent = std::max(kSidePad, (viewWidth - widest - 2.0f * kWireEndWidth) * 0.5f);

    std::vector<Ptr> rows;
    float total = 0.0f;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        const bool first = i == 0, last = i + 1 == lines.size();
        std::vector<Ptr> row{HGap(indent)};
        row.push_back(first ? HGap(kWireEndWidth) : std::static_pointer_cast<el::element>(std::make_shared<WireEnd>(mTheme, false)));
        for (const auto* segment : line.segments)
            for (const auto& part : segment->parts)
                row.push_back(part);
        if (!last)
            row.push_back(std::make_shared<WireEnd>(mTheme, true));
        row.push_back(Stretch());
        rows.push_back(FixedHeight(line.height, Row(row)));
        total += line.height;

        if (!last)
        {
            const float from = indent + kWireEndWidth + line.width + kWireEndWidth * 0.5f;
            const float to = indent + kWireEndWidth * 0.5f;
            rows.push_back(std::make_shared<ReturnRoute>(mTheme, from, to));
            total += kRouteHeight;
        }
    }

    // The block sits in the middle of the stage, between the scene chips and the add button; it
    // scrolls when taller than that, and sideways when the wrap is off.
    SyncChainLights();
    if (mWide)
    {
        // The full layout's chain is as tall as its lines, up to a third of the window, so the
        // editor under it keeps the rest; past that it scrolls.
        const float wanted = total + 24.0f;
        const float most = std::max(kCardHeight + 24.0f, ViewSize().y / 3.0f);
        mChainAreaHeight = std::min(wanted, most) + 24.0f + 30.0f; // the scene chips, the add button
        if (!mWrapChain)
        {
            const float lineWidth = 2.0f * indent + 2.0f * kWireEndWidth + widest;
            return FixedHeight(wanted, HScroll(PadXY(0, 12, FixedWidth(lineWidth, Col(rows)))));
        }
        if (wanted > most)
            return FixedHeight(most, VScroll(PadXY(0, 12, Col(rows))));
        return FixedHeight(wanted, PadXY(0, 12, Col(rows)));
    }
    const float stageHeight = ViewSize().y - metrics::kTopBarHeight - metrics::kFooterHeight - 24.0f - 44.0f;
    if (!mWrapChain)
    {
        const float lineWidth = 2.0f * indent + 2.0f * kWireEndWidth + widest;
        auto block = FixedWidth(lineWidth, Col(rows));
        return Col({std::make_shared<el::element>(), FixedHeight(total + 24.0f, HScroll(PadXY(0, 12, block))),
                    std::make_shared<el::element>()});
    }
    if (total + 24.0f > stageHeight)
        return VScroll(PadXY(0, 12, Col(rows)));
    return Col({std::make_shared<el::element>(), FixedHeight(total, Col(rows)), std::make_shared<el::element>()});
}

void Shell::SyncChainLights()
{
    const auto& telemetry = mState.telemetry;
    const std::string scope = mState.activePresetId + "::";

    auto lightFor = [](const guitarfx::uiclient::LevelReading& reading) {
        if (reading.clipped)
            return NodeCard::Light::Clip;
        if (reading.channelCount > 0 && reading.peakDb > -60.0)
            return NodeCard::Light::Live;
        return NodeCard::Light::Idle;
    };

    for (auto& [nodeId, card] : mChainCards)
    {
        NodeCard::Light light = NodeCard::Light::None;
        if (card->BoundaryKind() == NodeCard::Boundary::Input)
            light = lightFor(telemetry.input);
        else if (card->BoundaryKind() == NodeCard::Boundary::Output)
            light = lightFor(telemetry.output);
        else if (card->Enabled())
        {
            const auto it = telemetry.nodes.find(scope + nodeId);
            light = it != telemetry.nodes.end() ? lightFor(it->second) : NodeCard::Light::Idle;
        }

        if (card->SetLight(light))
            mSession.View().refresh(*card);
    }
}
} // namespace nanoq::ui
