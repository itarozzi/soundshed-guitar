// The presets page, as the web UI's preset chooser: at the left Recents, Favourites, All and the
// user's folders as a tree with their counts (a right click edits a folder); at the right a search,
// the standard tags as chips, and the presets (filtered and ordered as the web UI's library:
// uiclient::FilterPresets), each with its rating and favourite. A tap loads a preset, asking first
// when there are unsaved changes; a right click rates, files, adds to the setlist or deletes it.
// The Setlists tab chooses, steps and edits setlists, as a list or as pads. Every edit is an
// engine command (folders, setlists, ratings), answered with the new state.

#include "ui/Shell.h"
#include "uiclient/FeatureFlags.h"

#include "uiclient/PresetBrowse.h"
#include "uiclient/ToneSharing.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

void Shell::LoadWithConfirm(const std::string& presetId)
{
    if (presetId == mState.activePresetId)
    {
        ShowPage(Page::Chain);
        return;
    }

    const auto load = [this, presetId] {
        mSession.Commands().LoadPreset(presetId);
        ShowPage(Page::Chain);
    };

    if (!mState.activePresetDirty)
    {
        load();
        return;
    }

    const auto* summary = mState.FindPresetSummary(presetId);
    OpenConfirm("Unsaved changes",
                "Load " + (summary != nullptr ? summary->name : std::string("this preset")) +
                    " and lose the changes to the current one?",
                "Load", load);
}

void Shell::DeleteWithConfirm(const std::string& presetId)
{
    const auto* summary = mState.FindPresetSummary(presetId);
    OpenConfirm("Delete preset",
                "Delete " + (summary != nullptr ? summary->name : std::string("this preset")) +
                    "? It cannot be undone.",
                "Delete", [this, presetId] {
                    const bool wasActive = presetId == mState.activePresetId;
                    mSession.Commands().DeletePreset(presetId);

                    // The one playing went with it: load the first one left, as Soundshed Guitar does.
                    if (wasActive)
                    {
                        for (const auto& p : mState.presetList)
                        {
                            if (p.id != presetId)
                            {
                                mSession.Commands().LoadPreset(p.id);
                                break;
                            }
                        }
                    }
                });
}

void Shell::PostMenu(const std::string& title, std::vector<MenuItem> items)
{
    // A menu opened from another menu's item: once that menu has closed.
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), title, items = std::move(items)]() mutable {
        if (alive.lock())
        {
            OpenMenu(title, std::move(items));
        }
    });
}

void Shell::PostPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       std::function<void(const std::string&)> onSubmit)
{
    mSession.View().post(
        [this, alive = std::weak_ptr<bool>(mAlive), title, initial, confirmLabel, onSubmit = std::move(onSubmit)] {
            if (alive.lock())
            {
                OpenPrompt(title, initial, confirmLabel, onSubmit);
            }
        });
}

namespace
{
/// The tree in display order: by name at every level, ignoring case, as sortPresetFoldersAlphabetically.
void FlattenFolders(const std::vector<PresetFolder>& folders, int depth,
                    std::vector<std::pair<const PresetFolder*, int>>& out)
{
    std::vector<const PresetFolder*> sorted;

    for (const auto& f : folders)
    {
        sorted.push_back(&f);
    }

    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::stable_sort(sorted.begin(), sorted.end(), [&lower](const PresetFolder* a, const PresetFolder* b) {
        return lower(a->name) < lower(b->name);
    });

    for (const auto* f : sorted)
    {
        out.emplace_back(f, depth);
        FlattenFolders(f->children, depth + 1, out);
    }
}

/// The folder a preset is filed in, as a path ("Live / Leads"), or empty.
std::string FolderPathOf(const std::vector<PresetFolder>& folders, const std::string& presetId,
                         const std::string& prefix = {})
{
    for (const auto& f : folders)
    {
        const auto path = prefix.empty() ? f.name : prefix + " / " + f.name;

        if (std::find(f.presetIds.begin(), f.presetIds.end(), presetId) != f.presetIds.end())
        {
            return path;
        }

        if (auto inner = FolderPathOf(f.children, presetId, path); !inner.empty())
        {
            return inner;
        }
    }

    return {};
}

int RatingOf(const ClientState& state, const std::string& presetId)
{
    const auto it = state.presetRatings.find(presetId);
    return it != state.presetRatings.end() && it->is_number() ? std::clamp(it->get<int>(), 0, 5) : 0;
}

/// A bank number typed into a prompt: a whole number from 0, or -1 for none.
int TypedBank(const std::string& text)
{
    char* end = nullptr;
    const long bank = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && bank >= 0 && bank < 10000 ? static_cast<int>(bank) : -1;
}
} // namespace

void Shell::OpenPresetRowMenu(const PresetSummary& preset)
{
    const bool favourite = IsPresetFavorite(mState, preset.id);
    const std::string id = preset.id;
    const std::string name = preset.name;
    auto& commands = mSession.Commands();
    std::vector<MenuItem> items;
    items.push_back({"Load", false, false, [this, id] { LoadWithConfirm(id); }});
    items.push_back({favourite ? "Remove from favourites" : "Add to favourites", false, false,
                     [&commands, id, favourite] { commands.SetPresetFavorite(id, !favourite); }});

    const int rating = RatingOf(mState, id);
    items.push_back({"Rate...", false, false, [this, id, name, rating] {
                         std::vector<MenuItem> rates;

                         for (int stars = 5; stars >= 1; --stars)
                         {
                             rates.push_back({std::to_string(stars) + (stars == 1 ? " star" : " stars"),
                                              rating == stars, false,
                                              [this, id, stars] { mSession.Commands().SetPresetRating(id, stars); }});
                         }

                         rates.push_back({"No rating", rating == 0, false,
                                          [this, id] { mSession.Commands().SetPresetRating(id, 0); }});
                         PostMenu("Rate " + name, std::move(rates));
                     }});

    items.push_back({"Move to folder...", false, false, [this, id, name] {
                         std::vector<std::pair<const PresetFolder*, int>> flat;
                         FlattenFolders(mState.presetFolders, 0, flat);
                         std::vector<MenuItem> rows;
                         rows.push_back({"No folder", false, false,
                                         [this, id] { mSession.Commands().MovePresetToFolder(id, kPresetFolderAll); }});

                         for (const auto& [f, depth] : flat)
                         {
                             const bool here =
                                 std::find(f->presetIds.begin(), f->presetIds.end(), id) != f->presetIds.end();
                             rows.push_back({std::string(static_cast<std::size_t>(depth) * 2, ' ') + f->name, here,
                                             false, [this, id, folderId = f->id] {
                                                 mSession.Commands().MovePresetToFolder(id, folderId);
                                             }});
                         }

                         PostMenu("Move " + name, std::move(rows));
                     }});

    for (const auto& s : mState.setlists)
    {
        if (s.id == mState.activeSetlistId)
        {
            items.push_back({"Add to " + s.name, false, false, [&commands, id] { commands.AddSetlistSlot({}, id); }});
        }
    }

    // Multi-Rig: play it alongside the presets already in the mix (the web UI's + Mixer).
    const auto& inMix = mState.mixer.activePresetIds;

    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::MultiRig) &&
        std::find(inMix.begin(), inMix.end(), id) == inMix.end())
    {
        items.push_back({"Add to the mix", false, false, [&commands, id] { commands.AddActivePreset(id); }});
    }

    if (preset.source == "user")
    {
        items.push_back({"Delete...", false, false, [this, id] { DeleteWithConfirm(id); }});
    }

    OpenMenu(preset.name, std::move(items));
}

void Shell::OpenFolderMenu(const std::string& folderId)
{
    const auto* folder = FindPresetFolder(mState.presetFolders, folderId);

    if (folder == nullptr)
    {
        return;
    }

    const std::string name = folder->name;
    std::vector<MenuItem> items;
    items.push_back({"New folder inside...", false, false, [this, folderId] {
                         PostPrompt("New folder", "", "Create", [this, folderId](const std::string& entered) {
                             mSession.Commands().CreatePresetFolder(entered, folderId);
                         });
                     }});
    items.push_back({"Rename...", false, false, [this, folderId, name] {
                         PostPrompt("Rename folder", name, "Rename", [this, folderId](const std::string& entered) {
                             mSession.Commands().RenamePresetFolder(folderId, entered);
                         });
                     }});
    items.push_back(
        {"Move into...", false, false, [this, folderId, name] {
             std::vector<std::pair<const PresetFolder*, int>> flat;
             FlattenFolders(mState.presetFolders, 0, flat);
             std::vector<MenuItem> rows;
             rows.push_back({"The top level", false, false,
                             [this, folderId] { mSession.Commands().MovePresetFolder(folderId, {}); }});
             // Not into itself or a folder inside it: the engine refuses those anyway.
             const auto* self = FindPresetFolder(mState.presetFolders, folderId);

             for (const auto& [f, depth] : flat)
             {
                 if (f->id == folderId || (self != nullptr && FindPresetFolder(self->children, f->id) != nullptr))
                 {
                     continue;
                 }

                 rows.push_back(
                     {std::string(static_cast<std::size_t>(depth) * 2, ' ') + f->name, false, false,
                      [this, folderId, target = f->id] { mSession.Commands().MovePresetFolder(folderId, target); }});
             }

             PostMenu("Move " + name, std::move(rows));
         }});
    items.push_back({"Delete...", false, false, [this, folderId, name] {
                         mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), folderId, name] {
                             if (alive.lock())
                             {
                                 OpenConfirm("Delete folder",
                                             "Delete " + name +
                                                 " and the folders in it? Its presets stay in the library.",
                                             "Delete", [this, folderId] {
                                                 mQuery.folderId = kPresetFolderAll;
                                                 mSession.Commands().DeletePresetFolder(folderId);
                                             });
                             }
                         });
                     }});
    OpenMenu(name, std::move(items));
}

void Shell::SelectPresetBrowserFolder(const std::string& folderId)
{
    mQuery.folderId = folderId;
    mShowMultiRigs = false;
    mSession.Commands().SelectPresetFolder(folderId);
    Mark(kPage);
}

Ptr Shell::BuildFolderColumn()
{
    // The virtual folders first, then the user's as a tree, each with its count.
    const auto count = [this](const std::string& folderId) {
        PresetQuery q;
        q.folderId = folderId;
        return std::to_string(FilterPresets(mState, q).size());
    };
    std::vector<Ptr> rows;
    const auto add = [&](const std::string& id, const std::string& label, const char* icon, int depth, bool editable) {
        const bool chosen = mQuery.folderId == id && !mShowMultiRigs;
        auto row = std::make_shared<ListRow>(mTheme, std::string(static_cast<std::size_t>(depth) * 3, ' ') + label,
                                             count(id), chosen, false, [this, id] { SelectPresetBrowserFolder(id); });
        row->Icon(&mIcons, icon, chosen ? mTheme.Accent() : mTheme.TextSecondary());

        if (editable)
        {
            row->onMenu = [this, id] { OpenFolderMenu(id); };
        }

        rows.push_back(FixedHeight(36, row));
    };
    add(kPresetFolderRecents, "Recents", "ui-clock", 0, false);
    add(kPresetFolderFavorites, "Favourites", "heart", 0, false);
    add(kPresetFolderAll, "All Presets", "ui-presets", 0, false);

    // Saved Multi-Rigs, while the feature is on (the web UI's Multi-Rig tab).
    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::MultiRig))
    {
        auto row = std::make_shared<ListRow>(mTheme, "Multi-Rigs", std::to_string(mState.mixer.compositePresets.size()),
                                             mShowMultiRigs, false, [this] {
                                                 mShowMultiRigs = true;
                                                 mSession.Commands().GetCompositePresetList();
                                                 Mark(kPage);
                                             });
        row->Icon(&mIcons, "ui-pads", mShowMultiRigs ? mTheme.Accent() : mTheme.TextSecondary());
        rows.push_back(FixedHeight(36, row));
    }

    std::vector<std::pair<const PresetFolder*, int>> flat;
    FlattenFolders(mState.presetFolders, 0, flat);

    if (!flat.empty())
    {
        auto heading = std::make_shared<Text>(mTheme, "FOLDERS", 9.5f, Weight::SemiBold);
        heading->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(26, PadXY(12, 0, heading)));
    }

    for (const auto& [f, depth] : flat)
    {
        add(f->id, f->name, "ui-folder", depth, true);
    }

    auto newFolder = std::make_shared<Button>(
        mTheme, "+ New folder",
        [this] {
            OpenPrompt("New folder", "", "Create",
                       [this](const std::string& entered) { mSession.Commands().CreatePresetFolder(entered); });
        },
        Button::Style::Flat);
    newFolder->TextSize(metrics::kSmallText);
    rows.push_back(VGap(6));
    rows.push_back(FixedHeight(32, Row({newFolder, Stretch()})));
    return VScroll(Col(rows));
}

Ptr Shell::BuildPresetListContent()
{
    if (mShowMultiRigs)
    {
        return BuildMultiRigList();
    }

    mQuery.tags = mPresetTags;
    const auto presets = FilterPresets(mState, mQuery);
    std::vector<Ptr> rows;

    for (const auto& p : presets)
    {
        // Under the name: its category, the folder it is in (outside that folder), its tags.
        std::string detail = p.category;

        if (FindPresetFolder(mState.presetFolders, mQuery.folderId) == nullptr)
        {
            if (auto path = FolderPathOf(mState.presetFolders, p.id); !path.empty())
            {
                detail += "  \xC2\xB7  " + path;
            }
        }

        for (const auto& tag : p.tags)
        {
            detail += "  #" + tag;
        }

        auto row = std::make_shared<PresetRow>(mTheme, mIcons, p.name, detail, RatingOf(mState, p.id),
                                               IsPresetFavorite(mState, p.id), p.id == mState.activePresetId);
        row->onClick = [this, id = p.id] { LoadWithConfirm(id); };
        row->onMenu = [this, p] { OpenPresetRowMenu(p); };
        row->onFavourite = [this, id = p.id] {
            mSession.Commands().SetPresetFavorite(id, !IsPresetFavorite(mState, id));
        };
        row->onRate = [this, id = p.id](int rating) { mSession.Commands().SetPresetRating(id, rating); };
        rows.push_back(row);
    }

    if (rows.empty())
    {
        auto none = std::make_shared<Text>(
            mTheme, mQuery.text.empty() && mPresetTags.empty() ? "Nothing here yet." : "No presets match.", 14.0f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(32, PadXY(12, 0, none)));
    }

    if (mPresetCount)
    {
        mPresetCount->Set(std::to_string(presets.size()) + (presets.size() == 1 ? " preset" : " presets"));
    }

    return VScroll(Col(rows));
}

void Shell::RebuildPresetList()
{
    // Typing in the search box: only the list changes, so the box keeps its focus.
    if (!mPresetListSlot)
    {
        return;
    }

    Fill(*mPresetListSlot, BuildPresetListContent());
    mSession.View().layout();
    mSession.View().refresh();
}

Ptr Shell::BuildSetlistsView(Ptr tabs)
{
    auto& commands = mSession.Commands();

    const Setlist* active = nullptr;

    for (const auto& s : mState.setlists)
    {
        if (s.id == mState.activeSetlistId)
        {
            active = &s;
        }
    }

    if (active == nullptr && !mState.setlists.empty())
    {
        active = &mState.setlists.front();
    }

    auto picker = std::make_shared<Button>(
        mTheme, active != nullptr ? active->name : std::string("No setlists"),
        [this] {
            std::vector<MenuItem> items;

            for (const auto& s : mState.setlists)
            {
                items.push_back({s.name + (s.bank ? "  (bank " + std::to_string(*s.bank) + ")" : std::string()),
                                 s.id == mState.activeSetlistId, false,
                                 [this, id = s.id] { mSession.Commands().SelectSetlist(id); }});
            }

            if (items.empty())
            {
                items.push_back({"No setlists yet", false, true, {}});
            }

            OpenMenu("Setlist", std::move(items));
        },
        Button::Style::Plain);
    picker->MinWidth(200);

    const int cursor = mState.setlistCursor;
    const int count = active != nullptr ? static_cast<int>(active->presetIds.size()) : 0;
    auto prev = std::make_shared<Button>(
        mTheme, "\xE2\x80\xB9",
        [&commands, cursor, count] {
            if (count > 0)
            {
                commands.SetSetlistCursor((cursor - 1 + count) % count);
            }
        },
        Button::Style::Plain);
    auto next = std::make_shared<Button>(
        mTheme, "\xE2\x80\xBA",
        [&commands, cursor, count] {
            if (count > 0)
            {
                commands.SetSetlistCursor((cursor + 1) % count);
            }
        },
        Button::Style::Plain);
    prev->MinWidth(44).TextSize(20);
    next->MinWidth(44).TextSize(20);

    // Making, editing and deleting setlists, as the web UI's setlist panel and pad editor do.
    auto create = std::make_shared<Button>(
        mTheme, "New",
        [this] {
            OpenPrompt("New setlist", "", "Next", [this](const std::string& name) {
                PostPrompt("Bank number (blank for none)", "", "Create", [this, name](const std::string& bank) {
                    mSession.Commands().CreateSetlist(name, TypedBank(bank));
                });
            });
        },
        Button::Style::Plain);
    std::vector<Ptr> toolbar{picker, HGap(6), prev, HGap(4), next, HGap(12), create};

    if (active != nullptr)
    {
        const std::string id = active->id;
        const std::string name = active->name;
        const std::string bank = active->bank ? std::to_string(*active->bank) : std::string();
        auto edit = std::make_shared<Button>(
            mTheme, "Edit",
            [this, id, name, bank] {
                OpenPrompt("Setlist name", name, "Next", [this, id, bank](const std::string& entered) {
                    PostPrompt("Bank number (blank for none)", bank, "Save", [this, id, entered](const std::string& b) {
                        mSession.Commands().UpdateSetlist(id, entered, TypedBank(b));
                    });
                });
            },
            Button::Style::Plain);
        auto remove = std::make_shared<Button>(
            mTheme, "Delete",
            [this, id, name] {
                OpenConfirm("Delete setlist", "Delete " + name + "? Its presets stay in the library.", "Delete",
                            [this, id] { mSession.Commands().DeleteSetlist(id); });
            },
            Button::Style::Plain);
        auto addCurrent = std::make_shared<Button>(
            mTheme, "Add current preset",
            [this, id] {
                if (!mState.activePresetId.empty())
                {
                    mSession.Commands().AddSetlistSlot(id, mState.activePresetId);
                }
            },
            Button::Style::Plain);
        addCurrent->Enabled(!mState.activePresetId.empty());
        toolbar.insert(toolbar.end(), {HGap(4), edit, HGap(4), remove, HGap(12), addCurrent});
    }

    auto view = std::make_shared<Button>(
        mTheme, mSetlistPads ? "List" : "Pads",
        [this] {
            mSetlistPads = !mSetlistPads;
            Mark(kPage);
        },
        Button::Style::Flat);
    toolbar.push_back(HGap(12));
    toolbar.push_back(view);

    Ptr body;

    if (active != nullptr && mSetlistPads)
    {
        // The pads: eight to a bank, a tap plays the slot, a right click puts the preset playing
        // there or clears it (the web UI's performance pads).
        constexpr int kPerBank = 8;
        const int banks = std::max(1, (count + kPerBank - 1) / kPerBank);
        mSetlistPadBank = std::clamp(mSetlistPadBank, 0, banks);
        const std::string id = active->id;
        std::vector<Ptr> grid;

        for (int row = 0; row < 2; ++row)
        {
            std::vector<Ptr> cells;

            for (int col = 0; col < 4; ++col)
            {
                const int slot = mSetlistPadBank * kPerBank + row * 4 + col;
                const std::string presetId =
                    slot < count ? active->presetIds[static_cast<std::size_t>(slot)] : std::string();
                const auto* summary = presetId.empty() ? nullptr : mState.FindPresetSummary(presetId);
                const std::string label = std::to_string(slot + 1) + "  " +
                                          (summary != nullptr ? summary->name
                                           : presetId.empty() ? std::string("Empty")
                                                              : presetId);
                auto pad = std::make_shared<Button>(
                    mTheme, label,
                    [&commands, slot, presetId] {
                        if (!presetId.empty())
                        {
                            commands.SetSetlistCursor(slot);
                        }
                    },
                    slot == cursor && !presetId.empty() ? Button::Style::Primary : Button::Style::Plain);
                pad->Flexible(true);
                std::vector<MenuItem> padMenu;

                if (!mState.activePresetId.empty())
                {
                    padMenu.push_back({"Put the current preset here", false, false, [this, id, slot] {
                                           mSession.Commands().SetSetlistSlot(id, slot, mState.activePresetId);
                                       }});
                }

                if (!presetId.empty())
                {
                    padMenu.push_back({"Clear this pad", false, false,
                                       [this, id, slot] { mSession.Commands().SetSetlistSlot(id, slot, {}); }});
                }

                auto withMenu = std::make_shared<MenuArea>(
                    pad, [this, slot, padMenu] { OpenMenu("Pad " + std::to_string(slot + 1), padMenu); });

                if (col > 0)
                {
                    cells.push_back(HGap(8));
                }

                cells.push_back(withMenu);
            }

            if (row > 0)
            {
                grid.push_back(VGap(8));
            }

            grid.push_back(FixedHeight(84, Row(cells)));
        }

        auto bankPrev = std::make_shared<Button>(
            mTheme, "\xE2\x80\xB9 Bank",
            [this] {
                --mSetlistPadBank;
                Mark(kPage);
            },
            Button::Style::Flat);
        auto bankNext = std::make_shared<Button>(
            mTheme, "Bank \xE2\x80\xBA",
            [this] {
                ++mSetlistPadBank;
                Mark(kPage);
            },
            Button::Style::Flat);
        bankPrev->Enabled(mSetlistPadBank > 0);
        bankNext->Enabled(mSetlistPadBank < banks);
        auto bankText = std::make_shared<Text>(mTheme,
                                               "Pads " + std::to_string(mSetlistPadBank * kPerBank + 1) + " to " +
                                                   std::to_string(mSetlistPadBank * kPerBank + kPerBank),
                                               metrics::kSmallText);
        bankText->Colour(mTheme.TextMuted());
        body = Col({FixedHeight(36, Row({bankPrev, HGap(8), bankText, HGap(8), bankNext, Stretch()})), VGap(8),
                    Col(grid), std::make_shared<el::element>()});
    }
    else
    {
        std::vector<Ptr> rows;

        if (active != nullptr)
        {
            const std::string id = active->id;

            for (int i = 0; i < count; ++i)
            {
                const auto& presetId = active->presetIds[static_cast<std::size_t>(i)];
                const auto* summary = mState.FindPresetSummary(presetId);
                auto row = std::make_shared<ListRow>(
                    mTheme, std::to_string(i + 1) + ".  " + (summary != nullptr ? summary->name : presetId),
                    summary != nullptr ? summary->category : std::string(), i == cursor, false,
                    [&commands, i] { commands.SetSetlistCursor(i); });
                row->onMenu = [this, id, i, count] {
                    std::vector<MenuItem> items;

                    if (i > 0)
                    {
                        items.push_back({"Move up", false, false,
                                         [this, id, i] { mSession.Commands().MoveSetlistSlot(id, i, i - 1); }});
                    }

                    if (i + 1 < count)
                    {
                        items.push_back({"Move down", false, false,
                                         [this, id, i] { mSession.Commands().MoveSetlistSlot(id, i, i + 1); }});
                    }

                    if (!mState.activePresetId.empty())
                    {
                        items.push_back({"Put the current preset here", false, false, [this, id, i] {
                                             mSession.Commands().SetSetlistSlot(id, i, mState.activePresetId);
                                         }});
                    }

                    items.push_back({"Remove from the setlist", false, false,
                                     [this, id, i] { mSession.Commands().RemoveSetlistSlot(id, i); }});
                    OpenMenu("Slot " + std::to_string(i + 1), std::move(items));
                };
                rows.push_back(row);
            }
        }

        if (rows.empty())
        {
            auto none = std::make_shared<Text>(
                mTheme,
                active != nullptr ? "This setlist is empty: add the preset playing, or add one from a preset's menu."
                                  : "No setlists yet: New makes one.",
                14.0f);
            none->Colour(mTheme.TextMuted());
            rows.push_back(FixedHeight(32, PadXY(12, 0, none)));
        }

        body = VScroll(Col(rows));
    }

    return Fill(mTheme.Background(), Pad(16, 8, 16, 8, Col({tabs, FixedHeight(48, PadXY(0, 4, Row(toolbar))), body})));
}

Ptr Shell::BuildPresetsPage()
{
    auto tab = [this](const char* label, bool setlists) {
        auto b = std::make_shared<Button>(
            mTheme, label,
            [this, setlists] {
                mShowSetlists = setlists;
                Mark(kPage);
            },
            Button::Style::Flat);
        b->Selected(mShowSetlists == setlists).MinWidth(100);
        return b;
    };
    Ptr tabs = FixedHeight(44, Row({tab("Presets", false), HGap(4), tab("Setlists", true), Stretch()}));

    if (mShowSetlists)
    {
        return BuildSetlistsView(tabs);
    }

    // ── The library ──
    if (!mSearchBox)
    {
        // Made once and kept: the page is rebuilt when the library changes, and a search box
        // made afresh each time would lose what is typed in it, and its focus.
        auto [element, box] = el::input_box("Search presets", Font(Weight::Regular), 1.0f);
        mSearchBox = box;
        mSearchElement = el::share(std::move(element));
        mSearchBox->on_text = [this](std::string_view text) {
            mQuery.text = std::string(text);
            RebuildPresetList();
        };
    }

    // The folder the engine shows: what the page opens on, and a folder just made.
    if (mState.activePresetFolderId != mSeenPresetFolder)
    {
        mSeenPresetFolder = mState.activePresetFolderId;
        mQuery.folderId = mSeenPresetFolder.empty() ? std::string(kPresetFolderAll) : mSeenPresetFolder;
    }

    if (mQuery.folderId != kPresetFolderAll && mQuery.folderId != kPresetFolderFavorites &&
        mQuery.folderId != kPresetFolderRecents && FindPresetFolder(mState.presetFolders, mQuery.folderId) == nullptr)
    {
        mQuery.folderId = kPresetFolderAll;
    }

    // The standard tags (presetTags.ts) as chips: each one on narrows the list to presets that carry it.
    std::vector<Ptr> chips;

    for (const auto& tag : tones::StandardTags())
    {
        const bool on = std::find(mPresetTags.begin(), mPresetTags.end(), tag) != mPresetTags.end();
        auto chip = std::make_shared<Button>(
            mTheme, tag,
            [this, tag, on] {
                if (on)
                {
                    mPresetTags.erase(std::remove(mPresetTags.begin(), mPresetTags.end(), tag), mPresetTags.end());
                }
                else
                {
                    mPresetTags.push_back(tag);
                }

                Mark(kPage);
            },
            Button::Style::Flat);
        chip->Selected(on).TextSize(metrics::kSmallText);

        if (!chips.empty())
        {
            chips.push_back(HGap(4));
        }

        chips.push_back(chip);
    }

    mPresetCount = std::make_shared<Text>(mTheme, "", metrics::kSmallText);
    mPresetCount->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);

    auto newPreset =
        std::make_shared<Button>(mTheme, "New", [this] { mSession.Commands().NewPreset(); }, Button::Style::Plain);
    newPreset->MinWidth(72);

    mPresetListSlot = MakeSlot();
    Fill(*mPresetListSlot, BuildPresetListContent());

    auto right =
        Col({FixedHeight(
                 44, PadXY(0, 4, Row({mSearchElement, HGap(8), FixedWidth(100, mPresetCount), HGap(8), newPreset}))),
             FixedHeight(36, HScroll(Row(chips))), VGap(4), mPresetListSlot});
    auto page = Col({tabs, Row({FixedWidth(230, BuildFolderColumn()), HGap(14), right})});
    return Fill(mTheme.Background(), Pad(16, 8, 16, 8, page));
}
} // namespace nanoq::ui
