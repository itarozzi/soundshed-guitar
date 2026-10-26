// The effect page's controls that are not parameters: a node's model and IR slots, the list a
// long enum chooses from, a split's branch count, and renaming a node.
//
// A slot is the web UI's resource picker (signalPath/paramsPanel/resourceSelector.ts): what is
// loaded, previous and next through the library, the whole library by category, a file from
// disk (the engine shows the dialog and adds the file to the library), and clearing an optional
// slot such as the IR Cabinet's IR B.

#include "ui/Shell.h"

#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// The library's entries of one type in the order the picker lists them and previous/next
/// steps: by category, then by name.
std::vector<const LibraryResource*> OrderedResources(const ClientState& state, const std::string& resourceType)
{
    std::vector<const LibraryResource*> list;
    if (const auto it = state.resources.find(resourceType); it != state.resources.end())
        for (const auto& resource : it->second)
            list.push_back(&resource);
    std::stable_sort(list.begin(), list.end(), [](const LibraryResource* a, const LibraryResource* b) {
        return a->category != b->category ? a->category < b->category : a->name < b->name;
    });
    return list;
}

/// The slot's resource reference, or null when it is empty.
const guitarfx::ResourceRef* SlotRef(const guitarfx::GraphNode& node, int resourceIndex)
{
    const auto index = static_cast<std::size_t>(std::max(0, resourceIndex));
    if (index >= node.resources.size())
        return nullptr;
    const auto& ref = node.resources[index];
    return ref.IsLibraryRef() || ref.IsFilePath() ? &ref : nullptr;
}

/// Where the slot's resource sits in `list`, or -1.
int IndexIn(const std::vector<const LibraryResource*>& list, const guitarfx::ResourceRef* ref)
{
    if (ref == nullptr)
        return -1;
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if ((!ref->resourceId.empty() && list[i]->id == ref->resourceId) ||
            (ref->IsFilePath() && list[i]->filePath == guitarfx::util::PathToUtf8(ref->filePath)))
            return static_cast<int>(i);
    }
    return -1;
}
} // namespace

Ptr Shell::BuildResourceSlot(const guitarfx::GraphNode& node, const std::string& resourceType, int resourceIndex,
                             const std::string& label, bool optional)
{
    auto& commands = mSession.Commands();
    const std::string nodeId = node.id;
    const auto* ref = SlotRef(node, resourceIndex);
    const auto list = OrderedResources(mState, resourceType);
    const int at = IndexIn(list, ref);
    const bool missing = ref != nullptr && std::find(mState.missingNodeResources.begin(), mState.missingNodeResources.end(),
                                                     nodeId) != mState.missingNodeResources.end();

    std::string name = ref != nullptr ? NodeResourceName(mState, node, static_cast<std::size_t>(std::max(0, resourceIndex))) : std::string();
    if (name.empty())
        name = optional ? "None" : "Choose...";

    auto heading = std::make_shared<Text>(mTheme, Upper(label), 11.0f, Weight::SemiBold);
    heading->Colour(mTheme.TextSecondary());
    std::vector<Ptr> headingRow{heading};
    if (missing)
    {
        auto warning = std::make_shared<Text>(mTheme, "\xE2\x9A\xA0  File missing", 11.0f, Weight::SemiBold);
        warning->Colour(mTheme.Warning()).Alignment(Text::Align::Right);
        headingRow.push_back(warning);
    }

    // Previous and next wrap round the library; with nothing loaded they start at either end.
    auto step = [this, nodeId, resourceType, resourceIndex](int delta) {
        return [this, nodeId, resourceType, resourceIndex, delta] {
            const auto* n = SelectedNode();
            if (n == nullptr || n->id != nodeId)
                return;
            const auto ordered = OrderedResources(mState, resourceType);
            if (ordered.empty())
                return;
            const int count = static_cast<int>(ordered.size());
            const int now = IndexIn(ordered, SlotRef(*n, resourceIndex));
            const int next = now < 0 ? (delta > 0 ? 0 : count - 1) : ((now + delta) % count + count) % count;
            mSession.Commands().SetNodeResource(nodeId, resourceType, ordered[static_cast<std::size_t>(next)]->id, resourceIndex);
        };
    };

    auto prev = std::make_shared<Button>(mTheme, "\xE2\x80\xB9", step(-1), Button::Style::Plain);
    prev->MinWidth(40).TextSize(18).Enabled(!list.empty());
    auto next = std::make_shared<Button>(mTheme, "\xE2\x80\xBA", step(1), Button::Style::Plain);
    next->MinWidth(40).TextSize(18).Enabled(!list.empty());

    auto choose = std::make_shared<Button>(mTheme, name + "  \xE2\x96\xBE",
                                           [this, nodeId, resourceType, resourceIndex] {
                                               OpenResourcePicker(nodeId, resourceType, resourceIndex);
                                           },
                                           Button::Style::Plain);
    choose->Flexible(true).MinWidth(120).Selected(at >= 0);

    auto browse = std::make_shared<Button>(mTheme, "Browse...",
                                           [&commands, nodeId, resourceType, resourceIndex] {
                                               commands.BrowseNodeResource(nodeId, resourceType, resourceIndex);
                                           },
                                           Button::Style::Plain);
    browse->TextSize(metrics::kSmallText);

    std::vector<Ptr> row{prev, HGap(6), choose, HGap(6), next};
    if (optional)
    {
        auto clear = std::make_shared<Button>(mTheme, "\xE2\x9C\x95",
                                              [&commands, nodeId, resourceType, resourceIndex] {
                                                  commands.SetNodeResource(nodeId, resourceType, {}, resourceIndex);
                                              },
                                              Button::Style::Plain);
        clear->MinWidth(40).Enabled(ref != nullptr);
        row.push_back(HGap(6));
        row.push_back(clear);
    }
    row.push_back(HGap(6));
    row.push_back(browse);

    return Col({FixedHeight(20, Row(headingRow)), VGap(4), FixedHeight(metrics::kRowHeight, Row(row))});
}

void Shell::OpenResourcePicker(const std::string& nodeId, const std::string& resourceType, int resourceIndex)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
    if (node == nullptr)
        return;

    const auto list = OrderedResources(mState, resourceType);
    const int at = IndexIn(list, SlotRef(*node, resourceIndex));
    const std::string icon = resourceType == "ir" ? "speaker" : resourceType == "nam" ? "amp" : "package";

    std::vector<MenuItem> items;
    items.push_back({"Browse for a file...", false, false, [this, nodeId, resourceType, resourceIndex] {
                         mSession.Commands().BrowseNodeResource(nodeId, resourceType, resourceIndex);
                     }, {}, "folder"});

    std::string category = "\x01"; // no category yet
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto* resource = list[i];
        if (resource->category != category)
        {
            category = resource->category;
            items.push_back({category.empty() ? "UNCATEGORISED" : Upper(category), false, true, {}});
        }
        MenuItem item{resource->name, static_cast<int>(i) == at, false,
                      [this, nodeId, resourceType, resourceIndex, id = resource->id] {
                          mSession.Commands().SetNodeResource(nodeId, resourceType, id, resourceIndex);
                      }};
        item.detail = resource->fileMissing ? "missing" : std::string();
        item.icon = icon;
        items.push_back(std::move(item));
    }

    if (list.empty())
        items.push_back({"The library has none yet", false, true, {}});

    OpenMenu(resourceType == "ir" ? "Impulse responses" : resourceType == "nam" ? "Models" : "Resources", std::move(items));
}

void Shell::OpenEnumPicker(const std::string& nodeId, const EffectParamInfo& info)
{
    const auto* node = SelectedNode();
    if (node == nullptr || node->id != nodeId)
        return;

    const int current = EnumLabelIndex(info, NodeParamValue(*node, info));
    std::vector<MenuItem> items;
    std::string group = "\x01";
    for (std::size_t i = 0; i < info.labels.size(); ++i)
    {
        // The engine's labelInfo groups a long list (Pickup Match's pickups by maker).
        if (i < info.labelGroups.size() && info.labelGroups[i] != group)
        {
            group = info.labelGroups[i];
            if (!group.empty())
                items.push_back({group, false, true, {}});
        }
        MenuItem item{info.labels[i], static_cast<int>(i) == current, false, [this, nodeId, info, i] {
                          mSession.Commands().SetNodeParam(nodeId, info.key, EnumLabelValue(info, i));
                          Mark(kValues | kPage);
                      }};
        if (i < info.labelSubgroups.size())
            item.detail = info.labelSubgroups[i];
        items.push_back(std::move(item));
    }
    OpenMenu(info.name, std::move(items));
}

Ptr Shell::BuildSplitBranches(const guitarfx::GraphNode& splitter)
{
    // The count is the splitter's outgoing edges; a branch holding effects cannot be removed,
    // so a count below those is offered disabled (splitBranches.ts).
    int count = 0;
    int holding = 0;
    if (mState.activePreset)
    {
        for (const auto& edge : mState.activePreset->graph.edges)
            if (edge.from == splitter.id)
                ++count;
        for (const auto& item : BuildChainLayout(mState.activePreset->graph).items)
            if (item.kind == ChainItem::Kind::Parallel && item.splitNodeId == splitter.id)
                holding = static_cast<int>(std::count_if(item.branches.begin(), item.branches.end(),
                                                         [](const ChainBranch& b) { return !b.nodeIds.empty(); }));
    }

    auto heading = std::make_shared<Text>(mTheme, "BRANCHES", 11.0f, Weight::SemiBold);
    heading->Colour(mTheme.TextSecondary());

    std::vector<Ptr> row;
    for (int n = 2; n <= 4; ++n)
    {
        auto b = std::make_shared<Button>(mTheme, std::to_string(n),
                                          [this, id = splitter.id, n, count] {
                                              if (n != count)
                                                  mSession.Commands().SetSplitBranchCount(id, n);
                                          },
                                          Button::Style::Plain);
        b->MinWidth(52).Selected(n == count).Enabled(n >= holding);
        if (!row.empty())
            row.push_back(HGap(6));
        row.push_back(b);
    }

    std::vector<Ptr> actions{Row(row), HGap(16)};
    if (holding <= 1)
    {
        auto collapse = std::make_shared<Button>(mTheme, "Back to one lane",
                                                 [this, id = splitter.id] { mSession.Commands().CollapseSplit(id); },
                                                 Button::Style::Plain);
        collapse->TextSize(metrics::kSmallText);
        actions.push_back(collapse);
    }
    actions.push_back(Stretch());

    auto hint = std::make_shared<Text>(mTheme, "Each branch runs in parallel and meets the others at the mixer, whose levels set the blend.",
                                       metrics::kSmallText);
    hint->Colour(mTheme.TextMuted());
    return Col({FixedHeight(20, heading), VGap(4), FixedHeight(metrics::kRowHeight, Row(actions)), VGap(8), FixedHeight(18, hint)});
}

void Shell::RenameNodeWithPrompt(const std::string& nodeId)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
    if (node == nullptr || IsBoundaryNode(*node))
        return;

    OpenPrompt("Rename effect", NodeDisplayName(mState, *node), "Rename",
               [this, nodeId](const std::string& entered) { mSession.Commands().RenameNode(nodeId, entered); });
}
} // namespace nanoq::ui
