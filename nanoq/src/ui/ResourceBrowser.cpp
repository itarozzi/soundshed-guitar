// The model and IR chooser, as the web UI's resource browser (ui-components/modals/
// resource-browser-modal.html, ts/resourceBrowser/): a dialog with three sources for a node's
// slot - the Resource Library, a Folder on disk, and Tone 3000 through Soundshed's relay. A click
// loads the choice into the node at once, so it can be heard; OK keeps it, Cancel puts back what
// the slot held.
//
// - Library: the library's entries of the slot's type, by category, with a search and a
//   category filter; File... asks the engine for a file dialog.
// - Folder: the folders the user added (the web UI's setting, so both products share them),
//   listed by the engine (listResourceFolder). A file is heard where it is; OK adds it to the
//   library in place (saveLocalLibraryResource), or picks the library entry it already is.
// - Tone 3000: a search by gear, sort and architecture; a tone opens to its models; Select
//   downloads one, stores it in the library and loads it (ToneService::ImportTone3000).

#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cctype>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;
namespace t3 = guitarfx::uiclient::tone3000;

namespace
{
constexpr const char* kRootsSetting = "resources.folderBrowser.roots";
constexpr const char* kActiveRootSetting = "resources.folderBrowser.activeRootId";
constexpr const char* kLastLocationsSetting = "resources.folderBrowser.lastLocationByContext";

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string MetaText(const nlohmann::json& metadata, const char* key)
{
    const auto it = metadata.find(key);
    if (it == metadata.end())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number())
        return it->dump();
    return {};
}

/// "by creator · A2 · gear", the line under an entry's name.
std::string Describe(const nlohmann::json& metadata, const std::string& category)
{
    std::vector<std::string> parts;
    auto creator = MetaText(metadata, "creatorName");
    if (creator.empty())
        creator = MetaText(metadata, "authorUsername");
    if (creator.empty())
        creator = MetaText(metadata, "modeled_by");
    if (!creator.empty())
        parts.push_back("by " + creator);
    if (auto arch = MetaText(metadata, "architectureVersion"); !arch.empty() && arch != "custom")
        parts.push_back(arch.front() == 'A' ? arch : "A" + arch);
    if (const auto rate = MetaText(metadata, "sample_rate"); !rate.empty())
        parts.push_back(rate + " Hz");
    if (!category.empty())
        parts.push_back(category);
    std::string text;
    for (const auto& p : parts)
        text += (text.empty() ? "" : " \xC2\xB7 ") + p;
    return text;
}

/// One row of a list: a name with a line under it, an optional icon or picture at the left, and
/// at the right a note or a button. A click calls `onClick`; a double click `onDouble`.
class BrowserRow final : public el::element
{
public:
    BrowserRow(const Theme& theme, Icons& icons, std::string title, std::string detail)
        : mTheme(theme), mIcons(icons), mTitle(std::move(title)), mDetail(std::move(detail))
    {
    }

    std::string icon;                        // ui-folder, a chevron...
    std::function<std::shared_ptr<artist::image>()> picture; // a tone's picture, read as it draws (it may arrive later)
    std::string note;                        // muted text at the right ("In library")
    el::color noteColour;
    std::string button;                      // a button at the right ("Select")
    bool selected = false;
    bool heading = false;                    // a group's caption
    bool indent = false;                     // a model under its tone
    std::function<void()> onClick, onDouble, onButton;

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = heading ? 30.0f : (mDetail.empty() ? 36.0f : 44.0f);
        return {{100.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        auto b = ctx.bounds;
        if (heading)
        {
            cnv.fill_style(el::rgba(255, 255, 255, mHover ? 14 : 8));
            cnv.fill_round_rect(b.inset(0.0f, 2.0f), 6.0f);
            mIcons.Draw(cnv, icon.empty() ? "ui-chevron-right" : icon, el::rect{b.left + 10.0f, b.top + 9.0f, b.left + 22.0f, b.top + 21.0f},
                        mTheme.TextSecondary());
            FillText(cnv, mTitle, Font(Weight::SemiBold).size(12.5f), mTheme.TextOnDark(), el::rect{b.left + 30.0f, b.top, b.right - 60.0f, b.bottom},
                     HAlign::Left);
            if (!note.empty())
            {
                const auto font = Font(Weight::SemiBold).size(9.5f);
                const float w = el::measure_text(cnv, note, font).x + 14.0f;
                const el::rect pill{b.right - 12.0f - w, b.top + 7.0f, b.right - 12.0f, b.bottom - 7.0f};
                cnv.fill_style(el::rgba(255, 255, 255, 22));
                cnv.fill_round_rect(pill, 8.0f);
                FillText(cnv, note, font, mTheme.TextSecondary(), pill, HAlign::Centre);
            }
            return;
        }
        if (indent)
            b.left += 16.0f;
        const auto face = b.inset(0.0f, 2.0f);
        cnv.fill_style(selected ? mTheme.Accent().opacity(0.22f).level(0.8f) : el::rgba(255, 255, 255, mHover ? 14 : 7));
        cnv.fill_round_rect(face, 6.0f);
        if (selected)
        {
            cnv.stroke_style(mTheme.Accent().opacity(0.45f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(face.inset(0.5f, 0.5f), 6.0f);
        }

        float x = face.left + 12.0f;
        if (const auto image = picture ? picture() : nullptr)
        {
            const el::rect r{x - 2.0f, face.top + 4.0f, x + face.height() - 10.0f, face.bottom - 4.0f};
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(r, 4.0f);
            cnv.clip();
            DrawImageCovering(cnv, *image, r);
            x = r.right + 10.0f;
        }
        else if (!icon.empty())
        {
            const float cy = face.top + face.height() * 0.5f;
            mIcons.Draw(cnv, icon, el::rect{x, cy - 8.0f, x + 16.0f, cy + 8.0f}, mTheme.TextSecondary());
            x += 26.0f;
        }

        float right = face.right - 12.0f;
        if (!button.empty())
        {
            const auto font = Font(Weight::SemiBold).size(11.0f);
            const float w = el::measure_text(cnv, button, font).x + 22.0f;
            mButton = el::rect{right - w, face.top + (face.height() - 26.0f) * 0.5f, right, face.top + (face.height() + 26.0f) * 0.5f};
            cnv.fill_style(mHoverButton ? el::rgba(72, 72, 86, 255) : el::rgba(48, 48, 58, 255));
            cnv.fill_round_rect(mButton, 6.0f);
            cnv.stroke_style(el::rgba(255, 255, 255, 30));
            cnv.stroke_round_rect(mButton.inset(0.5f, 0.5f), 6.0f);
            FillText(cnv, button, font, mTheme.TextOnDark(), mButton, HAlign::Centre);
            right = mButton.left - 10.0f;
        }
        else
        {
            mButton = {};
        }
        if (!note.empty())
        {
            const auto font = Font(Weight::SemiBold).size(10.5f);
            const float w = el::measure_text(cnv, note, font).x;
            FillText(cnv, note, font, noteColour.alpha > 0.0f ? noteColour : mTheme.TextMuted(),
                     el::rect{right - w, face.top, right, face.bottom}, HAlign::Right);
            right -= w + 10.0f;
        }

        const auto titleFont = Font(Weight::SemiBold).size(12.0f);
        const auto detailFont = Font(Weight::Regular).size(10.5f);
        if (mDetail.empty())
        {
            FillText(cnv, Ellipsize(cnv, mTitle, titleFont, right - x), titleFont, mTheme.TextOnDark(), el::rect{x, face.top, right, face.bottom},
                     HAlign::Left);
        }
        else
        {
            FillText(cnv, Ellipsize(cnv, mTitle, titleFont, right - x), titleFont, mTheme.TextOnDark(),
                     el::rect{x, face.top + 5.0f, right, face.top + 21.0f}, HAlign::Left);
            FillText(cnv, Ellipsize(cnv, mDetail, detailFont, right - x), detailFont, mTheme.TextMuted(),
                     el::rect{x, face.top + 21.0f, right, face.bottom - 4.0f}, HAlign::Left);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left || btn.down)
            return btn.state == el::mouse_button::left;
        if (!button.empty() && mButton.includes(btn.pos))
        {
            if (onButton)
                onButton();
            return true;
        }
        if (btn.num_clicks >= 2 && onDouble)
            onDouble();
        else if (btn.num_clicks < 2 && onClick)
            onClick();
        (void)ctx;
        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const bool overButton = inside && !button.empty() && mButton.includes(p);
        if (inside != mHover || overButton != mHoverButton)
        {
            mHover = inside;
            mHoverButton = overButton;
            ctx.view.refresh(ctx);
        }
        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mTitle, mDetail;
    bool mHover = false, mHoverButton = false;
    el::rect mButton;
};

/// The tab strip's tabs (.resource-browser-tab-btn): 13 px, the open one in the accent and underlined.
class BrowserTab final : public Pressable
{
public:
    BrowserTab(const Theme& theme, std::string label, bool active) : mTheme(theme), mLabel(std::move(label)), mActive(active) {}

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(13.0f)).x + 36.0f;
        return {{w, 38.0f}, {w, 38.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(13.0f), mActive ? mTheme.Accent() : (mHover ? mTheme.Text() : mTheme.TextSecondary()),
                 el::rect{b.left, b.top, b.right, b.bottom - 2.0f}, HAlign::Centre);
        if (mActive)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{b.left, b.bottom - 2.0f, b.right, b.bottom});
        }
    }

private:
    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
};

/// Which kind of slot the browser serves: its title, its Tone3000 gear and its folder context.
struct SlotRole
{
    std::string title;
    std::string context;
    std::string gear;
    std::vector<std::pair<std::string, std::string>> gears; // value, label
};

SlotRole RoleFor(const ClientState& state, const guitarfx::GraphNode& node, const std::string& resourceType, int resourceIndex)
{
    namespace guids = guitarfx::EffectGuids;
    SlotRole role;
    if (resourceType == "ir")
    {
        const bool reverb = node.type == guids::kReverbIr || NodeCategory(state, node) == "reverb";
        role.title = reverb ? "Select Reverb IR" : "Select IR Cabinet";
        role.context = reverb ? "ir-reverb" : (resourceIndex > 0 ? "ir-cab#" + std::to_string(resourceIndex) : "ir-cab");
        role.gear = reverb ? "space" : "cab";
        role.gears = {{"cab", "Cabinets"}, {"space", "Rooms"}, {"ir", "All IRs"}};
        return role;
    }
    if (node.type == guids::kFxNam)
    {
        role.title = "Select Pedal Model";
        role.context = "nam-fx";
        role.gear = "pedal";
    }
    else
    {
        // An amp with a cab IR after it wants an amp alone; otherwise a full rig.
        bool cabInPath = false;
        if (state.activePreset)
            for (const auto& n : state.activePreset->graph.nodes)
                cabInPath = cabInPath || n.type == guids::kCabIr;
        role.title = "Select Amp Model";
        role.context = "nam-amp";
        role.gear = cabInPath ? "amp" : "full-rig";
    }
    role.gears = {{"full-rig", "Full Rigs"}, {"amp", "Amps"}, {"pedal", "Pedals"}, {"outboard", "Preamps"}, {"", "Any gear"}};
    return role;
}
} // namespace

void Shell::OpenResourceBrowser(const std::string& nodeId, const std::string& resourceType, int resourceIndex)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
    if (node == nullptr)
        return;

    enum class Tab
    {
        Library,
        Folder,
        Tone3000
    };

    struct Browser
    {
        std::string nodeId, resourceType;
        int index = -1;
        SlotRole role;
        guitarfx::ResourceRef original;
        bool hadOriginal = false;
        bool previewed = false;
        std::string selectedId, selectedPath; // the choice being heard
        Tab tab = Tab::Library;

        std::string query, category;   // Library
        std::string filter;            // Folder
        std::string rootId;
        std::string t3Query, t3Gear, t3Sort = "downloads-all-time", t3Arch;
        int t3Page = 1, t3Pages = 1;
        bool t3Loading = false;
        std::string t3Error;
        std::vector<t3::Tone> t3Tones;
        std::string t3Open; // the tone showing its models
        std::vector<t3::Model> t3Models;
        bool t3ModelsLoading = false;

        SlotPtr tabs = MakeSlot(), controls = MakeSlot(), list = MakeSlot(), footer = MakeSlot();
        std::function<void()> fillTabs, fillControls, fillList, fillFooter;
        std::function<void(int)> searchTone3000;
        std::shared_ptr<el::basic_input_box> searchBox, filterBox, t3Box;
        Ptr searchElement, filterElement, t3Element;
        std::vector<Subscription> subscriptions;
        int toneListener = 0;
        std::weak_ptr<Browser> self;
    };

    auto state = std::make_shared<Browser>();
    auto* b = state.get();
    b->self = state;
    b->nodeId = nodeId;
    b->resourceType = resourceType;
    b->index = resourceIndex;
    b->role = RoleFor(mState, *node, resourceType, resourceIndex);
    b->t3Gear = b->role.gear;
    if (resourceType == "nam")
        b->t3Arch = "2";
    const auto slot = static_cast<std::size_t>(std::max(0, resourceIndex));
    if (slot < node->resources.size())
    {
        b->original = node->resources[slot];
        b->hadOriginal = b->original.IsLibraryRef() || b->original.IsFilePath();
        b->selectedId = b->original.resourceId;
        b->selectedPath = guitarfx::util::PathToUtf8(b->original.filePath);
    }

    // Everything below runs on the UI thread; a refill asked for from inside a list waits for
    // the click that asked to finish (the row it replaces must outlive its own handler).
    const auto later = [this](std::weak_ptr<Browser> weak, auto member) {
        mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weak, member] {
            if (const auto st = weak.lock(); st && alive.lock() && (st.get()->*member))
                (st.get()->*member)();
        });
    };
    const auto refillList = [later, weak = std::weak_ptr<Browser>(state)] { later(weak, &Browser::fillList); };

    auto& commands = mSession.Commands();
    const auto preview = [this, b](const std::string& id, const std::string& path) {
        b->previewed = true;
        b->selectedId = id;
        b->selectedPath = path;
        if (!id.empty())
            mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, id, b->index);
        else
            mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType, path, b->index);
        b->fillFooter();
    };

    // ── Library ─────────────────────────────────────────────────────────────────
    const auto libraryList = [this, b, preview, refillList]() -> Ptr {
        std::vector<const LibraryResource*> entries;
        if (const auto it = mState.resources.find(b->resourceType); it != mState.resources.end())
            for (const auto& r : it->second)
            {
                const auto q = Lower(b->query);
                const auto haystack = Lower(r.name + " " + r.category + " " + MetaText(r.metadata, "creatorName") + " " +
                                            MetaText(r.metadata, "authorUsername"));
                if ((q.empty() || haystack.find(q) != std::string::npos) && (b->category.empty() || r.category == b->category))
                    entries.push_back(&r);
            }
        std::stable_sort(entries.begin(), entries.end(), [](const LibraryResource* x, const LibraryResource* y) {
            return x->category != y->category ? x->category < y->category : Lower(x->name) < Lower(y->name);
        });

        std::vector<Ptr> rows;
        std::string category = "\x01";
        for (const auto* r : entries)
        {
            if (r->category != category)
            {
                category = r->category;
                const auto count = std::count_if(entries.begin(), entries.end(), [&](const LibraryResource* e) { return e->category == category; });
                auto head = std::make_shared<BrowserRow>(mTheme, mIcons, category.empty() ? "Uncategorised" : category, "");
                head->heading = true;
                head->icon = "ui-caret";
                head->note = std::to_string(count);
                rows.push_back(head);
            }
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, r->name, Describe(r->metadata, r->fileMissing ? "file missing" : ""));
            row->indent = true;
            row->selected = r->id == b->selectedId || (!b->selectedPath.empty() && r->filePath == b->selectedPath);
            row->onClick = [preview, refillList, id = r->id] {
                preview(id, {});
                refillList();
            };
            row->onDouble = [this, preview, id = r->id] {
                preview(id, {});
                CloseOverlay();
            };
            rows.push_back(row);
        }
        if (rows.empty())
        {
            auto none = std::make_shared<Text>(mTheme, b->query.empty() ? "The library has none yet." : "Nothing matches.", 12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            rows.push_back(FixedHeight(60, none));
        }
        return Col(rows);
    };

    const auto libraryControls = [this, b, &commands, refillList]() -> Ptr {
        if (!b->searchBox)
        {
            auto [element, box] = el::input_box(b->resourceType == "ir" ? "Search IRs..." : "Search models...", Font(Weight::Regular), 1.0f);
            b->searchBox = box;
            b->searchElement = el::share(std::move(element));
            b->searchBox->on_text = [b](std::string_view text) {
                b->query = std::string(text);
                if (b->fillList)
                    b->fillList(); // the list is its own slot: the box keeps its focus
            };
        }
        std::set<std::string> categories;
        if (const auto it = mState.resources.find(b->resourceType); it != mState.resources.end())
            for (const auto& r : it->second)
                categories.insert(r.category);
        auto category = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        category->Size(150, 30).Label(b->category.empty() ? std::string("All categories") : b->category, 11.0f).Caret(true);
        category->onClick = [this, b, categories, refillList, c = category.get()] {
            std::vector<MenuItem> items{{"All categories", b->category.empty(), false, [b, refillList] {
                                             b->category.clear();
                                             b->fillControls();
                                             refillList();
                                         }}};
            for (const auto& name : categories)
                if (!name.empty())
                    items.push_back({name, b->category == name, false, [b, name, refillList] {
                                         b->category = name;
                                         b->fillControls();
                                         refillList();
                                     }});
            OpenDropdown(c->Bounds(), std::move(items), 200.0f);
        };
        auto file = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        file->Size(64, 30).Label("File...", 11.0f);
        file->onClick = [this, b, &commands] {
            commands.BrowseNodeResource(b->nodeId, b->resourceType, b->index);
            b->previewed = true; // a file chosen there is the new choice; Cancel puts back the old
        };
        return Row({b->searchElement, HGap(8), category, HGap(8), file});
    };

    // ── Folder ──────────────────────────────────────────────────────────────────
    const auto roots = [this]() {
        std::vector<std::tuple<std::string, std::string, std::string>> list; // id, label, path
        if (const auto it = mState.appSettings.find(kRootsSetting); it != mState.appSettings.end() && it->is_array())
            for (const auto& r : *it)
                if (r.is_object())
                    list.emplace_back(r.value("id", ""), r.value("label", ""), r.value("path", ""));
        return list;
    };
    const auto rememberLocation = [this, b](const std::string& path) {
        auto locations = mState.appSettings.value(kLastLocationsSetting, nlohmann::json::object());
        if (!locations.is_object())
            locations = nlohmann::json::object();
        locations[b->role.context] = {{"rootId", b->rootId}, {"path", path}};
        mSession.Commands().SetSetting(kLastLocationsSetting, locations);
    };
    const auto openFolder = [this, b, rememberLocation](const std::string& path) {
        mSession.Commands().ListResourceFolder(path);
        rememberLocation(path);
    };

    const auto folderList = [this, b, preview, refillList, openFolder]() -> Ptr {
        const auto& listing = mState.resourceFolder;
        std::vector<Ptr> rows;
        if (b->rootId.empty())
        {
            auto hint = std::make_shared<Text>(mTheme, "Add a folder of models or IRs to browse it here.", 12.0f);
            hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            return FixedHeight(60, hint);
        }
        if (!listing.error.empty())
        {
            auto error = std::make_shared<Text>(mTheme, listing.error, 12.0f);
            error->Colour(mTheme.Error()).Alignment(Text::Align::Centre);
            return FixedHeight(60, error);
        }
        const auto filter = Lower(b->filter);
        for (const auto& dir : listing.dirs)
        {
            if (!filter.empty() && Lower(dir.name).find(filter) == std::string::npos)
                continue;
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, dir.name, "");
            row->icon = "ui-folder";
            row->onClick = [openFolder, path = dir.path] { openFolder(path); };
            rows.push_back(row);
        }
        for (const auto& file : listing.files)
        {
            if (file.resourceType != b->resourceType || (!filter.empty() && Lower(file.name).find(filter) == std::string::npos))
                continue;
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, file.name, Describe(file.metadata, ""));
            row->selected = !b->selectedPath.empty() && b->selectedPath == file.path;
            if (file.inLibrary)
            {
                row->note = "In library";
                row->noteColour = mTheme.Success();
            }
            row->onClick = [preview, refillList, path = file.path] {
                preview({}, path);
                refillList();
            };
            row->onDouble = [this, preview, path = file.path] {
                preview({}, path);
                CloseOverlay();
            };
            rows.push_back(row);
        }
        if (rows.empty())
        {
            auto none = std::make_shared<Text>(mTheme, listing.path.empty() ? "Loading..." : "No models or IRs here.", 12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            rows.push_back(FixedHeight(60, none));
        }
        return Col(rows);
    };

    const auto folderControls = [this, b, roots, openFolder, refillList]() -> Ptr {
        const auto list = roots();
        std::string label = "Choose a folder";
        for (const auto& [id, name, path] : list)
            if (id == b->rootId)
                label = name.empty() ? path : name;
        auto rootButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        rootButton->Size(140, 30).Label(label, 11.0f).Caret(true).Disabled(list.empty());
        rootButton->onClick = [this, b, list, openFolder, r = rootButton.get()] {
            std::vector<MenuItem> items;
            for (const auto& [id, name, path] : list)
                items.push_back({name.empty() ? path : name, id == b->rootId, false, [this, b, id = id, path = path, openFolder] {
                                     b->rootId = id;
                                     mSession.Commands().SetSetting(kActiveRootSetting, id);
                                     b->fillControls();
                                     openFolder(path);
                                 }});
            OpenDropdown(r->Bounds(), std::move(items), 220.0f);
        };
        auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Boxed);
        remove->Size(30, 30).IconSize(14).Disabled(b->rootId.empty());
        remove->onClick = [this, b] {
            auto rootsJson = mState.appSettings.value(kRootsSetting, nlohmann::json::array());
            nlohmann::json kept = nlohmann::json::array();
            for (const auto& r : rootsJson)
                if (r.is_object() && r.value("id", "") != b->rootId)
                    kept.push_back(r);
            mSession.Commands().SetSetting(kRootsSetting, kept);
            b->rootId.clear();
            // Refilled once this click is over: the button is in what the refill replaces.
            mSession.View().post([weak = b->self] {
                if (const auto st = weak.lock(); st && st->fillControls)
                {
                    st->fillControls();
                    st->fillList();
                }
            });
        };
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        add->Size(100, 30).Label("Add Folder...", 11.0f);
        add->onClick = [this] { mSession.Commands().BrowseResourceFolder(); };
        if (!b->filterBox)
        {
            auto [element, box] = el::input_box("Filter files...", Font(Weight::Regular), 1.0f);
            b->filterBox = box;
            b->filterElement = el::share(std::move(element));
            b->filterBox->on_text = [b](std::string_view text) {
                b->filter = std::string(text);
                if (b->fillList)
                    b->fillList();
            };
        }

        // Where the listing is, with the way up.
        const auto& listing = mState.resourceFolder;
        auto up = std::make_shared<IconButton>(mTheme, mIcons, "ui-up", IconButton::Look::Boxed);
        up->Size(30, 30).IconSize(14).Disabled(listing.parent.empty() || b->rootId.empty());
        up->onClick = [openFolder, parent = listing.parent] { openFolder(parent); };
        auto where = std::make_shared<Text>(mTheme, b->rootId.empty() ? std::string() : listing.path, 11.0f, Weight::Medium);
        where->Colour(mTheme.TextSecondary());
        std::size_t files = 0;
        for (const auto& f : listing.files)
            files += f.resourceType == b->resourceType ? 1 : 0;
        auto counts = std::make_shared<Text>(mTheme, b->rootId.empty() ? std::string()
                                                                      : std::to_string(listing.dirs.size()) +
                                                                            (listing.dirs.size() == 1 ? " folder  " : " folders  ") +
                                                                            std::to_string(files) + (files == 1 ? " file" : " files"),
                                             10.5f);
        counts->Colour(mTheme.TextMuted()).Fixed(true);
        return Col({FixedHeight(30, Row({rootButton, HGap(6), remove, HGap(6), add, HGap(8), b->filterElement})), VGap(8),
                    FixedHeight(30, Row({up, HGap(10), where, HGap(10), counts}))});
    };

    // ── Tone 3000 ───────────────────────────────────────────────────────────────
    const auto search = [this, b, refillList](int page) {
        b->t3Loading = true;
        b->t3Error.clear();
        b->t3Page = std::max(1, page);
        b->t3Open.clear();
        b->fillList();
        mSession.Tones().SearchTone3000(b->t3Query, b->t3Gear, b->t3Sort, b->resourceType == "nam" ? b->t3Arch : std::string(), b->t3Page,
                                        [b, refillList](const t3::Page<t3::Tone>& result, const std::string& error) {
                                            b->t3Loading = false;
                                            b->t3Error = error;
                                            b->t3Tones = result.items;
                                            b->t3Pages = result.totalPages;
                                            if (b->fillList)
                                                b->fillList();
                                            if (b->fillFooter)
                                                b->fillFooter();
                                        });
    };

    b->searchTone3000 = search;

    const auto tone3000List = [this, b, preview, refillList]() -> Ptr {
        std::vector<Ptr> rows;
        const auto message = [this](const std::string& text, el::color colour) {
            auto t = std::make_shared<Text>(mTheme, text, 12.0f);
            t->Colour(colour).Alignment(Text::Align::Centre);
            return FixedHeight(60, t);
        };
        if (b->t3Loading)
            return message("Searching Tone3000...", mTheme.TextMuted());
        if (!b->t3Error.empty())
            return message(b->t3Error, mTheme.Error());
        if (b->t3Tones.empty())
            return message("Search Tone3000 for models to add to your library.", mTheme.TextMuted());

        for (const auto& tone : b->t3Tones)
        {
            const int count = b->resourceType == "ir" ? tone.irsCount : tone.modelsCount;
            std::string detail = (tone.creatorName.empty() ? std::string() : "by " + tone.creatorName + " \xC2\xB7 ") + std::to_string(count) +
                                 (b->resourceType == "ir" ? " IRs" : " models") + (tone.platform.empty() ? "" : " \xC2\xB7 " + tone.platform);
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, tone.title, detail);
            row->picture = [this, url = tone.imageUrl] { return RemoteImage(url); };
            row->note = b->t3Open == tone.id ? "\xE2\x96\xB4" : "\xE2\x96\xBE";
            row->onClick = [this, b, tone, refillList] {
                if (b->t3Open == tone.id)
                {
                    b->t3Open.clear();
                    refillList();
                    return;
                }
                b->t3Open = tone.id;
                b->t3Models.clear();
                b->t3ModelsLoading = true;
                refillList();
                mSession.Tones().Tone3000Models(tone.id, [b, id = tone.id](const std::vector<t3::Model>& models, const std::string&) {
                    if (b->t3Open != id)
                        return;
                    b->t3Models = models;
                    b->t3ModelsLoading = false;
                    if (b->fillList)
                        b->fillList();
                });
            };
            rows.push_back(row);

            if (b->t3Open != tone.id)
                continue;
            if (b->t3ModelsLoading)
            {
                auto loading = std::make_shared<BrowserRow>(mTheme, mIcons, "Loading models...", "");
                loading->indent = true;
                rows.push_back(loading);
                continue;
            }
            for (const auto& model : b->t3Models)
            {
                auto modelRow = std::make_shared<BrowserRow>(mTheme, mIcons, model.name,
                                                             model.architecture.empty() ? std::string() : "A" + model.architecture);
                modelRow->indent = true;
                const auto* imported = t3::FindImported(mState, b->resourceType, model);
                modelRow->selected = imported != nullptr && imported->id == b->selectedId;
                if (mSession.Tones().IsImporting(model.id))
                {
                    modelRow->note = "Importing...";
                }
                else if (imported != nullptr)
                {
                    modelRow->note = "In library";
                    modelRow->noteColour = mTheme.Success();
                    modelRow->button = "Select";
                    modelRow->onButton = [preview, refillList, id = imported->id] {
                        preview(id, {});
                        refillList();
                    };
                }
                else
                {
                    modelRow->button = "Select";
                    modelRow->onButton = [this, b, tone, model, preview, refillList] {
                        mSession.Tones().ImportTone3000(tone, model, b->resourceType, b->resourceType == "ir" ? b->role.gear : std::string(),
                                                        [this, preview, refillList](const std::string& id, const std::string& error) {
                                                            if (!error.empty())
                                                            {
                                                                ShowToast("Could not import the model", error, true);
                                                                refillList();
                                                                return;
                                                            }
                                                            preview(id, {});
                                                            refillList();
                                                        });
                        refillList();
                    };
                }
                rows.push_back(modelRow);
            }
        }
        return Col(rows);
    };

    const auto tone3000Controls = [this, b, search]() -> Ptr {
        if (!b->t3Box)
        {
            auto [element, box] = el::input_box(b->resourceType == "ir" ? "Search cabinets and rooms..." : "Search amps and pedals...",
                                                Font(Weight::Regular), 1.0f);
            b->t3Box = box;
            b->t3Element = el::share(std::move(element));
            b->t3Box->on_text = [b](std::string_view text) { b->t3Query = std::string(text); };
            b->t3Box->on_enter = [b, search](std::string_view text) {
                b->t3Query = std::string(text);
                search(1);
                return true;
            };
        }
        auto go = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        go->Size(70, 30).Label("Search", 11.0f);
        go->onClick = [search] { search(1); };

        const auto choice = [this, b, search](const std::string& label, std::vector<std::pair<std::string, std::string>> options,
                                              std::string Browser::*field, float width) {
            auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            button->Size(width, 30).Label(label, 11.0f).Caret(true);
            button->onClick = [this, b, options, field, search, width, me = button.get()] {
                std::vector<MenuItem> items;
                for (const auto& [value, text] : options)
                    items.push_back({text, b->*field == value, false, [b, field, value = value, search] {
                                         b->*field = value;
                                         b->fillControls();
                                         search(1);
                                     }});
                OpenDropdown(me->Bounds(), std::move(items), std::max(150.0f, width));
            };
            return button;
        };
        const auto labelOf = [](const std::vector<std::pair<std::string, std::string>>& options, const std::string& value) {
            for (const auto& [v, text] : options)
                if (v == value)
                    return text;
            return options.empty() ? std::string() : options.front().second;
        };
        const std::vector<std::pair<std::string, std::string>> sorts{
            {"downloads-all-time", "Popular"}, {"newest", "Recent"}, {"trending", "Trending"}, {"best-match", "Best match"}};
        const std::vector<std::pair<std::string, std::string>> architectures{{"", "Any Architecture"}, {"1", "A1"}, {"2", "A2"}};

        std::vector<Ptr> row{b->t3Element, HGap(8), go, HGap(8), choice(labelOf(b->role.gears, b->t3Gear), b->role.gears, &Browser::t3Gear, 110),
                             HGap(6), choice(labelOf(sorts, b->t3Sort), sorts, &Browser::t3Sort, 96)};
        if (b->resourceType == "nam")
        {
            row.push_back(HGap(6));
            row.push_back(choice(labelOf(architectures, b->t3Arch), architectures, &Browser::t3Arch, 132));
        }
        return Row(row);
    };

    // ── The dialog ──────────────────────────────────────────────────────────────
    b->fillTabs = [this, b] {
        auto tab = [this, b](const char* label, Tab which) {
            auto t = std::make_shared<BrowserTab>(mTheme, label, b->tab == which);
            t->onClick = [this, b, which] {
                if (b->tab == which)
                    return;
                b->tab = which;
                // Tone 3000 opens on its popular tones for the slot's gear, as the web UI's does.
                if (which == Tab::Tone3000 && b->t3Tones.empty() && !b->t3Loading && b->searchTone3000)
                    b->searchTone3000(1);
                mSession.View().post([weak = b->self] {
                    if (const auto st = weak.lock())
                    {
                        st->fillTabs();
                        st->fillControls();
                        st->fillList();
                        st->fillFooter();
                    }
                });
            };
            return t;
        };
        Fill(*b->tabs, Row({HGap(4), tab("Resource Library", Tab::Library), tab("Folder", Tab::Folder), tab("Tone 3000", Tab::Tone3000), Stretch()}));
    };
    b->fillControls = [this, b, libraryControls, folderControls, tone3000Controls] {
        const float height = b->tab == Tab::Folder ? 68.0f : 30.0f;
        Fill(*b->controls, FixedHeight(height, b->tab == Tab::Library  ? libraryControls()
                                                 : b->tab == Tab::Folder ? folderControls()
                                                                         : tone3000Controls()));
        mSession.View().layout();
        mSession.View().refresh();
    };
    b->fillList = [this, b, libraryList, folderList, tone3000List] {
        Fill(*b->list, VScroll(Pad(0, 0, 10, 0, b->tab == Tab::Library ? libraryList() : b->tab == Tab::Folder ? folderList() : tone3000List())));
        mSession.View().layout();
        mSession.View().refresh();
    };
    b->fillFooter = [this, b, search] {
        std::vector<Ptr> row;
        if (b->tab == Tab::Tone3000 && !b->t3Tones.empty())
        {
            auto prev = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            prev->Size(60, 30).Label("Prev", 11.0f).Disabled(b->t3Page <= 1);
            prev->onClick = [b, search] { search(b->t3Page - 1); };
            auto next = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            next->Size(60, 30).Label("Next", 11.0f).Disabled(b->t3Page >= b->t3Pages);
            next->onClick = [b, search] { search(b->t3Page + 1); };
            auto page = std::make_shared<Text>(mTheme, "Page " + std::to_string(b->t3Page) + " of " + std::to_string(b->t3Pages), 11.0f);
            page->Colour(mTheme.TextSecondary()).Fixed(true);
            row = {Stretch(), prev, HGap(6), page, HGap(6), next, Stretch()};
        }
        else
        {
            row = {Stretch()};
        }
        auto cancel = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        cancel->Size(64, 30).Label("Cancel", 11.5f);
        cancel->onClick = [this, b] {
            // Put back what the slot held before anything was heard.
            if (b->previewed)
            {
                if (b->hadOriginal && b->original.IsLibraryRef())
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, b->original.resourceId, b->index);
                else if (b->hadOriginal && b->original.IsFilePath())
                    mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType, guitarfx::util::PathToUtf8(b->original.filePath),
                                                            b->index);
                else
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, {}, b->index);
            }
            b->previewed = false;
            CloseOverlay();
        };
        auto ok = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        ok->Size(44, 30).Label("OK", 11.5f).Disabled(!b->previewed);
        ok->onClick = [this, b] {
            // A file heard from a folder joins the library (in place), or picks the entry it is.
            if (b->previewed && b->selectedId.empty() && !b->selectedPath.empty())
            {
                std::string libraryId;
                std::string name = b->selectedPath;
                for (const auto& f : mState.resourceFolder.files)
                    if (f.path == b->selectedPath)
                    {
                        libraryId = f.inLibrary ? f.libraryId : std::string();
                        name = f.name;
                    }
                if (!libraryId.empty())
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, libraryId, b->index);
                else
                {
                    const auto dot = name.rfind('.');
                    mSession.Commands().SaveLocalLibraryResource(
                        b->resourceType, b->selectedPath, dot == std::string::npos ? name : name.substr(0, dot),
                        {{"sourceFolder", mState.resourceFolder.path}, {"sourceFileName", name}, {"origin", "folder-browser"}}, b->nodeId,
                        b->index);
                }
            }
            b->previewed = false;
            CloseOverlay();
        };
        row.push_back(cancel);
        row.push_back(HGap(8));
        row.push_back(ok);
        Fill(*b->footer, Row(row));
        mSession.View().layout();
        mSession.View().refresh();
    };

    // The folder to show first: where this kind of slot was browsed last, else the active root.
    {
        const auto list = roots();
        const auto active = mState.appSettings.value(kActiveRootSetting, std::string());
        std::string path;
        if (const auto locations = mState.appSettings.find(kLastLocationsSetting);
            locations != mState.appSettings.end() && locations->is_object() && locations->contains(b->role.context))
        {
            const auto& last = (*locations)[b->role.context];
            b->rootId = last.value("rootId", std::string());
            path = last.value("path", std::string());
        }
        if (b->rootId.empty())
            b->rootId = active;
        bool known = false;
        for (const auto& [id, name, rootPath] : list)
            if (id == b->rootId)
            {
                known = true;
                if (path.empty())
                    path = rootPath;
            }
        if (!known && !list.empty())
        {
            b->rootId = std::get<0>(list.front());
            path = std::get<2>(list.front());
        }
        else if (!known)
        {
            b->rootId.clear();
        }
        if (!path.empty())
            mSession.Commands().ListResourceFolder(path);
    }

    // The engine's answers: a folder listed or picked, the library grown (an import done).
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::ResourceFolders, [this, b, openFolder] {
        if (!mState.pickedResourceFolder.empty())
        {
            // A folder just added: kept as a root, shared with the web UI, and opened.
            const auto path = mState.pickedResourceFolder;
            mSession.Client().MutableState().pickedResourceFolder.clear();
            auto rootsJson = mState.appSettings.value(kRootsSetting, nlohmann::json::array());
            if (!rootsJson.is_array())
                rootsJson = nlohmann::json::array();
            std::string id;
            for (const auto& r : rootsJson)
                if (r.is_object() && r.value("path", "") == path)
                    id = r.value("id", "");
            if (id.empty())
            {
                id = "root-" + std::to_string(std::hash<std::string>{}(path) % 1000000007ull);
                const auto slash = path.find_last_of("/\\");
                rootsJson.push_back({{"id", id}, {"label", slash == std::string::npos ? path : path.substr(slash + 1)}, {"path", path}});
                mSession.Commands().SetSetting(kRootsSetting, rootsJson);
            }
            b->rootId = id;
            mSession.Commands().SetSetting(kActiveRootSetting, id);
            b->tab = Tab::Folder;
            b->fillTabs();
            openFolder(path);
        }
        if (b->tab == Tab::Folder)
        {
            b->fillControls();
            b->fillList();
        }
    }));
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::Resources, [b] {
        if (b->tab != Tab::Folder && b->fillList)
            b->fillList();
    }));
    // A tone's picture arriving, or an import moving on.
    b->toneListener = mSession.Tones().AddListener([this, b] {
        if (b->tab == Tab::Tone3000)
            mSession.View().refresh();
    });

    b->fillTabs();
    b->fillControls();
    b->fillList();
    b->fillFooter();

    auto title = std::make_shared<Text>(mTheme, b->role.title, 13.5f, Weight::SemiBold);
    title->Colour(mTheme.TextOnDark());
    auto close = std::make_shared<IconButton>(mTheme, mIcons, "x", IconButton::Look::Ghost);
    close->Size(28, 28).IconSize(12);
    close->onClick = [this, b] {
        // The x is Cancel, as in the web UI.
        if (b->previewed)
        {
            if (b->hadOriginal && b->original.IsLibraryRef())
                mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, b->original.resourceId, b->index);
            else if (b->hadOriginal && b->original.IsFilePath())
                mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType, guitarfx::util::PathToUtf8(b->original.filePath), b->index);
            else
                mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, {}, b->index);
        }
        CloseOverlay();
    };

    const auto view = mSession.View().size();
    const float width = std::min(944.0f, view.x - 16.0f);
    const float height = std::max(320.0f, view.y - 12.0f);
    auto rule = FixedHeight(1, Fill(mTheme.Text().opacity(0.07f), std::make_shared<el::element>()));
    auto panel = Col({FixedHeight(46, Pad(16, 9, 10, 9, Row({title, close}))), rule,
                      Pad(16, 10, 16, 0, Col({FixedHeight(38, b->tabs), FixedHeight(1, Fill(mTheme.Text().opacity(0.07f), std::make_shared<el::element>())), VGap(12),
                                              b->controls, VGap(10), b->list})),
                      FixedHeight(1, Fill(mTheme.Text().opacity(0.07f), std::make_shared<el::element>())),
                      FixedHeight(48, Pad(16, 9, 12, 9, b->footer))});
    ShowOverlay(panel, width, height);
    mOnOverlayClosed = [this, state] {
        mSession.Tones().RemoveListener(state->toneListener);
        state->subscriptions.clear();
        state->fillTabs = state->fillControls = state->fillList = state->fillFooter = nullptr;
        state->searchTone3000 = nullptr;
    };
    (void)commands;
}
} // namespace nanoq::ui
