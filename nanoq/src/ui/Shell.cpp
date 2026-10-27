#include "ui/Shell.h"

#include "ui/ShellIcons.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

Shell::Shell(UiSession& session)
    : mSession(session), mState(session.Client().MutableState()), mTheme(session.GetTheme()),
      mIcons(session.GetEngine().GetBundledAssetsPath() / "ui" / "images" / "icons"),
      mImages(session.GetEngine().GetBundledAssetsPath() / "ui"), mRemoteImages([] {
          std::error_code ec;
          const auto temp = std::filesystem::temp_directory_path(ec);
          return (ec ? std::filesystem::path(".") : temp) / "soundshed-nanoq-images";
      }())
{
    // The effect icons, colours and artwork: one table for every UI (core/ui/data/effect-presentation.json).
    mPresentation = guitarfx::uiclient::EffectPresentation::Load(session.GetEngine().GetBundledAssetsPath() / "ui" / "data" /
                                                                  "effect-presentation.json");
    AddShellIcons(mIcons);
    ApplyElementsTheme();
    BuildRoot();
    Subscribe();

    // The tone service's news (a picture arrived, a download moved on): the Tones page's cards
    // read it as they draw, so a redraw shows it. Its messages show as toasts.
    mToneListener = mSession.Tones().AddListener([this] {
        if (mPage == Page::Tones)
        {
            Mark(kPage); // rebuilt only if a card's state changed (TonesPageSignature)
            mSession.View().refresh();
        }
    });
    mSession.onToast = [this](const std::string& title, const std::string& detail, bool isError) { ShowToast(title, detail, isError); };
}

Shell::~Shell()
{
    *mAlive = false;
    mSession.Tones().RemoveListener(mToneListener);
    mSession.onToast = nullptr;

    // A dialog still open goes with the window, and so must what it asked of the engine: the
    // tuner left on with the window closed kept the output muted, with nothing left to turn it
    // off. The session, and so the engine, is still there (NanoQPresenter::on_detach).
    mInstrumentSubs.clear();
    if (auto closed = std::move(mOnOverlayClosed))
        closed();

    mSubscriptions.clear();
}

void Shell::Subscribe()
{
    auto& client = mSession.Client();
    auto on = [this, &client](Topic topic, unsigned bits) {
        mSubscriptions.push_back(client.Subscribe(topic, [this, bits] { Mark(bits); }));
    };

    // A settings change is patched in place; a new theme is noticed in Rebuild and redraws everything.
    on(Topic::Session, kTransport | kValues | kSettings);
    on(Topic::ActivePreset, kTopBar | kScenes | kChain | kPage | kValues);
    on(Topic::PresetLibrary, kTopBar | kPage);
    // Choosing a setlist moves only the cursor (nothing loads), so no other topic redraws the
    // Setlists tab for it.
    on(Topic::Setlists, kPage);
    on(Topic::Catalog, kChain | kPage);
    on(Topic::Resources, kChain | kPage);
    on(Topic::Tones, kPage);
    on(Topic::GlobalChain, kTransport);
    on(Topic::Mixer, kTransport);
    on(Topic::Demo, kTransport);

    // Every change the engine reports to the preset may be an edit for the chain's history.
    mSubscriptions.push_back(client.Subscribe(Topic::ActivePreset, [this] {
        NoteChainEdit();
        NotePresetTrail();
    }));
}

void Shell::BuildRoot()
{
    mRailSlot = MakeSlot();
    mTopSlot = MakeSlot();
    mSceneSlot = MakeSlot();
    mChainSlot = MakeSlot();
    mPageSlot = MakeSlot();
    mTransportSlot = MakeSlot();
    mEffectSlot = MakeSlot();

    // The frame (BuildFrame) holds the regions as the layout in force places them; it is
    // swapped when the window crosses the compact breakpoint. The background is its own slot
    // because a theme change repaints it.
    mBgSlot = MakeSlot();
    Fill(*mBgSlot, el::share(el::box(mTheme.Background())));
    mFrameSlot = MakeSlot();
    mWide = WantsWide();
    Fill(*mFrameSlot, BuildFrame());
    mRoot = Over(MinSize(metrics::kMinWidth, metrics::kMinHeight, mFrameSlot), mBgSlot);
}

void Shell::Rebuild()
{
    if (mAppliedTheme != mState.theme)
    {
        mAppliedTheme = mState.theme;
        mTheme.Set(mState.theme);
        ApplyElementsTheme();
        Fill(*mBgSlot, el::share(el::box(mTheme.Background())));
        mDirty |= kAll;
        mChainSignature.clear();
        mEffectSignature.clear();
        mSceneSignature.clear();
        mTopSignature.clear();
    }
    if (!mState.haveState)
    {
        if (!mShowingStarting)
        {
            Fill(*mRailSlot, Fill(mTheme.Bar(), std::make_shared<el::element>()));
            Fill(*mTopSlot, std::make_shared<el::element>());
            Fill(*mSceneSlot, std::make_shared<el::element>());
            Fill(*mChainSlot, std::make_shared<el::element>());
            Fill(*mTransportSlot, std::make_shared<el::element>());
            Fill(*mPageSlot, BuildStarting());
            mShowingStarting = true;
            mSession.View().layout();
            mSession.View().refresh();
        }
        mDirty = 0;
        return;
    }

    // The first state has arrived: every region is built.
    if (mShowingStarting)
    {
        mShowingStarting = false;
        mDirty |= kAll;
    }

    // The full layout's play page holds the chain and the editor in one stage: it is put back
    // when the page comes back to it, and its regions built afresh with it.
    const bool playStage = mWide && PlayShown();
    if (!playStage)
    {
        mPlayStageShown = false;
    }
    else if (!mPlayStageShown)
    {
        mDirty |= kPage | kChain | kScenes;
        mEffectSignature.clear();
        mChainSignature.clear();
        mSceneSignature.clear();
    }


    EnsureSelection();
    SelectAddedNode();

    // The chain strip is rebuilt only when what it shows changed: a parameter edit marks the
    // chain dirty too, and a rebuild under the cursor would cancel a click in progress.
    if (mDirty & kChain)
    {
        const auto signature = ChainSignature();
        if (signature == mChainSignature)
            mDirty &= ~kChain;
        else
            mChainSignature = signature;
    }

    // A page of sliders is patched in place when only values moved: rebuilding it under a drag
    // would drop the drag.
    if ((mDirty & kPage) && PlayShown() && mState.activePreset)
    {
        const auto signature = EffectPageSignature();
        if (signature == mEffectSignature)
            mDirty &= ~kPage;
        else
            mEffectSignature = signature;
    }
    else if ((mDirty & kPage) && mPage == Page::Tones)
    {
        // The Tones page scrolls; it is rebuilt (back to the top) only when what it lists changed.
        mEffectSignature.clear();
        const auto signature = TonesPageSignature();
        if (signature == mTonesSignature)
            mDirty &= ~kPage;
        else
            mTonesSignature = signature;
    }
    else if (mDirty & kPage)
    {
        mEffectSignature.clear();
        mTonesSignature.clear();
    }

    // The bar is rebuilt only when what it shows changed: every edit to the preset marks it, and
    // the full layout's bar holds knobs that a rebuild under a drag would drop.
    if (mDirty & kTopBar)
    {
        const auto signature = TopBarSignature();
        if (signature == mTopSignature)
            mDirty &= ~kTopBar;
        else
            mTopSignature = signature;
    }

    if (mDirty & kScenes)
    {
        const auto signature = SceneSignature();
        if (signature == mSceneSignature)
            mDirty &= ~kScenes;
        else
            mSceneSignature = signature;
    }

    if (mDirty & kRail)
        Fill(*mRailSlot, BuildRail());
    if (mDirty & kTopBar)
        Fill(*mTopSlot, BuildTopBar());
    if (mDirty & kScenes)
        Fill(*mSceneSlot, BuildScenes());
    if (mDirty & kChain)
        Fill(*mChainSlot, BuildChain());
    if ((mDirty & kPage) && playStage)
    {
        if (!mPlayStageShown)
        {
            Fill(*mPageSlot, BuildPlayStage());
            mPlayStageShown = true;
        }
        Fill(*mEffectSlot, BuildEffectPage());
    }
    else if (mDirty & kPage)
    {
        switch (mPage)
        {
        case Page::Chain:
            Fill(*mPageSlot, BuildChainStage());
            break;
        case Page::Effect:
            Fill(*mPageSlot, BuildEffectPage());
            break;
        case Page::Tones:
            Fill(*mPageSlot, BuildTonesPage());
            break;
        case Page::Presets:
            Fill(*mPageSlot, BuildPresetsPage());
            break;
        case Page::Settings:
            Fill(*mPageSlot, BuildSettingsPage());
            break;
        }
    }
    if (mDirty & kTransport)
        Fill(*mTransportSlot, BuildTransport());
    if ((mDirty & kValues) && PlayShown())
        SyncEffectValues();
    if ((mDirty & kSettings) && mPage == Page::Settings)
        SyncSettings();

    const bool structural = (mDirty & (kTopBar | kScenes | kChain | kPage | kTransport | kRail)) != 0;
    mDirty = 0;

    if (structural)
    {
        mSession.View().layout();
        mSession.View().refresh();
    }
}

void Shell::OnTick()
{
    // A window resized (or zoomed) across the breakpoint gets the other frame, every region
    // built for it.
    if (const bool wide = WantsWide(); wide != mWide)
    {
        mWide = wide;
        mPlayStageShown = false;
        mInMeter.reset();
        mOutMeter.reset();
        mHeaderSubs.clear();
        CloseOverlay();
        CloseDropdown();
        Fill(*mFrameSlot, BuildFrame());
        mDirty |= kAll;
        mChainSignature.clear();
        mEffectSignature.clear();
        mSceneSignature.clear();
        mTopSignature.clear();
        mTonesSignature.clear();
        mShowingStarting = false;
    }

    // A new size: the regions laid out for the old one are looked at again (the chain and the
    // pages only rebuild where their signatures see the change).
    if (const auto size = ViewSize(); size.x != mLastSize.x || size.y != mLastSize.y)
    {
        mLastSize = size;
        mDirty |= kTransport | kChain | kPage;
    }

    if (mHistoryCommitAt > 0.0 && UiSession::Now() >= mHistoryCommitAt)
        CommitChainHistory();
    if (mDirty != 0)
        Rebuild();

    // Errors and confirmations the engine sent (a save that failed, a preset saved).
    for (auto& n : mSession.Client().TakeNotifications())
        ShowToast(n.title, n.detail, n.kind == guitarfx::uiclient::Notification::Kind::Error);

    // The meters: the telemetry feed arrives at 20 Hz; only a bar that moved is redrawn.
    if (mInMeter && mOutMeter && mState.haveState)
    {
        const auto& t = mState.telemetry;
        if (mInMeter->Set(t.input.peakDb, t.input.clipped))
            mSession.View().refresh(*mInMeter);
        if (mOutMeter->Set(t.output.peakDb, t.output.clipped))
            mSession.View().refresh(*mOutMeter);

        char text[32];
        std::snprintf(text, sizeof(text), "DSP %d%%", static_cast<int>(t.dspLoadPercent + 0.5));
        if (mLoadText && mLoadText->Value() != text)
        {
            mLoadText->Set(text);
            mSession.View().refresh(*mLoadText);
        }

        SyncChainLights();
    }
}

// ── Selection and pages ─────────────────────────────────────────────────────────

const guitarfx::GraphNode* Shell::SelectedNode() const
{
    if (!mState.activePreset || mSelectedNode.empty())
        return nullptr;
    return mState.activePreset->graph.FindNode(mSelectedNode);
}

void Shell::EnsureSelection()
{
    if (!mState.activePreset)
    {
        mSelectedNode.clear();
        return;
    }

    if (SelectedNode() != nullptr)
        return;

    // The first thing worth editing: an effect, not the input or output.
    const auto layout = guitarfx::uiclient::BuildChainLayout(mState.activePreset->graph);
    for (const auto& id : layout.DrawnNodeIds())
    {
        if (const auto* node = mState.activePreset->graph.FindNode(id); node && !guitarfx::uiclient::IsBoundaryNode(*node))
        {
            mSelectedNode = id;
            return;
        }
    }
    mSelectedNode.clear();
}

void Shell::SelectAddedNode()
{
    // After adding an effect, the node that appears next is the one to show.
    if (mSelectAddedUntil <= 0.0)
        return;
    if (UiSession::Now() > mSelectAddedUntil || !mState.activePreset)
    {
        mSelectAddedUntil = 0.0;
        return;
    }

    for (const auto& node : mState.activePreset->graph.nodes)
    {
        if (mNodesBeforeAdd.count(node.id) == 0 && !guitarfx::uiclient::IsBoundaryNode(node))
        {
            mSelectAddedUntil = 0.0;
            mSelectedNode = node.id;
            mPage = Page::Effect;
            mDirty |= kChain | kPage | kTopBar | kRail;
            mChainSignature.clear();
            return;
        }
    }
}

void Shell::SelectNode(const std::string& nodeId)
{
    mSelectedNode = nodeId;
    mPage = Page::Effect;
    Mark(kChain | kPage | kTopBar | kRail);
    mChainSignature.clear();
}

void Shell::ShowPage(Page page)
{
    if (page == mPage)
        return;
    mPage = page;
    Mark(kTopBar | kPage | kRail);
}

// ── Starting ────────────────────────────────────────────────────────────────────

Ptr Shell::BuildStarting()
{
    auto text = std::make_shared<Text>(mTheme, "Starting the engine...", 16.0f, Weight::Medium);
    text->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
    return Fill(mTheme.Background(), el::share(el::align_center(el::align_middle(el::hold_any(text)))));
}

std::string Shell::SceneSignature() const
{
    if (!mState.activePreset)
        return {};

    std::string s = mState.activeSceneId;
    for (const auto& scene : mState.activePreset->scenes)
        s += "|" + scene.id + ":" + scene.title;
    return s;
}
} // namespace nanoq::ui
