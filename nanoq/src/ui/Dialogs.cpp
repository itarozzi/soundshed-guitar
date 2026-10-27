// What floats over the page: a pop-up list, a text prompt, a confirmation and the effect picker.
// One at a time, over a dimmed view; a tap outside, or a choice, closes it.

#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/FxMarks.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/ToneSharing.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>
#include <set>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectTypeInfo;

namespace
{
// The chooser's two lists before the categories (not categories of any effect).
const std::string kFavouritesTab = "__favourites__";
const std::string kRecentTab = "__recent__";
} // namespace

namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
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
    mOverlay = KeyLayer(Over(centred, el::share(Backdrop([this] { CloseOverlay(); }))));

    mSession.View().add(mOverlay);
}

void Shell::CloseOverlay()
{
    CloseDropdown();

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

    const auto view = ViewSize();
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

    const auto view = ViewSize();
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

    const auto view = ViewSize();
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
    using guitarfx::uiclient::Feature;
    using guitarfx::uiclient::IsFeatureEnabled;
    const bool showExperimental = IsFeatureEnabled(mState, Feature::ExperimentalEffects);
    const bool showCustom = IsFeatureEnabled(mState, Feature::CustomEffects);
    const bool showPickupMatch = IsFeatureEnabled(mState, Feature::PickupMatch);

    std::vector<const EffectTypeInfo*> offered;
    for (const auto& info : mState.catalog)
    {
        // As core/ui/ts/fxSelector.ts's getCatalogEffects. Composites need a picker of their own,
        // which lives in Soundshed Guitar. The plugin host (and Guitar to MIDI, which only plays a
        // plugin host's instrument) is offered where the engine can host plugins: the catalog
        // lists it only there.
        if (info.type == guids::kMixer || info.type == guids::kAmpNamBlend || info.type.rfind("composite:", 0) == 0)
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

    if (mPickerCategory.empty() ||
        (mPickerCategory != kFavouritesTab && mPickerCategory != kRecentTab &&
         std::find(categories.begin(), categories.end(), mPickerCategory) == categories.end()))
        mPickerCategory = categories.front();
    // The web UI's Favourites and Recent tabs come first (fxBrowser.ts).
    categories.insert(categories.begin(), {kFavouritesTab, kRecentTab});

    // The web UI's chooser (fx-selector-panel.html): a title and a search over two columns, the
    // categories with their counts at the left and the chosen one's effects at the right. Both
    // columns are slots refilled in place, so the search box keeps its focus while typing.
    struct PickerState
    {
        std::string search;
        SlotPtr left = MakeSlot();
        SlotPtr right = MakeSlot();
        std::function<void()> fill;
        std::weak_ptr<PickerState> self;
    };
    auto state = std::make_shared<PickerState>();
    auto* s = state.get();
    s->self = state;

    auto choose = [this, afterNodeId, replaceNodeId, currentType](const std::string& type) {
        CloseOverlay();
        if (type == currentType)
            return; // a model or settings are not reset by accident
        guitarfx::uiclient::RecordFxRecent(mSession.Commands(), mState, guitarfx::uiclient::FxItemKey(type));
        ExpectAddedNode();
        if (!replaceNodeId.empty())
        {
            // The replacement is a new node; its controls come up.
            mSession.Commands().ReplaceNode(replaceNodeId, type);
            return;
        }
        if (mPickerEdge)
            mSession.Commands().AddNodeOnEdge(type, *mPickerEdge);
        else
            mSession.Commands().AddNode(type, afterNodeId);
    };

    s->fill = [this, s, offered, categories, currentType, choose] {
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const auto query = lower(s->search);

        // What the Favourites and Recent tabs list: offered effects, in the lists' own order.
        const auto marked = [&offered](const std::vector<std::string>& keys) {
            std::vector<const EffectTypeInfo*> effects;
            for (const auto& key : keys)
                for (const auto* e : offered)
                    if (guitarfx::uiclient::FxItemKey(e->type) == key)
                        effects.push_back(e);
            return effects;
        };
        const auto favourites = marked(guitarfx::uiclient::FxFavoriteKeys(mState));
        const auto recents = marked(guitarfx::uiclient::FxRecentKeys(mState));

        std::vector<Ptr> left;
        for (const auto& id : categories)
        {
            const bool fav = id == kFavouritesTab, recent = id == kRecentTab;
            const auto count = fav      ? favourites.size()
                               : recent ? recents.size()
                                        : static_cast<std::size_t>(std::count_if(offered.begin(), offered.end(),
                                                                                 [&id](const EffectTypeInfo* e) { return e->category == id; }));
            const auto* look = (fav || recent) ? nullptr : mPresentation.Category(id);
            auto row = std::make_shared<CategoryRow>(mTheme, mIcons, fav ? std::string("star") : recent ? std::string("ui-clock") : look != nullptr ? look->icon : std::string("gear"),
                                                     fav ? std::string("Favourites") : recent ? std::string("Recent") : look != nullptr ? look->name : id,
                                                     static_cast<int>(count),
                                                     (fav || recent) ? mTheme.Accent() : Theme::Argb(mPresentation.CategoryColour(id)),
                                                     query.empty() && id == mPickerCategory);
            row->onClick = [this, s, id] {
                mPickerCategory = id;
                s->search.clear();
                // Refilled once this click is over: the row being clicked is in the column that
                // the refill replaces, and must outlive its own handler.
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), this, weak = s->self] {
                    const auto state = weak.lock();
                    if (alive.lock() && state && state->fill)
                        state->fill();
                });
            };
            left.push_back(row);
            left.push_back(VGap(2));
        }

        std::vector<Ptr> right;
        const auto& listed = query.empty() && mPickerCategory == kFavouritesTab ? favourites
                             : query.empty() && mPickerCategory == kRecentTab   ? recents
                                                                                 : offered;
        const bool marksTab = &listed != &offered;
        for (const auto* effect : listed)
        {
            const bool match = marksTab        ? true
                               : query.empty() ? effect->category == mPickerCategory
                                               : lower(effect->name).find(query) != std::string::npos;
            if (!match)
                continue;
            const auto* look = mPresentation.Category(effect->category);
            std::shared_ptr<cycfi::artist::image> thumb;
            if (effect->requiresResource || !effect->resourceType.empty())
                thumb = mImages.Find(mPresentation.EquipmentImage(effect->type, effect->category));
            auto row = std::make_shared<EffectRow>(mTheme, mIcons, mPresentation.IconFor(effect->type, effect->category), thumb, effect->name,
                                                   look != nullptr ? look->name : effect->category,
                                                   Theme::Argb(mPresentation.CategoryColour(effect->category)), effect->type == currentType);
            row->onClick = [choose, type = effect->type] { choose(type); };
            const auto key = guitarfx::uiclient::FxItemKey(effect->type);
            row->Star(guitarfx::uiclient::IsFxFavorite(mState, key), [this, s, key] {
                guitarfx::uiclient::ToggleFxFavorite(mSession.Commands(), mState, key);
                // Refilled once this click is over (the row is in the column being replaced).
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weak = s->self] {
                    const auto state = weak.lock();
                    if (alive.lock() && state && state->fill)
                        state->fill();
                });
            });
            right.push_back(row);
            right.push_back(VGap(4));
        }
        if (right.empty())
        {
            const bool marks = query.empty() && (mPickerCategory == kFavouritesTab || mPickerCategory == kRecentTab);
            auto none = std::make_shared<Text>(mTheme,
                                               !marks ? "No effects match."
                                               : mPickerCategory == kFavouritesTab ? "Star an effect to keep it here."
                                                                                   : "The effects you add show here.",
                                               12.0f);
            none->Colour(mTheme.TextMuted());
            right.push_back(FixedHeight(30, PadXY(8, 0, none)));
        }

        Fill(*s->left, VScroll(Col(left)));
        Fill(*s->right, VScroll(Pad(0, 0, 10, 0, Col(right))));
        mSession.View().layout();
        mSession.View().refresh();
    };
    s->fill();

    std::string title = "ADD EFFECT";
    if (!replaceNodeId.empty() && mState.activePreset)
        if (const auto* node = mState.activePreset->graph.FindNode(replaceNodeId))
            title = Upper("Replace " + guitarfx::uiclient::NodeDisplayName(mState, *node));
    auto heading = std::make_shared<Text>(mTheme, title, 12.0f, Weight::SemiBold);
    heading->Colour(mTheme.Ink());

    auto [searchElement, searchBox] = el::input_box("Search effects...", Font(Weight::Regular), 1.0f);
    searchBox->on_text = [s](std::string_view text) {
        s->search = std::string(text);
        s->fill();
    };

    const auto view = ViewSize();
    const float stageWidth = view.x - StageLeft();
    const float width = std::min(560.0f, stageWidth - 24.0f);
    const float height = std::min(438.0f, view.y - 60.0f);
    const float x = StageLeft() + (stageWidth - width) * 0.5f;
    const float titleWidth = std::min(width * 0.45f, MeasureTextWidth(title, Font(Weight::SemiBold).size(12.0f)) + 4.0f);

    auto panel = Pad(10, 8, 6, 10,
                     Col({FixedHeight(30, Row({HGap(4), FixedWidth(titleWidth, heading), HGap(14), el::share(std::move(searchElement)), HGap(4)})),
                          VGap(10), Row({FixedWidth(156, s->left), HGap(10), s->right})}));
    ShowPopover(panel, el::rect{x, 2.0f, x + width, 2.0f}, width, height);
    // The state lives as long as the chooser.
    mOnOverlayClosed = [state] { state->fill = nullptr; };
}

void Shell::OpenPresetDetails(bool asNew)
{
    // The web UI's save dialog (save-preset-modal.html, Details): name, category, description,
    // the folder it is filed in, and its tags. Saving as new names the copy; editing saves over
    // the preset playing.
    if (!mState.activePreset)
        return;
    const auto& preset = *mState.activePreset;
    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    const bool userPreset = summary != nullptr && summary->source == "user";

    struct DetailsState
    {
        std::vector<std::string> tags;
        std::string folderId; // "" for none
        Button* folderButton = nullptr;
    };
    auto st = std::make_shared<DetailsState>();
    st->tags = preset.tags;

    // Saved as new it goes in the folder being browsed; edited, it stays in its own.
    const std::function<const guitarfx::uiclient::PresetFolder*(const std::vector<guitarfx::uiclient::PresetFolder>&)> holding =
        [&](const std::vector<guitarfx::uiclient::PresetFolder>& folders) -> const guitarfx::uiclient::PresetFolder* {
        for (const auto& f : folders)
        {
            if (std::find(f.presetIds.begin(), f.presetIds.end(), mState.activePresetId) != f.presetIds.end())
                return &f;
            if (const auto* inner = holding(f.children))
                return inner;
        }
        return nullptr;
    };
    const auto* current = asNew ? guitarfx::uiclient::FindPresetFolder(mState.presetFolders, mQuery.folderId) : holding(mState.presetFolders);
    if (current != nullptr)
        st->folderId = current->id;

    const auto field = [](const char* placeholder, const std::string& text) {
        auto [element, box] = el::input_box(placeholder, Font(Weight::Regular), 1.0f);
        box->set_text(text);
        return std::make_pair(el::share(std::move(element)), box);
    };
    auto [nameElement, nameBox] = field("Name", asNew && userPreset ? preset.name + " copy" : preset.name);
    auto [categoryElement, categoryBox] = field("Category", preset.category);
    auto [descriptionElement, descriptionBox] = field("Description", preset.description);

    const auto label = [this](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 10.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        return FixedHeight(18, t);
    };

    auto folder = std::make_shared<Button>(mTheme, current != nullptr ? current->name : std::string("No folder"), [this, st] {
        if (st->folderButton == nullptr)
            return;
        auto* button = st->folderButton;
        std::vector<MenuItem> items;
        items.push_back({"No folder", st->folderId.empty(), false, [st, button] {
                             st->folderId.clear();
                             button->Set("No folder");
                         }});
        std::function<void(const std::vector<guitarfx::uiclient::PresetFolder>&, int)> add =
            [&](const std::vector<guitarfx::uiclient::PresetFolder>& folders, int depth) {
                for (const auto& f : folders)
                {
                    items.push_back({std::string(static_cast<std::size_t>(depth) * 2, ' ') + f.name, f.id == st->folderId, false,
                                     [st, button, id = f.id, name = f.name] {
                                         st->folderId = id;
                                         button->Set(name);
                                     }});
                    add(f.children, depth + 1);
                }
            };
        add(mState.presetFolders, 0);
        OpenDropdown(mFolderAnchor, std::move(items), 260.0f);
    }, Button::Style::Plain);
    folder->Flexible(true).Alignment(HAlign::Left);
    st->folderButton = folder.get();
    auto folderArea = std::make_shared<BoundsProbe>(folder, [this](el::rect bounds) { mFolderAnchor = bounds; });

    // The standard tags, and any others the preset already has.
    auto tagSlot = MakeSlot();
    auto fillTags = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakFill = fillTags;
    *fillTags = [this, st, tagSlot, weakFill] {
        auto all = guitarfx::uiclient::tones::StandardTags();
        for (const auto& t : st->tags)
            if (std::find(all.begin(), all.end(), t) == all.end())
                all.push_back(t);
        std::vector<Ptr> rows, row;
        float used = 0.0f;
        for (const auto& tag : all)
        {
            const bool on = std::find(st->tags.begin(), st->tags.end(), tag) != st->tags.end();
            auto chip = std::make_shared<Button>(mTheme, tag, [this, st, tag, on, weakFill] {
                if (on)
                    st->tags.erase(std::remove(st->tags.begin(), st->tags.end(), tag), st->tags.end());
                else
                    st->tags.push_back(tag);
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weakFill] {
                    if (const auto fill = weakFill.lock(); fill && alive.lock())
                        (*fill)();
                });
            }, Button::Style::Flat);
            chip->Selected(on).TextSize(metrics::kSmallText);
            const float w = MeasureTextWidth(tag, Font(Weight::Medium).size(metrics::kSmallText)) + 26.0f;
            if (used + w > 400.0f && !row.empty())
            {
                row.push_back(Stretch());
                rows.push_back(FixedHeight(30, Row(row)));
                row.clear();
                used = 0.0f;
            }
            row.push_back(chip);
            row.push_back(HGap(4));
            used += w + 4.0f;
        }
        if (!row.empty())
        {
            row.push_back(Stretch());
            rows.push_back(FixedHeight(30, Row(row)));
        }
        Fill(*tagSlot, Col(rows));
        mSession.View().layout();
        mSession.View().refresh();
    };
    (*fillTags)();
    mDetailsTags = fillTags; // kept while the dialog is open: the chips hold it only weakly

    auto save = [this, st, asNew, nameBox = nameBox, categoryBox = categoryBox, descriptionBox = descriptionBox] {
        const auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t");
            const auto last = text.find_last_not_of(" \t");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        guitarfx::uiclient::UiCommands::PresetDetails details;
        details.name = trim(nameBox->get_utf8());
        if (details.name.empty())
            return;
        details.category = trim(categoryBox->get_utf8());
        details.description = trim(descriptionBox->get_utf8());
        details.tags = st->tags;
        details.folderId = st->folderId;
        CloseOverlay();
        mSession.Commands().SavePresetWithDetails(details, asNew);
    };
    nameBox->on_enter = [save](std::string_view) {
        save();
        return true;
    };

    auto heading = std::make_shared<Text>(mTheme, asNew ? "Save as a new preset" : "Preset details", 15.0f, Weight::SemiBold);
    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseOverlay(); }, Button::Style::Plain);
    auto ok = std::make_shared<Button>(mTheme, "Save", save, Button::Style::Primary);
    cancel->MinWidth(88);
    ok->MinWidth(88);

    const auto box = [](Ptr element) { return FixedHeight(36, element); };
    ShowOverlay(Pad(16, 14, 16, 14,
                    Col({FixedHeight(30, heading), label("NAME"), box(nameElement), VGap(8),
                         FixedHeight(54, Row({Col({label("CATEGORY"), box(categoryElement)}), HGap(10),
                                              Col({label("FOLDER"), FixedHeight(36, folderArea)})})),
                         VGap(8), label("DESCRIPTION"), box(descriptionElement), VGap(8), label("TAGS"), tagSlot, VGap(10),
                         FixedHeight(44, Row({Stretch(), cancel, HGap(8), ok}))})),
                DialogWidth(ViewSize(), 460.0f), 470.0f);
}
} // namespace nanoq::ui
