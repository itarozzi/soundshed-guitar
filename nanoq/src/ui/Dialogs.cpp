// What floats over the page: a pop-up list, a text prompt, a confirmation and the effect picker.
// One at a time, over a dimmed view; a tap outside, or a choice, closes it.

#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <set>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectTypeInfo;

namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool FeatureEnabled(const guitarfx::uiclient::ClientState& state, const char* feature)
{
    const auto key = std::string("features.") + feature + ".enabled";
    return state.appSettings.contains(key) && state.appSettings[key].is_boolean() && state.appSettings[key].get<bool>();
}
} // namespace

void Shell::ApplyElementsTheme()
{
    // Elements draws its own text boxes and scroll bars from one process-wide theme.
    auto t = el::get_theme();
    t.text_box_font = Font(Weight::Regular).size(metrics::kTextSize);
    t.text_box_font_color = mTheme.Text();
    t.text_box_hilite_color = mTheme.Accent().opacity(0.35f);
    t.text_box_caret_color = mTheme.Accent();
    t.inactive_font_color = mTheme.TextMuted();
    t.frame_color = mTheme.BorderStrong();
    t.frame_hilite_color = mTheme.Accent();
    t.scrollbar_color = mTheme.TextMuted().opacity(0.45f);
    t.scrollbar_width = 7.0f;
    t.panel_color = mTheme.Panel();
    el::set_theme(t);
}

void Shell::ShowOverlay(Ptr panel, float width, float height)
{
    CloseOverlay();

    auto face = Over(std::move(panel), el::share(Surface(mTheme.Elevated(), mTheme.BorderStrong(), 12.0f)));
    // Expand: a column is only as wide as its narrowest row allows, and a row of fixed-size
    // buttons allows little; the dialog's width is the one given.
    auto centred = el::share(el::align_center(el::align_middle(el::hold_any(FixedWidth(width, FixedHeight(height, Expand(face)))))));
    mOverlay = Over(centred, el::share(Backdrop([this] { CloseOverlay(); })));

    mSession.View().add(mOverlay);
}

void Shell::CloseOverlay()
{
    // A dialog with live feeds (tuner, metronome) stops listening, and tells the engine it is
    // done, whether it was closed by its button or by a tap outside it.
    mInstrumentSubs.clear();
    if (auto closed = std::move(mOnOverlayClosed))
    {
        mOnOverlayClosed = nullptr;
        closed();
    }

    if (!mOverlay)
        return;

    // Removing it from inside one of its own click handlers would pull it out from under the
    // event that is still being delivered; the view takes it out once that event is done.
    auto overlay = std::move(mOverlay);
    mOverlay.reset();
    auto& view = mSession.View();
    view.post([&view, overlay] { view.remove(overlay); });
}

namespace
{
float DialogWidth(const el::extent& view, float wanted)
{
    return std::min(wanted, std::max(240.0f, view.x - 48.0f));
}
} // namespace

void Shell::OpenMenu(const std::string& title, std::vector<MenuItem> items)
{
    auto heading = std::make_shared<Text>(mTheme, title, 15.0f, Weight::SemiBold);

    std::vector<Ptr> rows;
    for (auto& item : items)
    {
        auto choose = item.onChoose;
        auto row = std::make_shared<ListRow>(mTheme, item.label, item.detail, item.selected, item.heading || !choose,
                                             [this, choose] {
                                                 // Close first: the choice may open another dialog.
                                                 CloseOverlay();
                                                 if (choose)
                                                     choose();
                                             });
        if (!item.icon.empty())
            row->Icon(&mIcons, item.icon, item.tint != 0 ? Theme::Argb(item.tint) : mTheme.TextSecondary());
        rows.push_back(row);
    }

    const auto view = mSession.View().size();
    const float wanted = 64.0f + static_cast<float>(items.size()) * metrics::kRowHeight;
    const float height = std::clamp(wanted, 120.0f, std::max(120.0f, view.y - 80.0f));

    ShowOverlay(Pad(16, 14, 16, 12, Col({FixedHeight(30, heading), VScroll(Col(rows))})), DialogWidth(view, 420.0f), height);
}

void Shell::OpenPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       std::function<void(const std::string&)> onSubmit)
{
    auto heading = std::make_shared<Text>(mTheme, title, 15.0f, Weight::SemiBold);

    auto [boxElement, box] = el::input_box("Name", Font(Weight::Regular), 1.0f);
    box->set_text(initial);

    auto submit = [this, box, onSubmit] {
        auto text = box->get_utf8();
        // Trim: a name that is only spaces is no name.
        const auto first = text.find_first_not_of(" \t");
        const auto last = text.find_last_not_of(" \t");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        if (text.empty())
            return;
        CloseOverlay();
        onSubmit(text);
    };

    box->on_enter = [submit](std::string_view) {
        submit();
        return true;
    };
    box->on_escape = [this] { CloseOverlay(); };

    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseOverlay(); }, Button::Style::Plain);
    auto ok = std::make_shared<Button>(mTheme, confirmLabel, submit, Button::Style::Primary);
    cancel->MinWidth(88);
    ok->MinWidth(88);

    const auto view = mSession.View().size();
    ShowOverlay(Pad(16, 14, 16, 14,
                    Col({FixedHeight(30, heading), FixedHeight(44, PadXY(0, 4, el::share(std::move(boxElement)))), VGap(12),
                         FixedHeight(44, Row({Stretch(), cancel, HGap(8), ok}))})),
                DialogWidth(view, 420.0f), 178.0f);
}

void Shell::OpenConfirm(const std::string& title, const std::string& detail, const std::string& confirmLabel,
                        std::function<void()> onYes)
{
    auto heading = std::make_shared<Text>(mTheme, title, 15.0f, Weight::SemiBold);
    auto body = std::make_shared<Text>(mTheme, detail, metrics::kTextSize);
    body->Colour(mTheme.TextSecondary());

    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseOverlay(); }, Button::Style::Plain);
    auto yes = std::make_shared<Button>(mTheme, confirmLabel, [this, onYes] {
        CloseOverlay();
        onYes();
    }, Button::Style::Primary);
    cancel->MinWidth(88);
    yes->MinWidth(88);

    const auto view = mSession.View().size();
    ShowOverlay(Pad(16, 14, 16, 14,
                    Col({FixedHeight(30, heading), FixedHeight(26, body), VGap(14), FixedHeight(44, Row({Stretch(), cancel, HGap(8), yes}))})),
                DialogWidth(view, 460.0f), 170.0f);
}

void Shell::ShowToast(const std::string& title, const std::string& detail, bool isError)
{
    auto& view = mSession.View();
    if (mToast)
        view.remove(std::exchange(mToast, nullptr));

    // At the bottom, centred, above the transport bar. It takes no clicks.
    auto toast = std::make_shared<Toast>(mTheme, title, detail, isError);
    mToast = el::share(el::align_bottom(el::align_center(el::margin({0.0f, 0.0f, 0.0f, 64.0f}, el::hold_any(toast)))));
    view.add(mToast, false);

    view.post(std::chrono::seconds(isError ? 8 : 4), [&view, shown = mToast, alive = mAlive, this] {
        if (!*alive)
            return;
        view.remove(shown);
        if (mToast == shown)
            mToast = nullptr;
    });
}

// ── The effect picker ───────────────────────────────────────────────────────────

std::vector<const EffectTypeInfo*> Shell::OfferedEffects() const
{
    namespace guids = guitarfx::EffectGuids;
    const std::set<std::string> experimental{guids::kTransposeStft, guids::kTransposeHybrid};
    const bool showExperimental = FeatureEnabled(mState, "experimentalEffects");
    const bool showCustom = FeatureEnabled(mState, "customEffects");
    const bool showPickupMatch = FeatureEnabled(mState, "pickupMatch");

    std::vector<const EffectTypeInfo*> offered;
    for (const auto& info : mState.catalog)
    {
        // As core/ui/ts/fxSelector.ts's getCatalogEffects. The plugin host and composites need
        // pickers of their own, which live in Soundshed Guitar; Guitar to MIDI only plays a
        // plugin host's instrument, so it goes with it.
        if (info.type == guids::kMixer || info.type == guids::kAmpNamBlend || info.type == guids::kPluginHost ||
            info.type == guids::kGuitarToMidi || info.type.rfind("composite:", 0) == 0)
            continue;
        if (info.type == guids::kWasmHost && !showCustom)
            continue;
        // Pickup Match is experimental, behind a feature of its own.
        if (info.type == guids::kPickupMatch && !showPickupMatch)
            continue;
        if (experimental.count(info.type) > 0 && !showExperimental)
            continue;
        offered.push_back(&info);
    }
    return offered;
}

void Shell::ExpectAddedNode()
{
    // The node that appears next is the one to show.
    mSelectAddedUntil = UiSession::Now() + 5.0;
    mNodesBeforeAdd.clear();
    if (mState.activePreset)
        for (const auto& n : mState.activePreset->graph.nodes)
            mNodesBeforeAdd.insert(n.id);
}

void Shell::OpenEffectPickerOnEdge(const guitarfx::GraphEdge& edge)
{
    OpenEffectPicker(edge.from);
    mPickerEdge = edge; // after: opening the picker forgets any edge a previous one had
}

void Shell::OpenEffectPicker(const std::string& afterNodeId, const std::string& replaceNodeId)
{
    mPickerEdge.reset();
    const auto offered = OfferedEffects();

    // Replacing opens on the node's own category, with its effect marked (fxSelector.ts).
    std::string currentType;
    if (!replaceNodeId.empty() && mState.activePreset)
    {
        const auto* node = mState.activePreset->graph.FindNode(replaceNodeId);
        if (node == nullptr || guitarfx::uiclient::IsSplitterType(node->type) || guitarfx::uiclient::IsMixerType(node->type))
            return;
        currentType = node->type;
        if (const auto* type = mState.FindEffectType(node->type))
            mPickerCategory = type->category;
    }

    // Categories that have something in them, in the shared table's order.
    std::vector<std::string> categories;
    for (const auto& id : mPresentation.CategoryOrder())
        if (std::any_of(offered.begin(), offered.end(), [&id](const EffectTypeInfo* e) { return e->category == id; }))
            categories.push_back(id);
    for (const auto* effect : offered)
        if (std::find(categories.begin(), categories.end(), effect->category) == categories.end())
            categories.push_back(effect->category);

    if (categories.empty())
    {
        OpenMenu("Add effect", {{"No effects are available", false, true, {}}});
        return;
    }

    if (mPickerCategory.empty() || std::find(categories.begin(), categories.end(), mPickerCategory) == categories.end())
        mPickerCategory = categories.front();

    // A category picked, or the one before: the dialog is rebuilt around the new choice.
    auto show = [this, afterNodeId, replaceNodeId, currentType, offered, categories] {
        // The categories, wrapped onto as many rows as the dialog needs (widths estimated from
        // the text: a chip is about 7 px a letter plus its padding).
        const auto viewSize = mSession.View().size();
        const float room = DialogWidth(viewSize, 560.0f) - 32.0f;
        std::vector<Ptr> chipRows;
        std::vector<Ptr> chips;
        float used = 0.0f;
        for (const auto& id : categories)
        {
            const auto* look = mPresentation.Category(id);
            const std::string label = look != nullptr ? look->name : id;
            const float width = static_cast<float>(label.size()) * 7.0f + 2.0f * metrics::kPad + 4.0f;
            if (!chips.empty() && used + width > room)
            {
                chips.push_back(Stretch());
                chipRows.push_back(FixedHeight(40, Row(chips)));
                chips.clear();
                used = 0.0f;
            }
            used += width;
            auto chip = std::make_shared<Button>(mTheme, label,
                                                 [this, id, afterNodeId, replaceNodeId] {
                                                     mPickerCategory = id;
                                                     const auto edge = mPickerEdge;
                                                     OpenEffectPicker(afterNodeId, replaceNodeId);
                                                     mPickerEdge = edge;
                                                 },
                                                 Button::Style::Flat);
            chip->Selected(id == mPickerCategory).TextSize(metrics::kSmallText);
            chips.push_back(chip);
            chips.push_back(HGap(4));
        }
        chips.push_back(Stretch());
        chipRows.push_back(FixedHeight(40, Row(chips)));

        std::vector<Ptr> rows;
        for (const auto* effect : offered)
        {
            if (effect->category != mPickerCategory)
                continue;
            const std::string type = effect->type;
            const bool current = type == currentType;
            auto row = std::make_shared<ListRow>(mTheme, effect->name, current ? "Current" : std::string(), current, false,
                                                 [this, type, afterNodeId, replaceNodeId, current] {
                                                     CloseOverlay();
                                                     if (current)
                                                         return; // a model or settings are not reset by accident
                                                     if (!replaceNodeId.empty())
                                                     {
                                                         // The replacement is a new node; its controls come up.
                                                         ExpectAddedNode();
                                                         mSession.Commands().ReplaceNode(replaceNodeId, type);
                                                         return;
                                                     }
                                                     ExpectAddedNode();
                                                     if (mPickerEdge)
                                                         mSession.Commands().AddNodeOnEdge(type, *mPickerEdge);
                                                     else
                                                         mSession.Commands().AddNode(type, afterNodeId);
                                                 });
            row->Icon(&mIcons, mPresentation.IconFor(type, effect->category), Theme::Argb(mPresentation.CategoryColour(effect->category)));
            rows.push_back(row);
        }

        std::string title = "Add effect";
        if (!replaceNodeId.empty() && mState.activePreset)
            if (const auto* node = mState.activePreset->graph.FindNode(replaceNodeId))
                title = "Replace " + guitarfx::uiclient::NodeDisplayName(mState, *node);
        auto heading = std::make_shared<Text>(mTheme, title, 15.0f, Weight::SemiBold);
        const auto view = mSession.View().size();
        ShowOverlay(Pad(16, 14, 16, 12,
                        Col({FixedHeight(30, heading), Col(chipRows), VGap(6), VScroll(Col(rows))})),
                    DialogWidth(view, 560.0f), std::max(240.0f, view.y - 80.0f));
    };
    show();
}
} // namespace nanoq::ui
