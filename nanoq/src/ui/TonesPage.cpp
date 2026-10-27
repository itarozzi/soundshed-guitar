// The Tones page: Soundshed's tone sharing, as the web UI's Tones tab draws it at its compact
// size (ui-components/panels/sharing-panel.html, css/tone-sharing.css, css/compact/panels.css):
// a strip of chips (the account, Presets, Packs, Installed), a search with a tag filter, then the
// tones as cards - the latest packs in a row that scrolls sideways over the community presets in
// a grid - each with Install and Share, a pack with Open for its presets. Installed lists what
// this profile has installed, each with a way to remove it.
//
// The requests, the account and installing are the shared ToneService's
// (core/src/uiclient/ToneService.h); installing fetches the Tone3000 models a preset names from
// Soundshed's relay first. Publishing, packs of one's own, previews and favourites are Soundshed
// Guitar's.

#include "ui/Shell.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <ctime>

namespace nanoq::ui
{
namespace tones = guitarfx::uiclient::tones;

namespace
{
constexpr float kCardGap = 10.0f;
constexpr float kPresetCardMin = 196.0f; // the grid's minmax(168px, 1fr), at the compact density
constexpr float kPresetCardHeight = 156.0f;
constexpr float kPackCardWidth = 172.0f;
constexpr float kPackCardHeight = 206.0f;
constexpr float kActionBar = 38.0f;

/// The card's accent where it has no picture: a hue from its id, as the web UI's.
el::color AccentFor(const std::string& id)
{
    const auto hash = std::hash<std::string>{}(id);
    const float hue = static_cast<float>(hash % 360) / 360.0f;
    const auto channel = [hue](float offset) {
        float h = hue + offset;
        h -= static_cast<float>(static_cast<int>(h));
        const float x = std::abs(h * 6.0f - 3.0f) - 1.0f;
        return std::clamp(x, 0.0f, 1.0f) * 0.55f + 0.2f;
    };
    return el::color{channel(0.0f), channel(2.0f / 3.0f), channel(1.0f / 3.0f), 1.0f};
}

/// Up to `maxLines` lines of `text` in `width`, the last cut with an ellipsis.
std::vector<std::string> Wrap(artist::canvas& cnv, std::string text, const el::font_descr& font, float width, int maxLines)
{
    // A card shows running text: line breaks in it are spaces.
    std::replace_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i < text.size() && static_cast<int>(lines.size()) < maxLines)
    {
        auto end = text.find(' ', i);
        if (end == std::string::npos)
            end = text.size();
        const auto word = text.substr(i, end - i);
        const auto candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && el::measure_text(cnv, candidate, font).x > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = candidate;
        }
        i = end + 1;
    }
    if (static_cast<int>(lines.size()) < maxLines && !line.empty())
        lines.push_back(line);
    if (!lines.empty() && (i < text.size() || el::measure_text(cnv, lines.back(), font).x > width))
        lines.back() = Ellipsize(cnv, lines.back() + (i < text.size() ? "..." : ""), font, width);
    return lines;
}

/// A small rounded label on a card (.tone-sharing-chip, .tag-badge).
float DrawChip(artist::canvas& cnv, const std::string& text, float x, float y, el::color ink, el::color fill)
{
    const auto font = Font(Weight::SemiBold).size(9.0f);
    const float w = el::measure_text(cnv, text, font).x + 12.0f;
    const el::rect r{x, y, x + w, y + 15.0f};
    cnv.fill_style(fill);
    cnv.fill_round_rect(r, 7.5f);
    FillText(cnv, text, font, ink, r, HAlign::Centre);
    return w;
}

/// One tone as a card: its picture or accent, title, chips, description, tags and creator over
/// a bar of actions. Pictures are fetched as the card is drawn, and drawn once they arrive.
class ToneCard final : public el::element
{
public:
    struct Action
    {
        std::string icon;
        bool primary = false;  // the accent's filled download button
        bool enabled = true;
        std::string label;     // drawn instead of an icon ("Installed")
        std::function<void()> onClick;
    };

    ToneCard(const Theme& theme, Icons& icons, tones::Tone tone, std::function<std::shared_ptr<artist::image>(const std::string&)> image,
             float width, float height)
        : mTheme(theme), mIcons(icons), mTone(std::move(tone)), mImage(std::move(image)), mWidth(width), mHeight(height)
    {
    }

    std::vector<Action> actions;
    std::vector<std::string> lines; // extra lines (Installed's details) in place of chips and tags

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mWidth, mHeight}, {mWidth, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.5f, 0.5f);
        constexpr float kRadius = 10.0f;
        const auto accent = AccentFor(mTone.id);
        const auto picture = mImage ? mImage(mTone.thumbnailUrl) : nullptr;
        const el::rect face{b.left, b.top, b.right, b.bottom - kActionBar};

        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, kRadius);
            cnv.clip();
            cnv.fill_style(mTheme.Pick(el::rgba(30, 30, 38, 255), mTheme.Card()));
            cnv.fill_rect(b);
            if (picture)
            {
                DrawImageCovering(cnv, *picture, face);
                artist::canvas::linear_gradient shade{face.top_left(), face.bottom_left()};
                // The light theme fades the picture into the card instead (.tone-sharing-pack-hero).
                shade.add_color_stop(0.0f, mTheme.Pick(el::rgba(12, 12, 16, 200), el::rgba(235, 238, 245, 240)));
                shade.add_color_stop(1.0f, mTheme.Pick(el::rgba(12, 12, 16, 110), el::rgba(235, 238, 245, 205)));
                cnv.fill_style(shade);
                cnv.fill_rect(face);
            }
            else
            {
                artist::canvas::linear_gradient wash{face.top_left(), face.bottom_left()};
                wash.add_color_stop(0.0f, accent.opacity(0.22f));
                wash.add_color_stop(1.0f, accent.opacity(0.0f));
                cnv.fill_style(wash);
                cnv.fill_rect(face);
            }
            cnv.fill_style(mTheme.Pick(el::rgba(20, 20, 26, 255), mTheme.Card()));
            cnv.fill_rect(el::rect{b.left, face.bottom, b.right, b.bottom});
        }
        cnv.stroke_style(mTheme.Pick(mHover ? el::rgba(255, 255, 255, 40) : el::rgba(255, 255, 255, 18), mHover ? mTheme.BorderStrong() : mTheme.Border()));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b, kRadius);

        // Title (two lines at most), then what kind it is.
        const float x = b.left + 9.0f, w = b.width() - 18.0f;
        float y = b.top + 9.0f;
        const auto titleFont = Font(Weight::SemiBold).size(11.0f);
        for (const auto& line : Wrap(cnv, mTone.title, titleFont, w, 2))
        {
            FillText(cnv, line, titleFont, mTheme.Ink(), el::rect{x, y, x + w, y + 14.0f}, HAlign::Left);
            y += 14.0f;
        }
        y += 3.0f;

        const auto chipFill = mTheme.Pick(el::rgba(255, 255, 255, 22), mTheme.Panel());
        if (!lines.empty())
        {
            const auto font = Font(Weight::Regular).size(10.0f);
            for (const auto& line : lines)
            {
                FillText(cnv, Ellipsize(cnv, line, font, w), font, mTheme.TextSecondary(), el::rect{x, y, x + w, y + 13.0f}, HAlign::Left);
                y += 15.0f;
            }
        }
        else
        {
            float cx = x;
            if (mTone.IsPack())
                cx += DrawChip(cnv, "pack", cx, y, mTheme.TextSecondary(), chipFill) + 4.0f;
            else if (!mTone.type.empty() && mTone.type != "preset")
                cx += DrawChip(cnv, mTone.type, cx, y, mTheme.TextSecondary(), chipFill) + 4.0f;
            if (mTone.downloads >= 0 && !mTone.IsPack())
                DrawChip(cnv, tones::FormatCount(mTone.downloads) + (mTone.downloads == 1 ? " download" : " downloads"), cx, y,
                         mTheme.TextSecondary(), chipFill);
            y += 21.0f;

            if (!mTone.description.empty())
            {
                const auto font = Font(Weight::Regular).size(10.0f);
                for (const auto& line : Wrap(cnv, mTone.description, font, w, mTone.IsPack() ? 3 : 1))
                {
                    FillText(cnv, line, font, mTheme.TextSecondary(), el::rect{x, y, x + w, y + 13.0f}, HAlign::Left);
                    y += 13.0f;
                }
                y += 5.0f;
            }
            if (!mTone.IsPack() && !mTone.tags.empty())
            {
                float tx = x;
                for (const auto& tag : mTone.tags)
                {
                    const float cw = el::measure_text(cnv, tag, Font(Weight::SemiBold).size(9.0f)).x + 12.0f;
                    if (tx + cw > x + w)
                        break;
                    tx += DrawChip(cnv, tag, tx, y, mTheme.TextSecondary(), mTheme.Pick(el::rgba(255, 255, 255, 14), mTheme.Panel())) + 4.0f;
                }
            }
        }

        // The creator, at the foot of the face: an avatar and the name.
        if (!mTone.creatorName.empty())
        {
            const float cy = face.bottom - 14.0f;
            const el::point c{x + 8.0f, cy};
            const auto avatar = mImage ? mImage(mTone.creatorAvatarUrl) : nullptr;
            if (avatar)
            {
                auto state = cnv.new_state();
                cnv.add_circle(artist::circle(c, 8.0f));
                cnv.clip();
                DrawImageCovering(cnv, *avatar, el::rect{c.x - 8.0f, c.y - 8.0f, c.x + 8.0f, c.y + 8.0f});
            }
            else
            {
                cnv.fill_style(mTheme.Pick(el::rgba(255, 255, 255, 30), mTheme.Text().opacity(0.12f)));
                cnv.add_circle(artist::circle(c, 8.0f));
                cnv.fill();
                FillText(cnv, mTone.creatorName.substr(0, 1), Font(Weight::SemiBold).size(9.0f), mTheme.Ink(),
                         el::rect{c.x - 8.0f, c.y - 8.0f, c.x + 8.0f, c.y + 8.0f}, HAlign::Centre);
            }
            const auto font = Font(Weight::SemiBold).size(10.0f);
            FillText(cnv, Ellipsize(cnv, mTone.creatorName, font, w - 22.0f), font, mTheme.Ink(),
                     el::rect{x + 21.0f, cy - 7.0f, x + w, cy + 7.0f}, HAlign::Left);
        }

        // The actions.
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            const auto r = ActionRect(b, i);
            const auto& a = actions[i];
            const bool hover = static_cast<int>(i) == mHoverAction && a.enabled;
            el::color fill = a.primary ? mTheme.Accent().level(hover ? 1.08f : 0.95f)
                                       : mTheme.Pick(el::rgba(255, 255, 255, hover ? 34 : 18), mTheme.Text().opacity(hover ? 0.12f : 0.06f));
            if (!a.enabled)
                fill = mTheme.Pick(el::rgba(255, 255, 255, 10), mTheme.Text().opacity(0.04f));
            cnv.fill_style(fill);
            cnv.fill_round_rect(r, 6.0f);
            const auto ink = a.primary && a.enabled ? el::rgba(255, 255, 255, 255) : (a.enabled ? mTheme.TextSecondary() : mTheme.TextMuted());
            if (!a.label.empty())
                FillText(cnv, a.label, Font(Weight::SemiBold).size(9.5f), ink, r, HAlign::Centre);
            else
                mIcons.Draw(cnv, a.icon, r.inset(6.5f, 6.5f), ink);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
            return false;
        const int hit = HitAction(ctx.bounds, btn.pos);
        if (!btn.down && hit >= 0 && actions[static_cast<std::size_t>(hit)].enabled && actions[static_cast<std::size_t>(hit)].onClick)
            actions[static_cast<std::size_t>(hit)].onClick();
        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const int hit = inside ? HitAction(ctx.bounds, p) : -1;
        if (inside != mHover || hit != mHoverAction)
        {
            mHover = inside;
            mHoverAction = hit;
            ctx.view.refresh(ctx);
        }
        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

private:
    el::rect ActionRect(const el::rect& b, std::size_t i) const
    {
        // Square icon buttons from the left; a labelled one is wider.
        float x = b.left + 8.0f;
        for (std::size_t j = 0; j < i; ++j)
            x += (actions[j].label.empty() ? 26.0f : 70.0f) + 5.0f;
        const float w = actions[i].label.empty() ? 26.0f : 70.0f;
        const float y = b.bottom - kActionBar + 6.0f;
        return el::rect{x, y, x + w, y + 26.0f};
    }

    int HitAction(const el::rect& bounds, el::point p) const
    {
        const auto b = bounds.inset(0.5f, 0.5f);
        for (std::size_t i = 0; i < actions.size(); ++i)
            if (ActionRect(b, i).includes(p))
                return static_cast<int>(i);
        return -1;
    }

    const Theme& mTheme;
    Icons& mIcons;
    tones::Tone mTone;
    std::function<std::shared_ptr<artist::image>(const std::string&)> mImage;
    float mWidth, mHeight;
    bool mHover = false;
    int mHoverAction = -1;
};

/// A section's heading (.tone-sharing-row-title): small capitals.
Ptr SectionTitle(const Theme& theme, const std::string& text)
{
    auto t = std::make_shared<Text>(theme, Upper(text), 9.5f, Weight::SemiBold);
    t->Colour(theme.TextSecondary());
    return FixedHeight(26, Pad(2, 10, 0, 2, t));
}
} // namespace

std::string Shell::TonesPageSignature() const
{
    const auto& service = mSession.Tones();
    std::string s = std::to_string(static_cast<int>(mTonesTab)) + "|" + mTonesQuery + "|" + mTonesTag + "|" + std::to_string(mTonesPage) +
                    "|" + (mTonesMore ? "m" : "") + (mTonesLoading ? "l" : "") + "|" + mTonesError + "|" +
                    std::to_string(mToneItems.size()) + "/" + std::to_string(mTonePacks.size()) + "/" +
                    std::to_string(mToneHomePacks.size()) + "|" + (service.Account() ? service.Account()->Label() : std::string()) +
                    (service.HasSession() ? "s" : "") + "|" + std::to_string(static_cast<int>(ViewSize().x / 32.0f));
    // A card's Install button follows its install.
    for (const auto& entry : tones::InstalledEntries(mState.appSettings))
        s += "|i" + entry.id;
    for (const auto& [id, install] : mState.toneInstalls)
        s += "|t" + id + std::to_string(static_cast<int>(install.status));
    for (const auto* list : {&mToneItems, &mTonePacks, &mToneHomePacks})
        for (const auto& tone : *list)
            if (service.IsDownloading(tones::EntryIdFor(tone)))
                s += "|d" + tone.id;
    return s;
}

std::shared_ptr<artist::image> Shell::RemoteImage(const std::string& url)
{
    if (url.empty())
        return nullptr;
    return mRemoteImages.Find(url, mSession.Tones().Image(url));
}

void Shell::LoadTones(bool more)
{
    auto& service = mSession.Tones();
    const int generation = ++mTonesGeneration;
    mTonesLoading = true;
    mTonesLoaded = true;
    mTonesError.clear();
    if (!more)
        mTonesPage = 1;

    const auto failed = [this, generation](const tones::Response& response) {
        if (generation != mTonesGeneration)
            return false;
        mTonesLoading = false;
        if (!response.ok)
        {
            mTonesError = response.error.empty() ? std::string("Tone sharing could not be reached") : response.error;
            Mark(kPage);
            return true;
        }
        return false;
    };

    switch (mTonesTab)
    {
    case TonesTab::Presets:
    {
        const bool home = !more && mTonesQuery.empty() && mTonesTag.empty();
        if (!more)
        {
            mToneItems.clear();
            mToneHomePacks.clear();
        }
        if (home)
        {
            // The featured page's pack rows become the row of latest packs over the presets.
            service.Get("/home", [this, generation](const tones::Response& response) {
                if (generation != mTonesGeneration || !response.ok)
                    return;
                mToneHomePacks.clear();
                for (const auto& row : tones::ParseHome(response.data))
                    for (const auto& tone : row.tones)
                        if (tone.IsPack() && std::none_of(mToneHomePacks.begin(), mToneHomePacks.end(),
                                                          [&tone](const tones::Tone& t) { return t.id == tone.id; }))
                            mToneHomePacks.push_back(tone);
                Mark(kPage);
            });
        }
        const int page = more ? mTonesPage + 1 : 1;
        service.Get(tones::ItemsPath(page, mTonesQuery, mTonesTag), [this, generation, page, failed](const tones::Response& response) {
            if (failed(response) || generation != mTonesGeneration)
                return;
            const auto items = tones::ParseItems(response.data);
            mToneItems.insert(mToneItems.end(), items.begin(), items.end());
            mTonesPage = page;
            mTonesMore = static_cast<int>(items.size()) >= tones::kItemsPageSize;
            Mark(kPage);
        });
        break;
    }
    case TonesTab::Packs:
    {
        const int page = std::max(1, mTonesPage);
        service.Get(tones::PacksPath(page), [this, generation, failed](const tones::Response& response) {
            if (failed(response) || generation != mTonesGeneration)
                return;
            mTonePacks = tones::ParsePacks(response.data);
            mTonesMore = static_cast<int>(mTonePacks.size()) >= tones::kPacksPageSize;
            Mark(kPage);
        });
        break;
    }
    case TonesTab::Installed:
        mTonesLoading = false; // read from the settings, not the service
        break;
    }
}

Ptr Shell::BuildTonesPage()
{
    auto& service = mSession.Tones();
    if (!mTonesLoaded && !mTonesLoading)
    {
        service.RefreshAccount();
        LoadTones(false);
    }

    const float stageWidth = StageWidth();
    const float contentWidth = stageWidth - 2.0f * 10.0f - 14.0f; // margins, scroll bar
    auto image = [this](const std::string& url) { return RemoteImage(url); };

    // ── The chips: the account, then the tabs ──
    const auto& account = service.Account();
    auto accountChip = std::make_shared<SceneChip>(mTheme, account ? account->Label() : std::string(service.HasSession() ? "Soundshed" : "Sign In"),
                                                   true);
    accountChip->onClick = [this, a = accountChip.get()] { OpenToneAccount(a->Bounds()); };
    auto tab = [this](const char* label, TonesTab which) {
        auto t = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
        t->Size(60, 28).Label(label, 12.0f);
        if (mTonesTab == which)
            t->Lit(true).Ink(mTheme.Ink());
        else
            t->Ink(mTheme.TextSecondary());
        t->onClick = [this, which] {
            if (mTonesTab == which)
                return;
            mTonesTab = which;
            mTonesPage = 1;
            LoadTones(false);
            Mark(kPage);
        };
        return t;
    };
    auto chips = Row({HGap(10), Pad(0, 5, 0, 5, accountChip), HGap(12), tab("Presets", TonesTab::Presets), HGap(6), tab("Packs", TonesTab::Packs),
                      HGap(6), tab("Installed", TonesTab::Installed), Stretch()});

    std::vector<Ptr> body;

    // ── The search: words, a tag, and Clear (Presets only, as the web UI) ──
    if (mTonesTab == TonesTab::Presets)
    {
        if (!mTonesSearchBox)
        {
            auto [element, box] = el::input_box("Name, description, creator, or tag", Font(Weight::Regular), 1.0f);
            mTonesSearchBox = box;
            mTonesSearchElement = el::share(std::move(element));
            mTonesSearchBox->on_enter = [this](std::string_view text) {
                mTonesQuery = std::string(text);
                LoadTones(false);
                Mark(kPage);
                return true;
            };
        }
        auto tag = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        tag->Size(120, 30).Label(mTonesTag.empty() ? std::string("All tags") : mTonesTag, 11.0f).Caret(true);
        tag->onClick = [this, t = tag.get()] {
            std::vector<MenuItem> items{{"All tags", mTonesTag.empty(), false, [this] {
                                             mTonesTag.clear();
                                             LoadTones(false);
                                             Mark(kPage);
                                         }}};
            for (const auto& name : tones::StandardTags())
                items.push_back({name, mTonesTag == name, false, [this, name] {
                                     mTonesTag = name;
                                     LoadTones(false);
                                     Mark(kPage);
                                 }});
            OpenDropdown(t->Bounds(), std::move(items), 160.0f);
        };
        auto clear = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        clear->Size(56, 30).Label("Clear", 11.0f).Disabled(mTonesQuery.empty() && mTonesTag.empty());
        clear->onClick = [this] {
            mTonesQuery.clear();
            mTonesTag.clear();
            if (mTonesSearchBox)
                mTonesSearchBox->set_text("");
            LoadTones(false);
            Mark(kPage);
        };
        body.push_back(FixedHeight(30, Row({mTonesSearchElement, HGap(8), tag, HGap(6), clear})));
        body.push_back(VGap(4));
    }

    // A card's actions: Open (a pack), Install (Installed once it is), Share.
    auto actionsFor = [this, &service](const tones::Tone& tone) {
        std::vector<ToneCard::Action> actions;
        if (tone.IsPack())
            actions.push_back({"ui-eye", false, true, {}, [this, tone] { OpenPackDetail(tone); }});
        const auto entryId = tones::EntryIdFor(tone);
        const bool installed = tones::FindInstalled(mState.appSettings, entryId).has_value();
        const bool busy = service.IsDownloading(entryId) ||
                          (mState.toneInstalls.count(entryId) > 0 &&
                           mState.toneInstalls.at(entryId).status == guitarfx::uiclient::ToneInstall::Status::Installing);
        if (busy)
            actions.push_back({"", false, false, "Installing...", {}});
        else if (installed)
            actions.push_back({"", false, false, "Installed", {}});
        else
            actions.push_back({"ui-download", true, true, {}, [this, tone] { mSession.Tones().Install(tone); }});
        actions.push_back({"ui-share", false, true, {}, [this, tone] { ShareTone(tone); }});
        return actions;
    };

    auto grid = [&](const std::vector<tones::Tone>& list, float minWidth, float height) {
        const int columns = std::max(1, static_cast<int>((contentWidth + kCardGap) / (minWidth + kCardGap)));
        const float width = (contentWidth - static_cast<float>(columns - 1) * kCardGap) / static_cast<float>(columns);
        std::vector<Ptr> rows;
        for (std::size_t i = 0; i < list.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> row;
            for (std::size_t j = i; j < std::min(list.size(), i + static_cast<std::size_t>(columns)); ++j)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, list[j], image, width, height);
                card->actions = actionsFor(list[j]);
                if (!row.empty())
                    row.push_back(HGap(kCardGap));
                row.push_back(card);
            }
            row.push_back(Stretch());
            if (!rows.empty())
                rows.push_back(VGap(kCardGap));
            rows.push_back(FixedHeight(height, Row(row)));
        }
        return Col(rows);
    };

    auto message = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, text, 12.0f);
        t->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        return FixedHeight(60, t);
    };

    if (!mTonesError.empty())
        body.push_back(message(mTonesError));

    switch (mTonesTab)
    {
    case TonesTab::Presets:
    {
        if (!mToneHomePacks.empty())
        {
            body.push_back(SectionTitle(mTheme, "Latest packs"));
            std::vector<Ptr> strip;
            for (const auto& pack : mToneHomePacks)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, pack, image, kPackCardWidth, kPackCardHeight);
                card->actions = actionsFor(pack);
                if (!strip.empty())
                    strip.push_back(HGap(kCardGap));
                strip.push_back(card);
            }
            const float stripWidth = static_cast<float>(mToneHomePacks.size()) * (kPackCardWidth + kCardGap);
            body.push_back(FixedHeight(kPackCardHeight + 14.0f, HScroll(FixedWidth(stripWidth, Row(strip)))));
        }
        body.push_back(SectionTitle(mTheme, mTonesQuery.empty() && mTonesTag.empty() ? "Community presets" : "Search results"));
        if (mToneItems.empty())
            body.push_back(message(mTonesLoading ? "Loading..." : "No presets match."));
        else
            body.push_back(grid(mToneItems, kPresetCardMin, kPresetCardHeight));
        if (mTonesMore || mTonesLoading)
        {
            auto loadMore = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            loadMore->Size(110, 30).Label(mTonesLoading ? "Loading..." : "Load More", 11.0f).Disabled(mTonesLoading);
            loadMore->onClick = [this] {
                LoadTones(true);
                Mark(kPage);
            };
            body.push_back(VGap(10));
            body.push_back(FixedHeight(30, Row({Stretch(), loadMore, Stretch()})));
        }
        break;
    }
    case TonesTab::Packs:
    {
        body.push_back(SectionTitle(mTheme, "Latest packs"));
        if (mTonePacks.empty())
            body.push_back(message(mTonesLoading ? "Loading..." : "No packs yet."));
        else
            body.push_back(grid(mTonePacks, kPackCardWidth, kPackCardHeight));
        auto step = [this](const char* label, int delta, bool enabled) {
            auto b = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            b->Size(80, 28).Label(label, 11.0f).Disabled(!enabled);
            b->onClick = [this, delta] {
                mTonesPage = std::max(1, mTonesPage + delta);
                LoadTones(true);
                Mark(kPage);
            };
            return b;
        };
        auto pageText = std::make_shared<Text>(mTheme, "Page " + std::to_string(mTonesPage) + (mTonesMore ? "" : " \xC2\xB7 end of results"), 11.0f);
        pageText->Colour(mTheme.TextMuted()).Fixed(true);
        body.push_back(VGap(12));
        body.push_back(FixedHeight(28, Row({Stretch(), step("Previous", -1, mTonesPage > 1), HGap(8), step("Next", 1, mTonesMore), HGap(12),
                                            pageText, Stretch()})));
        break;
    }
    case TonesTab::Installed:
    {
        const auto entries = tones::InstalledEntries(mState.appSettings);
        if (entries.empty())
        {
            body.push_back(message("Nothing installed from tone sharing yet."));
            break;
        }
        std::vector<tones::Tone> list;
        std::vector<std::vector<std::string>> details;
        for (const auto& entry : entries)
        {
            tones::Tone tone;
            tone.id = entry.id;
            tone.title = entry.title;
            list.push_back(tone);
            std::string when = entry.importedAt.substr(0, 10);
            const auto source = entry.source == "toneSharingApi" ? std::string("Tone Sharing") : entry.source;
            const auto presets = entry.presetIds.size();
            details.push_back({source + (when.empty() ? "" : " \xC2\xB7 " + when),
                               std::to_string(presets) + (presets == 1 ? " preset" : " presets") + " \xC2\xB7 " +
                                   std::to_string(entry.resourceCount) + (entry.resourceCount == 1 ? " resource" : " resources")});
        }
        const int columns = std::max(1, static_cast<int>((contentWidth + kCardGap) / (kPackCardWidth + kCardGap)));
        const float width = (contentWidth - static_cast<float>(columns - 1) * kCardGap) / static_cast<float>(columns);
        std::vector<Ptr> rows;
        for (std::size_t i = 0; i < list.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> row;
            for (std::size_t j = i; j < std::min(list.size(), i + static_cast<std::size_t>(columns)); ++j)
            {
                auto card = std::make_shared<ToneCard>(mTheme, mIcons, list[j], nullptr, width, 128.0f);
                card->lines = details[j];
                const auto id = list[j].id, title = list[j].title;
                card->actions.push_back({"ui-trash", false, true, {}, [this, id, title] {
                                             OpenConfirm("Remove \"" + title + "\"",
                                                         "Its presets are deleted, and the models and IRs nothing else uses.", "Remove",
                                                         [this, id] { mSession.Tones().Remove(id); });
                                         }});
                if (!row.empty())
                    row.push_back(HGap(kCardGap));
                row.push_back(card);
            }
            row.push_back(Stretch());
            if (!rows.empty())
                rows.push_back(VGap(kCardGap));
            rows.push_back(FixedHeight(128.0f, Row(row)));
        }
        body.push_back(VGap(8));
        body.push_back(Col(rows));
        break;
    }
    }

    body.push_back(VGap(16));
    auto content = VScroll(Pad(10, 4, 14, 0, Col(body)));
    return Over(Col({FixedHeight(46, Pad(0, 4, 0, 4, chips)), FixedHeight(1, Fill(mTheme.Text().opacity(0.06f), std::make_shared<el::element>())),
                     content}),
                std::make_shared<StageBackdrop>(mTheme));
}

void Shell::OpenToneAccount(el::rect anchor)
{
    auto& service = mSession.Tones();
    if (service.Account() || service.HasSession())
    {
        const auto& account = service.Account();
        std::vector<MenuItem> items;
        if (account)
            items.push_back({account->Label(), false, true, {}});
        items.push_back({"Sign out", false, false, [this] {
                             OpenConfirm("Sign out of tone sharing", "This signs Soundshed Guitar out too: they share the account.", "Sign out",
                                         [this] { mSession.Tones().SignOut(); });
                         }});
        OpenDropdown(anchor, std::move(items), 200.0f);
        return;
    }

    // Signing in: an email, then the code the service emails to it.
    OpenPrompt("Sign in to tone sharing", "", "Email me a code", [this](const std::string& email) {
        mSession.Tones().RequestCode(email, [this, email](const guitarfx::uiclient::tones::Response& response) {
            if (!response.ok)
            {
                ShowToast("Could not send a code", response.error, true);
                return;
            }
            OpenPrompt("Enter the code sent to " + email, "", "Sign in", [this, email](const std::string& code) {
                mSession.Tones().VerifyCode(email, code, [this](const guitarfx::uiclient::tones::Response& answer) {
                    if (!answer.ok)
                        ShowToast("Could not sign in", answer.error, true);
                    Mark(kPage);
                });
            });
        });
    });
}

void Shell::OpenPackDetail(const tones::Tone& pack)
{
    mSession.Tones().Get(tones::PackPath(pack.id), [this, pack](const tones::Response& response) {
        const auto detail = response.ok ? tones::ParsePackDetail(response.data) : std::nullopt;
        if (!detail)
        {
            ShowToast("Could not open \"" + pack.title + "\"", response.error, true);
            return;
        }
        std::vector<MenuItem> items;
        items.push_back({std::to_string(detail->items.size()) + (detail->items.size() == 1 ? " preset" : " presets"), false, true, {}});
        for (const auto& entry : detail->items)
        {
            tones::Tone item;
            item.id = entry.itemId;
            item.title = entry.title;
            item.type = entry.type;
            const bool installed = tones::FindInstalled(mState.appSettings, tones::ItemEntryId(entry.itemId)).has_value();
            MenuItem row{entry.title, false, installed, [this, item] { mSession.Tones().Install(item); }};
            row.detail = installed ? "Installed" : "Install";
            items.push_back(row);
        }
        items.push_back({"", false, true, {}});
        items.push_back({"Install the whole pack", false, false, [this, pack] { mSession.Tones().Install(pack); }});
        items.push_back({"Copy the link to share it", false, false, [this, pack] { ShareTone(pack); }});
        OpenMenu(detail->pack.title.empty() ? pack.title : detail->pack.title, std::move(items));
    });
}

void Shell::ShareTone(const tones::Tone& tone)
{
    el::clipboard(tones::ShareLink(tone));
    ShowToast("Link copied", tone.title, false);
}
} // namespace nanoq::ui
