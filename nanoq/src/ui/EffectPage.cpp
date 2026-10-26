// The effect page: the selected node drawn as the web UI's effect view draws it
// (core/ui/ts/signalPath/visualization.ts, paramsPanel/): on its category's backdrop, a header
// with the node's light, name, kind, presets and on/off switch; its equipment artwork at the
// left when it has some; and at the right its model or IR slots (NodeControls.cpp), the page's
// Main / Advanced tabs, and its parameters in boxed groups, each with a knob, a switch or a
// choice. A parameter the engine declares with `visibleWhen` shows only while that holds.

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

/// The one control the input and output nodes carry: a trim in dB, which the effect catalog
/// does not list since they are not effects.
EffectParamInfo BoundaryTrim()
{
    EffectParamInfo trim;
    trim.key = guitarfx::kBoundaryGainParam;
    trim.name = "Trim";
    trim.unit = "dB";
    trim.minValue = guitarfx::kBoundaryGainMinDb;
    trim.maxValue = guitarfx::kBoundaryGainMaxDb;
    trim.defaultValue = 0.0;
    return trim;
}

constexpr float kCellWidth = 84.0f; // a knob's width; switches and enum choices match it
constexpr float kCellGap = 10.0f;
constexpr float kBoxPad = 14.0f;
constexpr float kBoxGap = 12.0f;
constexpr float kBoxTitle = 26.0f;
constexpr float kArtWidth = 250.0f;
constexpr float kStagePad = 16.0f;
constexpr float kCellHeight = 98.0f; // the knob's own height

/// An enum with more choices than stepping through suits: a menu instead.
bool WantsPicker(const EffectParamInfo& info)
{
    return info.control == "picker" || !info.labelGroups.empty() || info.labels.size() > 6;
}

/// The page's own width, less its margins.
float PageWidth(const el::view& view)
{
    return std::max(280.0f, view.size().x - 32.0f);
}

/// Whether the artwork column shows: only with room for it beside the controls.
bool ShowsArt(float pageWidth)
{
    return pageWidth >= 720.0f;
}

/// The backdrop the whole effect sits on: its category's gradient (the web UI's
/// `--effect-visual-bg`, 145 degrees from the first colour to the second), or the card colour.
class Stage final : public el::element
{
public:
    Stage(const Theme& theme, std::optional<std::pair<Argb, Argb>> gradient) : mTheme(theme), mGradient(gradient) {}

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        cnv.fill_style(mTheme.Card());
        cnv.fill_round_rect(b, 14.0f);
        if (mGradient && !mTheme.IsLight())
        {
            artist::canvas::linear_gradient wash{b.top_left(), b.bottom_right()};
            wash.add_color_stop(0.0f, Theme::Argb(mGradient->first));
            wash.add_color_stop(1.0f, Theme::Argb(mGradient->second));
            cnv.fill_style(wash);
            cnv.fill_round_rect(b, 14.0f);
        }
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, 14.0f);
    }

private:
    const Theme& mTheme;
    std::optional<std::pair<Argb, Argb>> mGradient;
};

/// The effect's artwork, fitted into a dark panel; the icon in its place when it has none.
class ArtPanel final : public el::element
{
public:
    ArtPanel(const Theme& theme, Icons& icons, std::shared_ptr<artist::image> image, std::string icon, el::color tint, bool enabled)
        : mTheme(theme), mIcons(icons), mImage(std::move(image)), mIcon(std::move(icon)), mTint(tint), mEnabled(enabled)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        // Flexible in height, so a short window shrinks it rather than growing to fit it.
        return {{kArtWidth, 90.0f}, {kArtWidth, 240.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(el::rgba(0, 0, 0, 90));
        cnv.fill_round_rect(b, 12.0f);
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, 12.0f);

        const auto inner = b.inset(10.0f, 10.0f);
        if (mImage)
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(inner, 8.0f);
            cnv.clip();
            DrawImageFitted(cnv, *mImage, inner);
            if (!mEnabled)
            {
                cnv.fill_style(el::rgba(0, 0, 0, 120)); // a bypassed effect's picture dims, as its card does
                cnv.fill_rect(inner);
            }
        }
        else
        {
            const float side = std::min(inner.width(), inner.height()) * 0.45f;
            const auto c = artist::center_point(inner);
            mIcons.Draw(cnv, mIcon, el::rect{c.x - side * 0.5f, c.y - side * 0.5f, c.x + side * 0.5f, c.y + side * 0.5f},
                        mEnabled ? mTint : mTheme.TextMuted());
        }
    }

private:
    const Theme& mTheme;
    Icons& mIcons;
    std::shared_ptr<artist::image> mImage;
    std::string mIcon;
    el::color mTint;
    bool mEnabled;
};

/// A group's box: a darker panel with its title at the top, under the group's controls.
class GroupBox final : public el::element
{
public:
    GroupBox(const Theme& theme, std::string title) : mTheme(theme), mTitle(Upper(std::move(title))) {}

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        cnv.fill_style(el::rgba(0, 0, 0, mTheme.IsLight() ? 12 : 70));
        cnv.fill_round_rect(b, 12.0f);
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, 12.0f);
        if (!mTitle.empty())
            FillText(cnv, mTitle, Font(Weight::SemiBold).size(11.5f), mTheme.TextSecondary(),
                     el::rect{b.left, b.top + 4.0f, b.right, b.top + kBoxTitle}, HAlign::Centre);
    }

private:
    const Theme& mTheme;
    std::string mTitle;
};

/// The header's light: green while the effect is on.
class Light final : public el::element
{
public:
    Light(const Theme& theme, bool on) : mTheme(theme), mOn(on) {}

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{14.0f, 14.0f}, {14.0f, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto c = artist::center_point(ctx.bounds);
        if (mOn)
        {
            cnv.fill_style(mTheme.Success().opacity(0.25f));
            cnv.add_circle(artist::circle(c, 6.5f));
            cnv.fill();
        }
        cnv.fill_style(mOn ? mTheme.Success() : mTheme.Error().opacity(0.8f));
        cnv.add_circle(artist::circle(c, 4.0f));
        cnv.fill();
    }

private:
    const Theme& mTheme;
    bool mOn;
};
} // namespace

std::string Shell::EffectPageSignature() const
{
    const auto* node = SelectedNode();
    if (node == nullptr)
        return "none";

    const float width = PageWidth(mSession.View());
    std::string s = node->id + "|" + node->type + "|" + NodeDisplayName(mState, *node) + "|" + (node->enabled ? "1" : "0") +
                    "|" + (mShowAdvanced ? "adv" : "main") + "|" + std::to_string(static_cast<int>(width / 24.0f));

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

    const auto* node = SelectedNode();
    if (node == nullptr)
    {
        auto hint = std::make_shared<Text>(mTheme, "Choose an effect in the chain above to see its controls.", 14.0f);
        hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        return Fill(mTheme.Background(), PadXY(24, 48, FixedHeight(24, hint)));
    }

    auto& commands = mSession.Commands();
    const std::string nodeId = node->id;
    const bool boundary = IsBoundaryNode(*node);
    const auto* type = mState.FindEffectType(node->type);
    const auto category = NodeCategory(mState, *node);
    const auto tint = Theme::Argb(mPresentation.CategoryColour(category));

    const float pageWidth = PageWidth(mSession.View());
    const bool showArt = ShowsArt(pageWidth) && !boundary && !IsSplitterType(node->type) && !IsMixerType(node->type);
    const float controlsWidth = pageWidth - 2.0f * kStagePad - 14.0f - (showArt ? kArtWidth + kStagePad : 0.0f);

    // ── Header: light, name (a tap renames it), kind; presets and the on/off switch ──
    auto title = std::make_shared<Button>(mTheme, NodeDisplayName(mState, *node),
                                          [this, nodeId, boundary] {
                                              if (!boundary)
                                                  RenameNodeWithPrompt(nodeId);
                                          },
                                          Button::Style::Flat);
    // Flexible: a long model name is cut to fit rather than widening the window.
    title->TextSize(17.0f).Flexible(true).MinWidth(120).Alignment(HAlign::Left);

    std::string kind = boundary ? "Level trim" : (type != nullptr ? type->name : node->type);
    if (!boundary)
    {
        if (const auto* look = mPresentation.Category(category))
            kind = look->name + "  \xC2\xB7  " + kind;
        if (const auto badge = NodeArchitectureBadge(mState, *node); !badge.empty())
            kind += "  \xC2\xB7  " + badge;
    }
    auto subtitle = std::make_shared<Text>(mTheme, Upper(kind), 11.0f, Weight::SemiBold);
    subtitle->Colour(mTheme.TextMuted());

    std::vector<Ptr> header{std::make_shared<Light>(mTheme, node->enabled), HGap(2),
                            Col({FixedHeight(32, title), FixedHeight(16, Pad(6, 0, 0, 0, subtitle))}),
                            Stretch(nullptr, 0.001)};
    // The buttons keep their own height in the taller header.
    auto upright = [](Ptr button) { return Pad(0, 7, 0, 7, std::move(button)); };

    if (!boundary && !IsSplitterType(node->type))
    {
        if (type != nullptr && !IsMixerType(node->type))
        {
            auto presets = std::make_shared<Button>(mTheme, "Presets \xE2\x96\xBE", [this] { OpenEffectPresets(); }, Button::Style::Plain);
            presets->MinWidth(100);
            header.push_back(upright(presets));
            header.push_back(HGap(8));
        }

        mBypass = std::make_shared<Button>(
            mTheme, node->enabled ? "On" : "Off", [&commands, nodeId, enabled = node->enabled] {
                commands.SetNodeBypassed(nodeId, enabled);
            },
            Button::Style::Plain);
        mBypass->Dot(true).On(node->enabled).MinWidth(76);
        header.push_back(upright(mBypass));
    }

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

    std::vector<Ptr> controls;

    // The model and IR slots, and a split's branch count, come first.
    if (!boundary && type != nullptr && !type->resourceType.empty() && type->resourceType != "plugin" &&
        node->type != guitarfx::EffectGuids::kAmpNamBlend)
    {
        const auto& rt = type->resourceType;
        if (node->type == guitarfx::EffectGuids::kCabIr)
        {
            controls.push_back(BuildResourceSlot(*node, rt, 0, "IR A", false));
            controls.push_back(VGap(8));
            controls.push_back(BuildResourceSlot(*node, rt, 1, "IR B", true));
        }
        else
        {
            controls.push_back(BuildResourceSlot(*node, rt, -1, rt == "nam" ? "Model" : rt == "ir" ? "IR" : "Resource", false));
        }
        controls.push_back(VGap(10));
    }
    if (IsSplitterType(node->type))
    {
        controls.push_back(BuildSplitBranches(*node));
        controls.push_back(VGap(10));
    }

    if (anyAdvanced)
    {
        auto tab = [this](const char* label, bool advanced) {
            auto b = std::make_shared<Button>(mTheme, label, [this, advanced] {
                mShowAdvanced = advanced;
                Mark(kPage);
            }, Button::Style::Flat);
            b->Selected(mShowAdvanced == advanced).MinWidth(92).TextSize(metrics::kSmallText);
            return b;
        };
        controls.push_back(FixedHeight(40, Row({tab("Main", false), HGap(4), tab("Advanced", true), Stretch()})));
        controls.push_back(VGap(6));
    }

    // ── The groups, each a box of cells, laid into rows as they fit ──
    struct Group
    {
        std::string name;
        std::vector<Ptr> cells;
    };
    std::vector<Group> groups;

    for (const auto& info : params)
    {
        if (groups.empty() || groups.back().name != info.group)
            groups.push_back({info.group, {}});

        ParamControl control;
        control.info = info;

        if (IsToggleParam(info) || IsEnumParam(info))
        {
            const bool picker = IsEnumParam(info) && WantsPicker(info);
            // A switch, or the enum's current choice: a tap steps to the next one, or, for a long
            // list, opens it.
            control.toggle = std::make_shared<Button>(mTheme, "", [this, nodeId, info, picker] {
                const auto* n = SelectedNode();
                if (n == nullptr)
                    return;
                if (picker)
                {
                    OpenEnumPicker(nodeId, info);
                    return;
                }
                const double now = NodeParamValue(*n, info);
                double next = 0.0;
                if (IsToggleParam(info))
                {
                    next = now > 0.5 * (info.minValue + info.maxValue) ? info.minValue : info.maxValue;
                }
                else
                {
                    const auto count = std::max<std::size_t>(1, info.labels.size());
                    const auto index = EnumLabelIndex(info, now);
                    next = EnumLabelValue(info, static_cast<std::size_t>((index + 1) % static_cast<int>(count)));
                }
                mSession.Commands().SetNodeParam(nodeId, info.key, next);
                Mark(kValues | kPage);
            }, Button::Style::Plain);
            control.toggle->MinWidth(kCellWidth).TextSize(metrics::kSmallText);

            auto name = std::make_shared<Text>(mTheme, info.name, metrics::kSmallText, Weight::Medium);
            name->Colour(mTheme.TextSecondary()).Alignment(Text::Align::Centre);
            groups.back().cells.push_back(FixedWidth(kCellWidth, Col({VGap(8), FixedHeight(20, name), VGap(8), FixedHeight(34, control.toggle), Stretch()})));
        }
        else
        {
            control.knob = std::make_shared<ParamKnob>(mTheme, info.name);
            control.knob->DefaultPosition(ParamValueToPosition(info, info.defaultValue));
            control.knob->onChange = [this, nodeId, info, knob = control.knob.get()](double position) {
                const double value = SnapParamValue(info, ParamPositionToValue(info, position));
                knob->SetValueText(FormatParamValue(info, value));
                mSession.Commands().SetNodeParam(nodeId, info.key, value);
            };
            control.knob->onReset = [this, nodeId, info] {
                mSession.Commands().SetNodeParam(nodeId, info.key, info.defaultValue);
                Mark(kValues);
            };
            groups.back().cells.push_back(FixedWidth(kCellWidth, control.knob));
        }

        mControls.push_back(std::move(control));
    }

    // A group as wide as its cells, and no wider than the controls column: a big group wraps
    // its own cells onto more rows.
    const int maxColumns = std::max(1, static_cast<int>((controlsWidth - 2.0f * kBoxPad + kCellGap) / (kCellWidth + kCellGap)));
    std::vector<std::pair<float, Ptr>> boxes;
    for (auto& group : groups)
    {
        const int columns = std::min(maxColumns, static_cast<int>(group.cells.size()));
        std::vector<Ptr> rows;
        for (std::size_t i = 0; i < group.cells.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> row;
            for (std::size_t j = i; j < std::min(group.cells.size(), i + static_cast<std::size_t>(columns)); ++j)
            {
                if (!row.empty())
                    row.push_back(HGap(kCellGap));
                row.push_back(group.cells[j]);
            }
            if (!rows.empty())
                rows.push_back(VGap(6));
            rows.push_back(FixedHeight(kCellHeight, Row(row)));
        }
        const float width = 2.0f * kBoxPad + static_cast<float>(columns) * kCellWidth + static_cast<float>(columns - 1) * kCellGap;
        const float top = group.name.empty() ? 8.0f : kBoxTitle + 2.0f;
        auto box = Over(Pad(kBoxPad, top, kBoxPad, 8, Col(rows)), std::make_shared<GroupBox>(mTheme, group.name));
        boxes.emplace_back(width, FixedWidth(width, box));
    }

    std::vector<Ptr> boxRow;
    float used = 0.0f;
    auto flush = [&] {
        if (boxRow.empty())
            return;
        boxRow.push_back(Stretch());
        controls.push_back(Row(boxRow));
        controls.push_back(VGap(kBoxGap));
        boxRow.clear();
        used = 0.0f;
    };
    for (auto& [width, box] : boxes)
    {
        if (!boxRow.empty() && used + kBoxGap + width > controlsWidth)
            flush();
        if (!boxRow.empty())
        {
            boxRow.push_back(HGap(kBoxGap));
            used += kBoxGap;
        }
        boxRow.push_back(box);
        used += width;
    }
    flush();

    if (params.empty() && controls.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "This effect has no controls.", 14.0f);
        none->Colour(mTheme.TextMuted());
        controls.push_back(FixedHeight(24, none));
    }

    // ── The stage: artwork at the left, the controls scrolling at the right ──
    // Room at the right for the scroll bar.
    Ptr body = VScroll(Pad(0, 0, 14, 4, Col(controls)));
    if (showArt)
    {
        const auto image = mImages.Find(mPresentation.EquipmentImage(node->type, category));
        auto art = std::make_shared<ArtPanel>(mTheme, mIcons, image, mPresentation.IconFor(node->type, category), tint, node->enabled);
        body = Row({FixedWidth(kArtWidth, Col({art, Stretch()})), HGap(kStagePad), body});
    }

    const auto* look = mPresentation.Category(category);
    auto stage = Over(Pad(kStagePad, 10, kStagePad, kStagePad, Col({FixedHeight(54, Row(header)), VGap(6), body})),
                      std::make_shared<Stage>(mTheme, look != nullptr && !boundary ? look->visualBackground : std::nullopt));

    SyncEffectValues();
    return Fill(mTheme.Background(), Pad(16, 12, 16, 12, stage));
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
            const bool toggle = IsToggleParam(control.info);
            const bool on = toggle && value > 0.5 * (control.info.minValue + control.info.maxValue);
            std::string text = toggle ? (on ? "On" : "Off") : std::string();
            if (!toggle)
            {
                if (const int index = EnumLabelIndex(control.info, value);
                    index >= 0 && static_cast<std::size_t>(index) < control.info.labels.size())
                    text = control.info.labels[static_cast<std::size_t>(index)];
                if (WantsPicker(control.info))
                    text += " \xE2\x96\xBE";
            }
            control.toggle->Set(text).On(on).Dot(toggle);
            mSession.View().refresh(*control.toggle);
        }
    }

    if (mBypass)
    {
        mBypass->Set(node->enabled ? "On" : "Off").On(node->enabled);
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
