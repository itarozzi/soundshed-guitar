// The full layout, as the web UI draws it above its compact breakpoint (ui-components/
// header-icon-bar.html, control-bar.html with input-control-group.html, preset-group.html and
// output-control-group.html, signal-path-bar.html, footer-bar.html): the PLAY / TONES tabs, the
// control bar, the chain with the selected effect's editor under it, and the full footer. Also
// what decides which layout the window gets, and the pieces both footers share.

#include "ui/Shell.h"

#include "ui/FrameParts.h"
#include "ui/ShellWidgets.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/PresetBrowse.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using namespace frame;

namespace
{
/// A tab of the top row (.nav-tab): its capital label, the accent and an underline when open.
class NavTab final : public Pressable
{
public:
    NavTab(const Theme& theme, std::string label, bool active) : mTheme(theme), mLabel(std::move(label)), mActive(active) {}

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(12.0f)).x + 28.0f;
        return {{w, metrics::kTabBarHeight}, {w, metrics::kTabBarHeight}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto ink = mActive ? mTheme.Accent() : (mHover ? mTheme.Text() : mTheme.TextSecondary());
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(12.0f), ink, el::rect{b.left, b.top, b.right, b.bottom - 2.0f}, HAlign::Centre);
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

std::string SourceLabel(const std::string& source)
{
    if (source == "user")
        return "User";
    if (source == "factory")
        return "Factory";
    if (source == "session")
        return "Session";
    return {};
}
} // namespace

// ── Which layout ────────────────────────────────────────────────────────────────

el::extent Shell::ViewSize() const
{
    auto& view = mSession.View();
    const auto size = view.size();
    const float scale = view.scale() > 0.0f ? view.scale() : 1.0f;
    return {size.x / scale, size.y / scale};
}

bool Shell::WantsWide() const
{
    // The web UI's rule (compactMode.ts): compact at or below 980 wide or 620 high, in layout
    // pixels (the zoom divided out), unless the density setting says always one or the other.
    const auto density = StringSetting(mState.appSettings, kDensitySetting, "auto");
    if (density == "full")
        return true;
    if (density == "compact")
        return false;
    const auto size = ViewSize();
    return size.x > metrics::kCompactMaxWidth && size.y > metrics::kCompactMaxHeight;
}

Ptr Shell::BuildFrame()
{
    if (mWide)
    {
        auto rule = [this] { return FixedHeight(1, Fill(mTheme.Text().opacity(0.06f), std::make_shared<el::element>())); };
        return Col({FixedHeight(metrics::kTabBarHeight, Yielding(mRailSlot)), rule(), FixedHeight(metrics::kHeaderHeight - 1.0f, Yielding(mTopSlot)),
                    rule(), Yielding(mPageSlot), FixedHeight(metrics::kWideFooterHeight, Yielding(mTransportSlot))});
    }
    // The rail down the left; beside it the preset bar, the stage and the footer. The regions
    // yield their minimum sizes: each is built for the window's size and rebuilt when it changes
    // (OnTick), and the window's own floor is the root's (MinSize).
    return Row({FixedWidth(metrics::kRailWidth, Yielding(mRailSlot)),
                Col({FixedHeight(metrics::kTopBarHeight, Yielding(mTopSlot)), Yielding(mPageSlot), FixedHeight(metrics::kFooterHeight, Yielding(mTransportSlot))})});
}

std::string Shell::TopBarSignature() const
{
    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    std::string s = std::string(mWide ? "w" : "c") + std::to_string(static_cast<int>(mPage)) + "|" + mState.activePresetId + "|" +
                    PresetTitle(mState) + "|" + (mState.activePresetDirty ? "d" : "") + (mState.activePreset ? "p" : "") +
                    (guitarfx::uiclient::IsPresetFavorite(mState, mState.activePresetId) ? "f" : "") + "|" +
                    (summary != nullptr ? summary->source : std::string()) + "|" + std::to_string(mPresetTrailAt) + "/" +
                    std::to_string(mPresetTrail.size());
    return s;
}

// ── The presets loaded, for back and forward ────────────────────────────────────

void Shell::NotePresetTrail()
{
    const auto& id = mState.activePresetId;
    if (id.empty())
        return;
    // Stepping back or forward lands on the trail's own entry, which stays where it is.
    if (mPresetTrailAt < mPresetTrail.size() && mPresetTrail[mPresetTrailAt] == id)
        return;

    if (!mPresetTrail.empty())
        mPresetTrail.resize(mPresetTrailAt + 1);
    mPresetTrail.push_back(id);
    constexpr std::size_t kMostKept = 50;
    if (mPresetTrail.size() > kMostKept)
        mPresetTrail.erase(mPresetTrail.begin(), mPresetTrail.begin() + static_cast<std::ptrdiff_t>(mPresetTrail.size() - kMostKept));
    mPresetTrailAt = mPresetTrail.size() - 1;
    Mark(kTopBar);
}

void Shell::StepPresetTrail(int direction)
{
    const auto target = static_cast<std::ptrdiff_t>(mPresetTrailAt) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(mPresetTrail.size()))
        return;
    const auto& id = mPresetTrail[static_cast<std::size_t>(target)];
    if (mState.FindPresetSummary(id) == nullptr)
    {
        // Deleted since: dropped from the trail rather than stepped onto.
        mPresetTrail.erase(mPresetTrail.begin() + target);
        if (mPresetTrailAt >= mPresetTrail.size())
            mPresetTrailAt = mPresetTrail.empty() ? 0 : mPresetTrail.size() - 1;
        Mark(kTopBar);
        return;
    }
    mPresetTrailAt = static_cast<std::size_t>(target);
    Mark(kTopBar);
    LoadWithConfirm(id);
}

// ── The tab row ─────────────────────────────────────────────────────────────────

Ptr Shell::BuildTabBar()
{
    auto tab = [this](const char* label, bool active, Page page) {
        auto t = std::make_shared<NavTab>(mTheme, label, active);
        t->onClick = [this, page] { ShowPage(page); };
        return t;
    };
    const bool play = mPage == Page::Chain || mPage == Page::Effect;

    auto settings = std::make_shared<IconButton>(mTheme, mIcons, "ui-gear", IconButton::Look::Boxed);
    settings->Size(28, 28).IconSize(15).Lit(mPage == Page::Settings);
    settings->Ink(mPage == Page::Settings ? mTheme.Accent() : mTheme.TextSecondary());
    settings->onClick = [this] { ShowPage(mPage == Page::Settings ? Page::Chain : Page::Settings); };

    // The tabs sit in the middle of the row whatever is beside them.
    std::vector<Ptr> tabItems{tab("PLAY", play, play ? mPage : Page::Chain)};
    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ToneSharing))
        tabItems.push_back(tab("TONES", mPage == Page::Tones, Page::Tones));
    tabItems.push_back(tab("PRESETS", mPage == Page::Presets, Page::Presets));
    auto tabs = Tight(tabItems);
    return Fill(mTheme.Background(), Row({HGap(38), Stretch(), tabs, Stretch(), Pad(0, 4, 0, 4, settings), HGap(10)}));
}

// ── The control bar ─────────────────────────────────────────────────────────────

Ptr Shell::BuildHeader()
{
    auto& commands = mSession.Commands();

    // The global chain at either end, the same cells as the compact Controls sheet; the meters
    // move with OnTick.
    mHeaderSubs.clear();
    auto cells = BuildControlCells(mHeaderSubs);
    mInMeter = cells.inMeter;
    mOutMeter = cells.outMeter;

    // ── The preset toolbar ──
    const bool dirty = mState.activePresetDirty;
    const bool havePreset = mState.activePreset.has_value();
    const std::string presetId = mState.activePresetId;
    const bool favourite = guitarfx::uiclient::IsPresetFavorite(mState, presetId);
    const auto* summary = mState.FindPresetSummary(presetId);

    auto boxed = [this](const char* icon, std::function<void()> onClick) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Boxed);
        b->Size(30, 28).IconSize(14);
        b->onClick = std::move(onClick);
        return b;
    };
    auto divider = [this] { return Pad(7, 6, 7, 6, FixedWidth(1, Fill(mTheme.Text().opacity(0.1f), std::make_shared<el::element>()))); };

    auto create = std::make_shared<IconButton>(mTheme, mIcons, "ui-new", IconButton::Look::Boxed);
    create->Size(72, 28).IconSize(13).Label("NEW", 10.5f).Caret(true);
    create->onClick = [this, b = create.get()] { OpenNewPresetMenu(b->Bounds()); };

    auto edit = boxed("ui-edit", [this] {
        const auto* s = mState.FindPresetSummary(mState.activePresetId);
        const auto& preset = mState.activePreset;
        if (!preset)
            return;
        if (s == nullptr || s->source != "user")
        {
            SaveActivePreset(true);
            return;
        }
        OpenPrompt("Rename preset", preset->name, "Rename", [this](const std::string& entered) {
            if (const auto& current = mState.activePreset)
                mSession.Commands().RenamePreset(entered, current->category, current->description);
        });
    });
    edit->Disabled(!havePreset);
    auto save = boxed("ui-save", [this] { SaveActivePreset(false); });
    save->Disabled(!dirty);
    if (dirty)
        save->Ink(mTheme.Accent());
    auto random = boxed("ui-random", [this] { LoadRandomPreset(); });

    // Back and forward through the presets loaded (#preset-undo, #preset-redo).
    auto back = ToolButton(mTheme, mIcons, "ui-undo", [this] { StepPresetTrail(-1); }, 28.0f, 15.0f);
    back->Disabled(mPresetTrailAt == 0);
    auto forward = ToolButton(mTheme, mIcons, "ui-redo", [this] { StepPresetTrail(1); }, 28.0f, 15.0f);
    forward->Disabled(mPresetTrailAt + 1 >= mPresetTrail.size());

    auto more = std::make_shared<IconButton>(mTheme, mIcons, "ui-more", IconButton::Look::Boxed);
    more->Size(30, 28).IconSize(14).Caret(true);
    more->onClick = [this, b = more.get()] { OpenPresetMenu(b->Bounds()); };

    auto toolbar = Tight({create, HGap(6), edit, HGap(6), save, divider(), random, divider(), back, HGap(2), forward, divider(), more});

    // ── The preset selector ──
    auto prev = std::make_shared<IconButton>(mTheme, mIcons, "ui-prev", IconButton::Look::Soft);
    prev->Size(30, 30).IconSize(16);
    prev->onClick = [&commands] { commands.StepPreset(-1); };
    auto next = std::make_shared<IconButton>(mTheme, mIcons, "ui-next", IconButton::Look::Soft);
    next->Size(30, 30).IconSize(16);
    next->onClick = [&commands] { commands.StepPreset(1); };

    auto pill = std::make_shared<PresetPill>(mTheme, mIcons, PresetTitle(mState), favourite, dirty);
    pill->Height(36).Badge(summary != nullptr ? SourceLabel(summary->source) : std::string());
    pill->onClick = [this] { ShowPage(mPage == Page::Presets ? Page::Chain : Page::Presets); };
    pill->onFavourite = [&commands, presetId, favourite] {
        if (!presetId.empty())
            commands.SetPresetFavorite(presetId, !favourite);
    };

    // The middle gets what the knobs leave, up to the web UI's 680 px.
    auto centre = WidthRange(330.0f, 680.0f, Col({VGap(27), FixedHeight(28, Row({Stretch(), toolbar, Stretch()})), VGap(9),
                                          FixedHeight(36, Row({Pad(0, 3, 0, 3, prev), HGap(8), pill, HGap(8), Pad(0, 3, 0, 3, next)})),
                                          std::make_shared<el::element>()}));

    auto meterColumn = [](Ptr meter, Ptr below) {
        std::vector<Ptr> parts{VGap(26), FixedHeight(84, std::move(meter))};
        if (below)
        {
            parts.push_back(VGap(2));
            parts.push_back(FixedHeight(18, Row({Stretch(), std::move(below), Stretch()})));
        }
        parts.push_back(std::make_shared<el::element>());
        return FixedWidth(30, Col(parts));
    };
    auto cellsAt = [](std::vector<Ptr> list) {
        auto row = std::make_shared<el::htile_composite>();
        for (auto& c : list)
            row->push_back(Pad(0, 28, 0, 0, c));
        return Ptr(row);
    };

    auto bar = Row({HGap(28), meterColumn(cells.inMeter, nullptr), HGap(14), cellsAt({cells.in, cells.gate, cells.transpose}), Stretch(),
                    centre, Stretch(), cellsAt({cells.eq, cells.doubler, cells.out}), HGap(14), meterColumn(cells.outMeter, cells.mute),
                    HGap(28)});
    return Fill(mTheme.Tertiary(), bar);
}

// ── The play stage ──────────────────────────────────────────────────────────────

Ptr Shell::BuildPlayStage()
{
    // The chain, as tall as its lines (BuildChain), with the scene chips over it and the add
    // button at its right; the selected effect's editor takes the rest.
    auto add = std::make_shared<RoundAdd>(mTheme, 24.0f);
    add->onClick = [this] { OpenEffectPicker(LastNodeBeforeOutput()); };
    auto chain = Over(Col({mSceneSlot, mChainSlot, FixedHeight(30, Pad(0, 0, 0, 6, Row({Stretch(), add, HGap(14)})))}),
                      std::make_shared<StageBackdrop>(mTheme));
    auto rule = FixedHeight(1, Fill(mTheme.Text().opacity(0.06f), std::make_shared<el::element>()));
    return Col({chain, rule, mEffectSlot});
}

// ── The footers' shared pieces ──────────────────────────────────────────────────

void Shell::PlayOrStopDemo()
{
    auto& commands = mSession.Commands();
    if (mState.demo.playing)
    {
        commands.StopDemo();
    }
    else if (const auto* c = SelectedDemoClip())
    {
        if (c->takeId.empty())
            commands.PreviewDemoClip(c->id, BoolSetting(mState.appSettings, kDemoRepeatSetting, false));
        else
            commands.PreviewRiff(c->takeId);
    }
}

Ptr Shell::BuildDemoControls(float chooserWidth, bool withRepeat)
{
    const auto* clip = SelectedDemoClip();
    const bool playing = mState.demo.playing;

    auto chooser = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    chooser->Size(chooserWidth, 26).Label(clip != nullptr ? clip->title : "Demo", 11.0f).Caret(true);
    chooser->Disabled(clip == nullptr);
    chooser->onClick = [this] { OpenDemoClips(); };
    // A long clip name keeps to the chooser's width.
    auto chooserBox = FixedWidth(chooserWidth, chooser);

    auto play = ToolButton(mTheme, mIcons, playing ? "ui-stop" : "ui-play", [this] { PlayOrStopDemo(); }, 26.0f, 12.0f);
    play->Lit(playing).Disabled(clip == nullptr);

    std::vector<Ptr> parts{chooserBox, HGap(4), play};
    if (withRepeat)
    {
        const bool repeat = BoolSetting(mState.appSettings, kDemoRepeatSetting, false);
        auto loop = ToolButton(mTheme, mIcons, "ui-loop", [this, repeat] { mSession.Commands().SetSetting(kDemoRepeatSetting, !repeat); },
                               26.0f, 14.0f);
        loop->Lit(repeat);
        parts.push_back(HGap(2));
        parts.push_back(loop);
    }
    return Tight(parts);
}

Ptr Shell::BuildTempoControls(bool withTap)
{
    auto& commands = mSession.Commands();
    char bpm[32];
    std::snprintf(bpm, sizeof(bpm), "%.1f BPM", mState.metronome.bpm);
    auto tempo = std::make_shared<IconButton>(mTheme, mIcons, "");
    tempo->Size(64, 26).Label(bpm, 11.0f).Ink(mTheme.Text());
    tempo->onClick = [this] { OpenMetronome(); };

    std::vector<Ptr> parts{tempo};
    if (withTap)
    {
        auto tap = std::make_shared<IconButton>(mTheme, mIcons, "");
        tap->Size(40, 26).Label("TAP", 10.0f).Ink(mTheme.TextSecondary());
        tap->onClick = [this] { TapTempo(); };
        auto metronome = ToolButton(mTheme, mIcons, "ui-metronome", [this] { OpenMetronome(); }, 26.0f, 15.0f);
        metronome->Ink(mTheme.TextSecondary());
        parts.push_back(HGap(2));
        parts.push_back(tap);
        parts.push_back(HGap(2));
        parts.push_back(metronome);
    }
    const bool ticking = mState.metronome.enabled;
    auto tick = ToolButton(mTheme, mIcons, ticking ? "ui-stop" : "ui-play", [&commands, ticking] { commands.SetMetronomeEnabled(!ticking); },
                           26.0f, 12.0f);
    tick->Lit(ticking);
    if (!withTap)
    {
        // Compact: the metronome's glyph stands in for the TAP and the dialog button.
        auto metronome = ToolButton(mTheme, mIcons, "ui-metronome", [this] { OpenMetronome(); }, 26.0f, 15.0f);
        metronome->Ink(mTheme.TextSecondary());
        parts.push_back(metronome);
    }
    parts.push_back(HGap(2));
    parts.push_back(tick);
    return Tight(parts);
}

Ptr Shell::ZoomButton()
{
    const float zoom = mSession.getZoom ? mSession.getZoom() : 1.0f;
    const int percent = static_cast<int>(std::lround(zoom * 100.0f));
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    button->Size(72, 28).Label(std::to_string(percent) + "%", 11.0f).Caret(true);
    button->onClick = [this, b = button.get(), percent] {
        std::vector<MenuItem> items;
        for (const int p : {70, 80, 90, 100, 110, 120, 130, 150})
            items.push_back({std::to_string(p) + "%", p == percent, false, [this, p] {
                                 if (mSession.setZoom)
                                     mSession.setZoom(static_cast<float>(p) / 100.0f);
                                 Mark(kTransport);
                             }});
        OpenDropdown(b->Bounds(), std::move(items), 110.0f);
    };
    return button;
}

// ── The full footer ─────────────────────────────────────────────────────────────

Ptr Shell::BuildWideFooter()
{
    const float width = ViewSize().x;
    auto divider = [this] { return Pad(10, 8, 10, 8, FixedWidth(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>()))); };

    auto pads = ToolButton(mTheme, mIcons, "ui-pads", [this] {
        mShowSetlists = true;
        ShowPage(Page::Presets);
        Mark(kPage);
    }, 28.0f, 16.0f);
    pads->Ink(mTheme.TextSecondary());
    auto tuner = ToolButton(mTheme, mIcons, "ui-tuner", [this] { OpenTuner(); }, 28.0f, 16.0f);
    tuner->Ink(mTheme.TextSecondary());
    const bool muted = mState.outputMuted;
    auto mute = ToolButton(mTheme, mIcons, muted ? "ui-mute" : "ui-speaker", [this, muted] { mSession.Commands().SetOutputMuted(!muted); },
                           28.0f, 16.0f);
    mute->Lit(muted);
    if (!muted)
        mute->Ink(mTheme.TextSecondary());

    // Render the clip through the preset: the web UI's camera (#footer-demo-render-btn).
    auto render = ToolButton(mTheme, mIcons, "ui-more", [this] { OpenDemoClips(); }, 26.0f, 14.0f);
    render->Ink(mTheme.TextSecondary());

    auto themeButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    const std::string themeName = mState.theme == "light" ? "Light" : mState.theme == "classic" ? "Classic" : "Dark";
    themeButton->Size(84, 28).Label(themeName, 11.0f).Caret(true);
    themeButton->onClick = [this, t = themeButton.get()] {
        std::vector<MenuItem> items;
        for (const auto& [id, label] : {std::pair<const char*, const char*>{"dark", "Dark"}, {"light", "Light"}, {"classic", "Classic"}})
            items.push_back({label, mState.theme == id, false, [this, id = std::string(id)] { mSession.Commands().SetTheme(id); }});
        OpenDropdown(t->Bounds(), std::move(items), 150.0f);
    };

    std::vector<Ptr> row{HGap(12), BuildHistoryControls(), divider(), BuildDemoControls(width >= 1180.0f ? 150.0f : 120.0f, true),
                         HGap(2), render, Stretch(), Tight({pads, HGap(6), tuner, HGap(6), mute}), Stretch(),
                         BuildTempoControls(true), divider()};
    if (mSession.getZoom && mSession.setZoom)
    {
        row.push_back(ZoomButton());
        row.push_back(HGap(8));
    }
    row.push_back(themeButton);
    row.push_back(HGap(12));
    return Fill(mTheme.Footer(), PadXY(0, 8, Row(row)));
}
} // namespace nanoq::ui
