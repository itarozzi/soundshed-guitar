// The effect page: the selected node drawn as the web UI's effect view draws it at its compact
// density (core/ui/ts/signalPath/paramsPanel/panel.ts, css/signal-path/params-panel.css): a
// panel hanging from the preset bar, centred on the stage, with a header (the node's light, its
// name with a pencil to rename it, its category and effect, Presets and the on/off switch) over
// inset sections: the model or IR slots (NodeControls.cpp) with the Main / Advanced tabs beside
// them, a split's branch count, and the parameters in groups, each a knob, a switch or a list.
// A parameter the engine declares with `visibleWhen` shows only while that holds.

#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
namespace artist = cycfi::artist;

/// The one control the input and output nodes carry: their gain in dB, which the effect catalog
/// does not list since they are not effects.
EffectParamInfo BoundaryTrim()
{
    EffectParamInfo trim;
    trim.key = guitarfx::kBoundaryGainParam;
    trim.name = "Gain";
    trim.unit = "dB";
    trim.minValue = guitarfx::kBoundaryGainMinDb;
    trim.maxValue = guitarfx::kBoundaryGainMaxDb;
    trim.defaultValue = 0.0;
    return trim;
}

// The web UI's measurements: 60 px cells 13 apart in groups padded 13 at the sides, inside a
// section padded 8, inside the panel padded 10; the panel at least 448 wide.
constexpr float kCellWidth = 60.0f;
constexpr float kCellHeight = 63.0f;
constexpr float kPickerWidth = 120.0f;
constexpr float kCellGap = 13.0f;
constexpr float kGroupPadX = 13.0f;
constexpr float kGroupGap = 13.0f;
constexpr float kSectionPad = 8.0f;
constexpr float kPanelPad = 10.0f;
constexpr float kPanelMinWidth = 448.0f;
constexpr float kHeaderHeight = 26.0f;

/// An enum shown as a list rather than turned through on a knob: the engine asks for it.
bool WantsPicker(const EffectParamInfo& info)
{
    return info.control == "picker";
}

/// The panel hanging from the preset bar (.default-effect-shell): a dark surface shading down
/// from its top, its category's colour faint along the top, a deep shadow under it.
class EffectPanel final : public el::element
{
public:
    EffectPanel(const Theme& theme, el::color tint) : mTheme(theme), mTint(tint) {}

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const bool light = mTheme.IsLight();
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 18.0f}, 28.0f, el::rgba(0, 0, 0, light ? 40 : 128));
            cnv.fill_style(light ? mTheme.Card() : el::rgba(10, 11, 14, 255));
            cnv.begin_path();
            cnv.add_round_rect(b, 0.0f);
            cnv.fill();
        }
        if (!light)
        {
            auto state = cnv.new_state();
            artist::canvas::linear_gradient face{b.top_left(), el::point{b.left + b.height() * 0.36f, b.bottom}};
            face.add_color_stop(0.0f, el::rgba(26, 28, 33, 245));
            face.add_color_stop(1.0f, el::rgba(10, 11, 14, 245));
            cnv.fill_style(face);
            cnv.fill_rect(b);

            artist::canvas::linear_gradient wash{b.top_left(), b.bottom_left()};
            wash.add_color_stop(0.0f, mTint.opacity(0.12f));
            wash.add_color_stop(std::min(1.0f, 60.0f / std::max(1.0f, b.height())), mTint.opacity(0.02f));
            wash.add_color_stop(1.0f, mTint.opacity(0.0f));
            cnv.fill_style(wash);
            cnv.fill_rect(b);
        }
        cnv.stroke_style(el::rgba(255, 255, 255, light ? 120 : 20));
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left, b.top + 0.5f);
        cnv.line_to(b.right, b.top + 0.5f);
        cnv.stroke();
        cnv.stroke_style(el::rgba(0, 0, 0, light ? 40 : 148));
        cnv.begin_path();
        cnv.move_to(b.left, b.bottom - 0.5f);
        cnv.line_to(b.right, b.bottom - 0.5f);
        cnv.stroke();
    }

private:
    const Theme& mTheme;
    el::color mTint;
};

/// The stage behind the panel: the page colour, shaded darker under the panel.
class EffectBackdrop final : public el::element
{
public:
    EffectBackdrop(const Theme& theme, float shadeTo) : mTheme(theme), mShadeTo(shadeTo) {}

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Background());
        cnv.fill_rect(b);
        if (mTheme.IsLight())
            return;
        auto state = cnv.new_state();
        artist::canvas::linear_gradient shade{b.top_left(), el::point{b.left, b.top + mShadeTo}};
        shade.add_color_stop(0.0f, el::rgba(0, 0, 0, 90));
        shade.add_color_stop(1.0f, el::rgba(0, 0, 0, 0));
        cnv.fill_style(shade);
        cnv.fill_rect(el::rect{b.left, b.top, b.right, std::min(b.bottom, b.top + mShadeTo)});
    }

private:
    const Theme& mTheme;
    float mShadeTo;
};

/// A fixed-size box around `content`: a row of fixed children reports no maximum (Row()), so
/// the size is given rather than measured.
Ptr Sized(float w, float h, Ptr content)
{
    return FixedWidth(w, FixedHeight(h, std::move(content)));
}
} // namespace

std::string Shell::EffectPageSignature() const
{
    const auto* node = SelectedNode();
    if (node == nullptr)
        return "none";

    const float width = ViewSize().x;
    std::string s = node->id + "|" + node->type + "|" + NodeDisplayName(mState, *node) + "|" + (node->enabled ? "1" : "0") +
                    "|" + (mShowAdvanced ? "adv" : "main") + "|" + std::to_string(static_cast<int>(width / 24.0f)) + "x" +
                    std::to_string(static_cast<int>(ViewSize().y / 24.0f)) + "|" +
                    NodeArchitectureBadge(mState, *node);

    const auto* type = mState.FindEffectType(node->type);
    if (type != nullptr)
    {
        s += "|" + std::to_string(type->parameters.size()) + "|" + std::to_string(type->presets.size());
        // A control that comes and goes with another's value redraws the page when it does.
        for (const auto& param : type->parameters)
            if (!param.visibleWhenKey.empty())
                s += IsParamVisible(param, *node, type) ? "v" : "h";
    }

    // The model and IR slots show what is loaded and what the library holds.
    for (const auto& ref : node->resources)
        s += "|r" + ref.resourceType + ":" + ref.resourceId + ":" + guitarfx::util::PathToUtf8(ref.filePath);
    if (type != nullptr && !type->resourceType.empty())
    {
        const auto it = mState.resources.find(type->resourceType);
        s += "|lib" + std::to_string(it != mState.resources.end() ? it->second.size() : 0);
    }
    if (std::find(mState.missingNodeResources.begin(), mState.missingNodeResources.end(), node->id) != mState.missingNodeResources.end())
        s += "|missing";

    // A splitter's Branches control follows its edges.
    if (IsSplitterType(node->type) && mState.activePreset)
        for (const auto& edge : mState.activePreset->graph.edges)
            if (edge.from == node->id)
                s += "|e" + edge.to;
    return s;
}

Ptr Shell::BuildEffectPage()
{
    mControls.clear();
    mBypass.reset();
    mHeaderLed.reset();

    const auto* node = SelectedNode();
    if (node == nullptr)
    {
        auto hint = std::make_shared<Text>(mTheme, "Choose an effect in the chain to see its controls.", 13.0f);
        hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        return Over(PadXY(24, 48, Col({FixedHeight(24, hint), std::make_shared<el::element>()})),
                    std::make_shared<EffectBackdrop>(mTheme, 0.0f));
    }

    const std::string nodeId = node->id;
    const bool boundary = IsBoundaryNode(*node);
    const auto* type = mState.FindEffectType(node->type);
    const auto category = NodeCategory(mState, *node);
    const auto tint = Theme::Argb(mPresentation.CategoryColour(category));
    const bool hasSlots = !boundary && type != nullptr && !type->resourceType.empty() && type->resourceType != "plugin" &&
                          node->type != guitarfx::EffectGuids::kAmpNamBlend;

    // The full layout centres the editor under the chain, no wider than the web UI's panel.
    const float stageWidth = mWide ? std::min(StageWidth(), 900.0f) : StageWidth();
    const float stageHeight = mWide ? ViewSize().y - metrics::kTabBarHeight - metrics::kHeaderHeight - mChainAreaHeight - metrics::kWideFooterHeight
                                    : ViewSize().y - metrics::kTopBarHeight - metrics::kFooterHeight;

    // ── Which controls: this tab's, and only those whose condition holds ──
    std::vector<EffectParamInfo> params;
    bool anyAdvanced = false;
    if (boundary)
    {
        params.push_back(BoundaryTrim());
    }
    else if (type != nullptr)
    {
        for (const auto& param : type->parameters)
        {
            anyAdvanced = anyAdvanced || param.advanced;
            if (param.advanced == mShowAdvanced && IsParamVisible(param, *node, type))
                params.push_back(param);
        }
    }
    if (!anyAdvanced)
        mShowAdvanced = false;

    // ── The groups: each a box of cells ──
    struct Group
    {
        std::string name;
        std::vector<std::pair<float, Ptr>> cells; // width, cell
    };
    std::vector<Group> groups;

    for (const auto& info : params)
    {
        // A group gathers every control of its name, where they are apart in the list too
        // (a NAM amp's Input and Output both in Level), as the web UI groups them.
        auto group = std::find_if(groups.begin(), groups.end(), [&info](const Group& g) { return g.name == info.group; });
        if (group == groups.end())
        {
            groups.push_back({info.group, {}});
            group = std::prev(groups.end());
        }
        auto& target = *group;

        ParamControl control;
        control.info = info;

        if (IsEnumParam(info) && WantsPicker(info))
        {
            // A long list: its current choice on a chip; a tap opens the list.
            control.picker = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            control.picker->Size(kPickerWidth, 26).Caret(true);
            control.picker->onClick = [this, nodeId, info] { OpenEnumPicker(nodeId, info); };
            auto label = std::make_shared<Text>(mTheme, Upper(info.name), 9.0f, Weight::SemiBold);
            label->Colour(mTheme.Pick(el::rgba(201, 209, 225, 184), mTheme.TextSecondary())).Alignment(Text::Align::Centre);
            target.cells.emplace_back(kPickerWidth, Sized(kPickerWidth, kCellHeight, Col({FixedHeight(12, label), VGap(12),
                                                                                               FixedHeight(26, control.picker),
                                                                                               std::make_shared<el::element>()})));
        }
        else if (IsToggleParam(info))
        {
            control.toggle = std::make_shared<ParamToggle>(mTheme, info.name);
            control.toggle->onClick = [this, nodeId, info] {
                const auto* n = SelectedNode();
                if (n == nullptr)
                    return;
                const double now = NodeParamValue(*n, info);
                const double next = now > 0.5 * (info.minValue + info.maxValue) ? info.minValue : info.maxValue;
                mSession.Commands().SetNodeParam(nodeId, info.key, next);
                NoteChainEdit();
                Mark(kValues | kPage);
            };
            target.cells.emplace_back(kCellWidth, control.toggle);
        }
        else
        {
            // A knob: continuous values, and an enum short enough to turn through, as the web
            // UI's Mode knobs are.
            control.knob = std::make_shared<ParamKnob>(mTheme, info.name);
            control.knob->DefaultPosition(ParamValueToPosition(info, info.defaultValue));
            const bool stepped = IsEnumParam(info);
            control.knob->onChange = [this, nodeId, info, stepped, knob = control.knob.get()](double position) {
                const double value = SnapParamValue(info, ParamPositionToValue(info, position));
                knob->SetValueText(FormatParamValue(info, value));
                mSession.Commands().SetNodeParam(nodeId, info.key, value);
                NoteChainEdit();
                if (stepped)
                    Mark(kPage); // a visibleWhen on this enum may show or hide others
            };
            control.knob->onReset = [this, nodeId, info] {
                mSession.Commands().SetNodeParam(nodeId, info.key, info.defaultValue);
                NoteChainEdit();
                Mark(kValues);
            };
            target.cells.emplace_back(kCellWidth, control.knob);
        }

        mControls.push_back(std::move(control));
    }

    // Beside named groups, the controls that have none are titled Other, as the web UI does.
    if (std::any_of(groups.begin(), groups.end(), [](const Group& g) { return !g.name.empty(); }))
        for (auto& g : groups)
            if (g.name.empty())
                g.name = "Other";

    // A group's box: its title over its cells, as wide as they are; an untitled group alone in
    // its row stretches across it with its cells centred (as the Input's Gain and the Doubler's).
    auto groupWidth = [](const Group& g) {
        float w = 2.0f * kGroupPadX;
        for (std::size_t i = 0; i < g.cells.size(); ++i)
            w += g.cells[i].first + (i > 0 ? kCellGap : 0.0f);
        return w;
    };
    // A titled group: 4 above its title, the 12 px title, 2, the cells, 7. An untitled one: 8
    // above the cells and 1 below.
    auto groupHeight = [](const Group& g) { return g.name.empty() ? 8.0f + kCellHeight + 1.0f : 4.0f + 12.0f + 2.0f + kCellHeight + 7.0f; };
    auto buildGroup = [&](const Group& g, bool stretch) -> Ptr {
        std::vector<Ptr> row;
        if (stretch)
            row.push_back(Stretch());
        for (std::size_t i = 0; i < g.cells.size(); ++i)
        {
            if (i > 0)
                row.push_back(HGap(kCellGap));
            row.push_back(FixedWidth(g.cells[i].first, g.cells[i].second));
        }
        row.push_back(Stretch());
        const float top = g.name.empty() ? 8.0f : 4.0f + 12.0f + 2.0f;
        return Over(Pad(kGroupPadX, top, kGroupPadX, g.name.empty() ? 1.0f : 7.0f, FixedHeight(kCellHeight, Row(row))),
                    std::make_shared<InsetBox>(mTheme, g.name));
    };

    // The panel's width: the model slots take the whole stage; otherwise as wide as its groups,
    // and never under the web UI's minimum.
    float groupsWidth = 0.0f;
    for (const auto& g : groups)
        groupsWidth += groupWidth(g) + (groupsWidth > 0.0f ? kGroupGap : 0.0f);
    const float maxPanel = std::max(kPanelMinWidth, stageWidth - 20.0f);
    const float panelWidth =
        hasSlots ? maxPanel : std::clamp(groupsWidth + 2.0f * (kPanelPad + kSectionPad), kPanelMinWidth, maxPanel);
    const float sectionInner = panelWidth - 2.0f * (kPanelPad + kSectionPad);

    // ── Header ──
    const auto titleFont = Font(Weight::SemiBold).size(12.5f);
    const std::string titleText = NodeDisplayName(mState, *node);
    std::string kind = boundary ? std::string(node->type == guitarfx::kNodeTypeInput ? "Input" : "Output")
                                : (type != nullptr ? type->name : node->type);
    // The web UI's subtitle reads "DYNAMICS · NOISE GATE": the category as the engine names it.
    const auto subtitleText = Upper((boundary ? std::string("utility") : category) + " \xC2\xB7 " + kind);
    const auto subtitleFont = Font(Weight::SemiBold).size(9.0f);
    const auto badgeText = boundary ? std::string() : NodeArchitectureBadge(mState, *node);

    std::vector<Ptr> rightSide;
    if (!IsSplitterType(node->type) && !IsMixerType(node->type))
    {
        if (type != nullptr || boundary)
        {
            auto presets = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            presets->Size(77, 26).Label("Presets", 11.0f).Caret(true);
            presets->Disabled(boundary);
            presets->onClick = [this] { OpenEffectPresets(); };
            rightSide.push_back(presets);
            rightSide.push_back(HGap(8));
        }
        mBypass = std::make_shared<PowerSwitch>(mTheme, node->enabled);
        mBypass->Enabled(!boundary);
        // Read the node at the time of the tap: the switch outlives the value it was built with.
        mBypass->onClick = [this, nodeId, boundary] {
            const auto* n = SelectedNode();
            if (boundary || n == nullptr || n->id != nodeId)
                return;
            mSession.Commands().SetNodeBypassed(nodeId, n->enabled);
        };
        rightSide.push_back(mBypass);
    }
    float rightWidth = 0.0f;
    if (!rightSide.empty())
        rightWidth = (type != nullptr || boundary) ? 77.0f + 8.0f + 40.0f : 40.0f;

    const float subtitleWidth = MeasureTextWidth(subtitleText, subtitleFont) + 2.0f;
    const float badgeWidth = badgeText.empty() ? 0.0f : MeasureTextWidth(badgeText, Font(Weight::SemiBold).size(8.0f)) + 12.0f + 6.0f;
    const float pencilWidth = boundary ? 0.0f : 22.0f;
    const float roomForTitle =
        panelWidth - 2.0f * kPanelPad - 14.0f - 6.0f - pencilWidth - 10.0f - subtitleWidth - badgeWidth - rightWidth - 16.0f;
    const float titleWidth = std::clamp(MeasureTextWidth(titleText, titleFont) + 2.0f, 40.0f, std::max(40.0f, roomForTitle));

    mHeaderLed = std::make_shared<HeaderLed>(node->enabled, false);
    auto title = std::make_shared<Text>(mTheme, titleText, 12.5f, Weight::SemiBold);
    title->Colour(mTheme.Pick(el::rgba(238, 242, 255, 255), mTheme.Text()));
    auto subtitle = std::make_shared<Text>(mTheme, subtitleText, 9.0f, Weight::SemiBold);
    subtitle->Colour(mTheme.TextMuted()).Fixed(true);

    std::vector<Ptr> header{mHeaderLed, HGap(6), FixedWidth(titleWidth, title)};
    if (!boundary)
    {
        auto pencil = std::make_shared<IconButton>(mTheme, mIcons, "pencil");
        pencil->Size(20, 20).IconSize(11).Ink(mTheme.TextMuted());
        pencil->onClick = [this, nodeId] { RenameNodeWithPrompt(nodeId); };
        header.push_back(HGap(2));
        header.push_back(Pad(0, 3, 0, 3, pencil));
    }
    header.push_back(HGap(10));
    header.push_back(subtitle);
    if (!badgeText.empty())
    {
        header.push_back(HGap(6));
        header.push_back(std::make_shared<ArchBadge>(badgeText));
    }
    header.push_back(Stretch());
    for (auto& r : rightSide)
        header.push_back(r);

    // ── The controls section ──
    std::vector<Ptr> section;
    float sectionHeight = 0.0f;
    auto addToSection = [&](Ptr element, float height) {
        if (!section.empty())
        {
            section.push_back(VGap(8));
            sectionHeight += 8.0f;
        }
        section.push_back(FixedHeight(height, std::move(element)));
        sectionHeight += height;
    };

    auto tabs = [this, anyAdvanced]() -> Ptr {
        if (!anyAdvanced)
            return nullptr;
        auto tab = [this](const char* label, bool advanced) {
            auto t = std::make_shared<UnderlineTab>(mTheme, label, mShowAdvanced == advanced);
            t->onClick = [this, advanced] {
                mShowAdvanced = advanced;
                Mark(kPage);
            };
            return t;
        };
        auto t = std::make_shared<el::htile_composite>();
        t->push_back(tab("Main", false));
        t->push_back(HGap(4));
        t->push_back(tab("Advanced", true));
        return t;
    };

    bool tabsPlaced = false;
    if (hasSlots)
    {
        const auto& rt = type->resourceType;
        if (node->type == guitarfx::EffectGuids::kCabIr)
        {
            auto a = BuildResourceSlot(*node, rt, 0, "IR A", false);
            if (auto t = tabs())
            {
                a = Row({a, HGap(14), t});
                tabsPlaced = true;
            }
            addToSection(a, 26.0f);
            addToSection(BuildResourceSlot(*node, rt, 1, "IR B", true), 26.0f);
        }
        else
        {
            auto slot = BuildResourceSlot(*node, rt, -1, rt == "nam" ? "Model" : rt == "ir" ? "IR" : "Resource", false);
            if (auto t = tabs())
            {
                slot = Row({slot, HGap(14), t});
                tabsPlaced = true;
            }
            addToSection(slot, 26.0f);
        }
    }
    if (IsSplitterType(node->type))
        addToSection(BuildSplitBranches(*node), 96.0f);
    if (!tabsPlaced)
        if (auto t = tabs())
            addToSection(Row({t, Stretch()}), 26.0f);

    // The groups, laid into rows as they fit the section.
    {
        std::vector<Ptr> row;
        float used = 0.0f;
        float rowHeight = 0.0f;
        bool spans = false;
        auto flush = [&] {
            if (row.empty())
                return;
            if (!spans)
                row.push_back(Stretch());
            addToSection(Row(row), rowHeight);
            row.clear();
            used = 0.0f;
            rowHeight = 0.0f;
            spans = false;
        };
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            const auto& g = groups[i];
            const float w = std::min(groupWidth(g), sectionInner);
            if (!row.empty() && used + kGroupGap + w > sectionInner)
                flush();
            // An untitled group (the only one, since the rest are titled Other) spans its row.
            const bool alone = groups.size() == 1;
            rowHeight = std::max(rowHeight, groupHeight(g));
            if (!row.empty())
            {
                row.push_back(HGap(kGroupGap));
                used += kGroupGap;
            }
            if (alone && g.name.empty())
            {
                row.push_back(buildGroup(g, true));
                used = sectionInner;
                spans = true;
            }
            else
            {
                row.push_back(FixedWidth(w, buildGroup(g, false)));
                used += w;
            }
        }
        flush();
    }

    if (params.empty() && section.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "This effect has no controls.", 12.0f);
        none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        addToSection(none, 40.0f);
    }

    // ── The panel ──
    const float sectionBox = sectionHeight + 2.0f * kSectionPad;
    const float panelHeight = kPanelPad + kHeaderHeight + 10.0f + sectionBox + kPanelPad;
    auto sectionElement = Over(Pad(kSectionPad, kSectionPad, kSectionPad, kSectionPad, Col(section)), std::make_shared<InsetBox>(mTheme));
    auto body = Col({FixedHeight(kHeaderHeight, Row(header)), VGap(10), FixedHeight(sectionBox, sectionElement)});
    auto panel = Over(Pad(kPanelPad, kPanelPad, kPanelPad, kPanelPad, body), std::make_shared<EffectPanel>(mTheme, tint));

    Ptr placed = Row({Stretch(), FixedWidth(panelWidth, FixedHeight(panelHeight, panel)), Stretch()});
    Ptr stage;
    if (panelHeight > stageHeight)
        stage = VScroll(Col({placed, VGap(12)}));
    else
        stage = Col({placed, std::make_shared<el::element>()});

    SyncEffectValues();
    return Over(stage, std::make_shared<EffectBackdrop>(mTheme, panelHeight + 120.0f));
}

void Shell::SyncEffectValues()
{
    const auto* node = SelectedNode();
    if (node == nullptr)
        return;

    for (auto& control : mControls)
    {
        const double value = NodeParamValue(*node, control.info);
        if (control.knob)
        {
            const bool wasDragging = control.knob->Dragging();
            control.knob->Update(ParamValueToPosition(control.info, value), FormatParamValue(control.info, value));
            if (!wasDragging)
                mSession.View().refresh(*control.knob);
        }
        else if (control.toggle)
        {
            control.toggle->Set(value > 0.5 * (control.info.minValue + control.info.maxValue));
            mSession.View().refresh(*control.toggle);
        }
        else if (control.picker)
        {
            std::string text;
            if (const int index = EnumLabelIndex(control.info, value);
                index >= 0 && static_cast<std::size_t>(index) < control.info.labels.size())
                text = control.info.labels[static_cast<std::size_t>(index)];
            control.picker->Label(text, 11.0f);
            mSession.View().refresh(*control.picker);
        }
    }

    if (mBypass)
    {
        mBypass->Set(node->enabled);
        mSession.View().refresh(*mBypass);
    }
}

void Shell::OpenEffectPresets()
{
    const auto* node = SelectedNode();
    const auto* type = node != nullptr ? mState.FindEffectType(node->type) : nullptr;
    if (type == nullptr)
        return;

    const std::string nodeId = node->id;

    // The effect's own, and a factory archive's (these choose its models, IRs and blends), as
    // one list in name order; then the user's.
    auto factory = type->presets;
    if (const auto pack = mState.factoryPackEffectPresets.find(node->type); pack != mState.factoryPackEffectPresets.end())
        factory.insert(factory.end(), pack->second.begin(), pack->second.end());
    std::stable_sort(factory.begin(), factory.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

    std::vector<MenuItem> items;
    for (const auto& preset : factory)
        items.push_back({preset.name, false, false, [this, nodeId, preset] {
                             mSession.Commands().ApplyEffectPreset(nodeId, preset);
                         }});

    if (const auto custom = mState.customEffectPresets.find(node->type);
        custom != mState.customEffectPresets.end() && !custom->second.empty())
    {
        items.push_back({"Yours", false, true, {}});
        for (const auto& preset : custom->second)
            items.push_back({preset.name, false, false, [this, nodeId, preset] {
                                 mSession.Commands().ApplyEffectPreset(nodeId, preset);
                             }});
    }

    items.push_back({"", false, true, {}});
    items.push_back({"Save current settings as a preset...", false, false, [this, nodeId, name = type->name] {
                         OpenPrompt("Save " + name + " preset", "", "Save", [this, nodeId](const std::string& entered) {
                             mSession.Commands().SaveEffectPreset(nodeId, entered);
                         });
                     }});

    OpenMenu(type->name + " presets", std::move(items));
}
} // namespace nanoq::ui
