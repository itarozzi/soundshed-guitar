#pragma once

// Shell - the frame every page sits in. At or below the web UI's compact breakpoint (980 x 620,
// core/ui/ts/compactMode.ts; the ui.density setting can force either) it is laid out as the web
// UI's compact landscape layout (html[data-compact-layout="rail"]): a rail at the left that switches between the Chain and
// Effect stages (the two halves of the Play tab, which take turns at this size) and opens
// Settings; the preset bar along the top; the stage; and the footer with its Options sheet.
// Presets and Settings take the stage's place when open.
//
// Above it, the full layout: the PLAY / TONES / PRESETS tabs along the top, the control bar
// (input, gate, transpose, the preset toolbar and selector, EQ, doubler, output), the chain with
// the selected effect's editor under it, and the full footer. The same regions are rebuilt; the
// frame around them is swapped when the window crosses the breakpoint.
//
// The UI is rebuilt from the client's state, region by region, rather than patched: what a
// region shows is a pure function of ClientState, so a change just marks the regions it
// touches and the next tick (on the UI thread, between events) rebuilds them. Two things are
// updated in place instead, because rebuilding them would be visible: the meters (20 Hz) and
// the values on the sliders of the page being shown (a rebuild would drop a drag).

#include "ui/Graphics.h"
#include "ui/KeyRouter.h"
#include "ui/Layout.h"
#include "ui/ShellWidgets.h"
#include "ui/UiSession.h"
#include "ui/Widgets.h"
#include "uiclient/ChainHistory.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/Instruments.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/PresetTrail.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nanoq::ui
{
enum class Page
{
    Chain,
    Effect,
    Tones,
    Presets,
    Settings
};

/// One row of a pop-up list.
struct MenuItem
{
    std::string label;
    bool selected = false;
    bool heading = false;
    std::function<void()> onChoose;
    std::string detail; // muted, at the right
    std::string icon;   // an icon name (images/icons), drawn at the left in `tint`
    std::uint32_t tint = 0;
};

class NodeCard; // ChainStrip.cpp
namespace chain
{
struct HitMap; // ChainWidgets.h
}

class Shell
{
public:
    explicit Shell(UiSession& session);
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    [[nodiscard]] Ptr Root() const
    {
        return mRoot;
    }

    /// Rebuilds the regions the last messages touched and moves the meters. UI thread.
    void OnTick();

private:
    enum Dirty : unsigned
    {
        kTopBar = 1u << 0,
        kScenes = 1u << 1,
        kChain = 1u << 2,
        kPage = 1u << 3,
        kTransport = 1u << 4,
        kValues = 1u << 5, // the effect page's slider values only
        kSettings = 1u << 6, // the settings page's controls only
        kRail = 1u << 7, // the compact rail, or the full layout's tab row
        kAll = kTopBar | kScenes | kChain | kPage | kTransport | kRail
    };

    void Mark(unsigned bits)
    {
        mDirty |= bits;
    }

    void Subscribe();
    void BuildRoot();
    void Rebuild();

    /// The keyboard shortcuts (Keys.cpp), as the web UI's: undo and redo, Space, Delete, Esc, F2.
    bool HandleKey(const el::key_info& k);
    /// Space with no effect picked out: a tap on the tempo.
    void TapTempo();
    /// A layer of the view's own (a dialog, a drop-down), which takes keys while it has focus,
    /// wrapped so the shortcuts (Esc above all) still reach HandleKey.
    Ptr KeyLayer(Ptr layer);
    /// The standalone window's caption takes the colour of the bar under it (Windows).
    void SyncWindowChrome();

    // The window's size and which layout it gets (ShellFrame.cpp).
    /// The view's size in layout units: the zoom divided out.
    [[nodiscard]] el::extent ViewSize() const;
    [[nodiscard]] bool WantsWide() const;
    /// Where the stage starts and how wide it is: right of the rail when compact.
    [[nodiscard]] float StageLeft() const
    {
        return mWide ? 0.0f : metrics::kRailWidth;
    }
    [[nodiscard]] float StageWidth() const
    {
        return ViewSize().x - StageLeft();
    }
    /// The frame for the layout in force, around the region slots.
    Ptr BuildFrame();
    /// Chain and Effect are one page in the full layout.
    [[nodiscard]] bool PlayShown() const
    {
        return mPage == Page::Effect || (mWide && mPage == Page::Chain);
    }

    // Shell.cpp
    Ptr BuildRail();
    Ptr BuildTopBar();
    Ptr BuildScenes();
    Ptr BuildTransport();
    Ptr BuildStarting();
    Ptr BuildChainStage();

    // The full layout (WideFrame.cpp).
    Ptr BuildTabBar();
    Ptr BuildHeader();
    Ptr BuildPlayStage();
    Ptr BuildWideFooter();
    /// The demo clip chooser and its play button, as both footers show them.
    [[nodiscard]] Ptr BuildDemoControls(float chooserWidth, bool withRepeat);
    /// The tempo, tap and the metronome's switch, as both footers show them.
    [[nodiscard]] Ptr BuildTempoControls(bool withTap);
    /// Undo, redo, the A/B pair and the copy between them, as both footers show them.
    [[nodiscard]] Ptr BuildHistoryControls();
    void PlayOrStopDemo();
    /// The zoom's select (100%, ...), for the Options sheet and the full footer.
    [[nodiscard]] Ptr ZoomButton();
    /// The presets loaded, in order, for the control bar's back and forward (preset-undo/redo).
    void NotePresetTrail();
    void StepPresetTrail(int direction);

    /// The global chain's cells (Controls.cpp): the compact Controls sheet and the full
    /// layout's control bar lay out the same ones. Their values follow the state through
    /// `subs`, for as long as those are held.
    struct ControlCells
    {
        Ptr in, gate, transpose, eq, doubler, out;
        std::shared_ptr<MeterBar> inMeter, outMeter;
        Ptr mute;
    };
    ControlCells BuildControlCells(std::vector<guitarfx::uiclient::Subscription>& subs);
    /// A small panel beside `anchor` (a control's bounds), closed by a tap anywhere else.
    void ShowPopover(Ptr panel, el::rect anchor, float width, float height, bool above = false);
    /// The panel placed beside `anchor`, over a layer that calls `onOutside` when tapped.
    [[nodiscard]] Ptr PlacePanel(Ptr panel, el::rect anchor, float width, float height, bool above, std::function<void()> onOutside);
    void CloseDropdown();
    /// A drop-down list under `anchor`, as the web UI's menus open from their buttons.
    void OpenDropdown(el::rect anchor, std::vector<MenuItem> items, float width = 180.0f);
    void OpenOptions(el::rect anchor);
    void OpenNewPresetMenu(el::rect anchor);
    void LoadRandomPreset();

    // The chain's undo/redo and A/B (ShellFrame.cpp), as the web UI's footer (history.ts).
    /// An edit happened, or may have: the chain is compared and recorded once it stays still.
    void NoteChainEdit();
    void CommitChainHistory();
    void ApplyChainSnapshot(const guitarfx::uiclient::ChainSnapshot& target);
    void UndoChain();
    void RedoChain();
    void SetChainSlot(guitarfx::uiclient::ChainHistory::Slot slot);
    void CopyChainSlot();
    void ShowPage(Page page);
    void SelectNode(const std::string& nodeId);
    void EnsureSelection();
    void SelectAddedNode();
    [[nodiscard]] const guitarfx::GraphNode* SelectedNode() const;

    // ChainStrip.cpp
    Ptr BuildChain();
    [[nodiscard]] std::string ChainSignature() const;
    /// Moves each card's signal light to what the telemetry feed says. UI thread, per tick.
    void SyncChainLights();
    /// Files dragged in from the desktop (ChainStrip.cpp): a .nam model or a .wav IR held over
    /// an effect that plays one lights its card; dropped, it joins the library where it is and
    /// loads into that effect, or into the one shown, as the web UI's file drop does.
    void TrackFileDrop(const el::drop_info& info, bool leaving);
    bool DropFiles(const el::drop_info& info);
    /// The effect a model or IR file dropped at `where` goes into, or empty.
    [[nodiscard]] std::string FileDropTarget(const std::string& resourceType, el::point where) const;
    /// A card being dragged: the drop target under `to` is lit, and on `done` the card moves
    /// there by the web UI's rules (ChainDropRules).
    void DragCard(const std::string& nodeId, el::point from, el::point to, bool done);

    [[nodiscard]] std::string SceneSignature() const;
    /// What the preset bar (or the full control bar) shows that a rebuild would change.
    [[nodiscard]] std::string TopBarSignature() const;

    // EffectPage.cpp
    [[nodiscard]] std::string EffectPageSignature() const;
    Ptr BuildEffectPage();
    void SyncEffectValues();
    void OpenEffectPresets();
    void OpenEnumPicker(const std::string& nodeId, const guitarfx::uiclient::EffectParamInfo& info);
    [[nodiscard]] Ptr BuildResourceSlot(const guitarfx::GraphNode& node, const std::string& resourceType, int resourceIndex,
                                        const std::string& label, bool optional);
    [[nodiscard]] Ptr BuildSplitBranches(const guitarfx::GraphNode& splitter);
    /// A third-party plugin node's list, Open Plugin and Browse (NodeControls.cpp).
    [[nodiscard]] Ptr BuildPluginHost(const guitarfx::GraphNode& node);
    /// A blend node's blend and its mode (NodeControls.cpp).
    [[nodiscard]] Ptr BuildBlendInfo(const guitarfx::GraphNode& node);

    // PresetsPage.cpp
    Ptr BuildPresetsPage();
    Ptr BuildPresetListContent();
    void RebuildPresetList();
    void LoadWithConfirm(const std::string& presetId);
    void DeleteWithConfirm(const std::string& presetId);
    void OpenPresetRowMenu(const guitarfx::uiclient::PresetSummary& preset);
    void OpenFolderMenu(const std::string& folderId);
    void SelectPresetBrowserFolder(const std::string& folderId);
    [[nodiscard]] Ptr BuildFolderColumn();
    [[nodiscard]] Ptr BuildSetlistsView(Ptr tabs);
    /// A menu or prompt opened from another menu's item, once that menu has closed.
    void PostMenu(const std::string& title, std::vector<MenuItem> items);
    void PostPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                    std::function<void(const std::string&)> onSubmit);

    // TonesPage.cpp: tone sharing, as the web UI's Tones tab
    enum class TonesTab
    {
        Presets,
        Packs,
        Installed
    };
    Ptr BuildTonesPage();
    [[nodiscard]] std::string TonesPageSignature() const;
    /// Asks the service for the open tab's tones: page 1 afresh, or with `more` the next page.
    void LoadTones(bool more);
    void OpenToneAccount(el::rect anchor);
    void OpenPackDetail(const guitarfx::uiclient::tones::Tone& pack);
    void ShareTone(const guitarfx::uiclient::tones::Tone& tone);
    [[nodiscard]] std::shared_ptr<artist::image> RemoteImage(const std::string& url);

    // ResourceBrowser.cpp: the model and IR chooser, as the web UI's resource browser
    void OpenResourceBrowser(const std::string& nodeId, const std::string& resourceType, int resourceIndex);

    // SettingsPage.cpp
    Ptr BuildSettingsPage();
    void SyncSettings();

    // Dialogs.cpp
    void ApplyElementsTheme();
    void ShowOverlay(Ptr panel, float width, float height);
    void CloseOverlay();
    void OpenMenu(const std::string& title, std::vector<MenuItem> items);
    void OpenPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                    std::function<void(const std::string&)> onSubmit);
    void OpenConfirm(const std::string& title, const std::string& detail, const std::string& confirmLabel,
                     std::function<void()> onYes);
    /// Adds the chosen effect after `afterNodeId`, or, with `replaceNodeId`, puts it in that
    /// node's place.
    void OpenEffectPicker(const std::string& afterNodeId, const std::string& replaceNodeId = {});
    /// Adds the chosen effect on this edge of the graph.
    void OpenEffectPickerOnEdge(const guitarfx::GraphEdge& edge);
    void ExpectAddedNode();
    void ShowToast(const std::string& title, const std::string& detail, bool isError);
    [[nodiscard]] std::vector<const guitarfx::uiclient::EffectTypeInfo*> OfferedEffects() const;

    // Instruments.cpp
    void OpenDemoClips();
    [[nodiscard]] const guitarfx::uiclient::DemoClip* SelectedDemoClip() const;
    [[nodiscard]] std::vector<const std::vector<guitarfx::uiclient::DemoClip>*> DemoClipLists() const;
    void OpenTuner();
    void OpenMetronome();
    void OpenControls();
    void OpenGlobalEq(el::rect anchor); // Controls.cpp

    // Menus.cpp
    void OpenPresetMenu(std::optional<el::rect> anchor = std::nullopt);
    void OpenNodeMenu(const std::string& nodeId);
    void RenameNodeWithPrompt(const std::string& nodeId);
    void SaveActivePreset(bool asNew);
    /// The save dialog: name, category, description, folder and tags (Dialogs.cpp).
    void OpenPresetDetails(bool asNew);
    [[nodiscard]] std::string MoveTarget(const std::string& nodeId, int direction) const;
    [[nodiscard]] std::string LastNodeBeforeOutput() const;

    UiSession& mSession;
    guitarfx::uiclient::ClientState& mState;
    Theme& mTheme;
    Icons mIcons;
    Images mImages;

    Ptr mRoot;
    std::shared_ptr<KeyRouter> mKeys;
    /// The last click picked a chain card: Space bypasses it and Delete removes it, as when
    /// a card has focus in the web UI. Any other click clears it.
    bool mChainFocused = false;
    SlotPtr mFrameSlot, mEffectSlot;
    SlotPtr mBgSlot, mRailSlot, mTopSlot, mSceneSlot, mChainSlot, mPageSlot, mTransportSlot;

    Page mPage = Page::Chain;
    bool mWide = false;            // the full layout is in force
    el::extent mLastSize{0.0f, 0.0f}; // the regions sized to the window are rebuilt when it changes
    bool mPlayStageShown = false;  // the full layout's page slot holds the chain and editor
    float mChainAreaHeight = 0.0f; // the full layout's chain, as tall as its lines
    float mChainAreaLimit = 0.0f;  // how tall it may grow, once the bar under it is dragged
    float mChainDragStart = -1.0f; // the chain's room when that drag began
    std::vector<guitarfx::uiclient::Subscription> mHeaderSubs; // the control bar's live values
    guitarfx::uiclient::PresetTrail mPresetTrail;
    std::shared_ptr<guitarfx::uiclient::TapTempo> mTapTempo;
    bool mWrapChain = true; // the web UI's signalPathWrapCompact, on unless turned off

    // The Tones page: what it shows, and what the service last answered.
    TonesTab mTonesTab = TonesTab::Presets;
    std::string mTonesQuery, mTonesTag;
    int mTonesPage = 1;
    bool mTonesMore = false;     // another page of presets to load, or of packs to step to
    bool mTonesLoading = false;
    bool mTonesLoaded = false;   // the open tab has been asked for since it was opened
    int mTonesGeneration = 0;    // answers to an older request are dropped
    std::string mTonesError;
    std::vector<guitarfx::uiclient::tones::Tone> mToneItems, mTonePacks, mToneHomePacks;
    std::shared_ptr<el::basic_input_box> mTonesSearchBox;
    Ptr mTonesSearchElement;
    int mToneListener = 0;
    std::string mTonesSignature;
    RemoteImages mRemoteImages;

    guitarfx::uiclient::ChainHistory mHistory;
    std::string mHistoryContext;     // presetId::sceneId the stacks describe; a change re-seeds them
    double mHistoryCommitAt = 0.0;   // when to compare the chain again, 0 for no pending edit
    double mHistorySettleUntil = 0.0; // the engine's echo of a restore is not a new edit until then
    std::string mSelectedNode;
    bool mShowAdvanced = false;
    unsigned mDirty = kAll;
    bool mShowingStarting = false;
    std::string mAppliedTheme;
    std::uint32_t mChromeCaption = 0xffffffffu; // the caption colour last applied, none yet
    std::string mChainSignature;
    std::vector<std::pair<std::string, std::shared_ptr<NodeCard>>> mChainCards; // node id, card: for the lights
    std::shared_ptr<chain::HitMap> mHitMap; // where the chain's cards and cables are, for drops
    guitarfx::uiclient::PresetQuery mQuery;
    bool mShowSetlists = false;
    std::vector<std::string> mPresetTags; // the tag chips that are on
    std::string mSeenPresetFolder;        // the engine's folder choice last followed
    el::rect mFolderAnchor;               // the details dialog's folder button, for its drop-down
    std::shared_ptr<std::function<void()>> mDetailsTags; // the details dialog's tag chips, refilled in place
    bool mSetlistPads = false;             // the Setlists tab shows pads rather than a list
    int mSetlistPadBank = 0;
    std::shared_ptr<el::basic_input_box> mSearchBox; // kept across rebuilds, with what was typed
    Ptr mSearchElement;
    SlotPtr mPresetListSlot;
    std::shared_ptr<Text> mPresetCount;

    // The effect page's controls, kept to move their values without rebuilding.
    struct ParamControl
    {
        guitarfx::uiclient::EffectParamInfo info;
        std::shared_ptr<ParamKnob> knob;      // continuous values, and enums short enough to turn through
        std::shared_ptr<ParamToggle> toggle;  // on/off
        std::shared_ptr<IconButton> picker;   // an enum the engine asks to show as a list
    };

    std::string mEffectSignature;
    std::string mSceneSignature;
    std::string mTopSignature;
    std::vector<ParamControl> mControls;
    std::vector<std::function<void()>> mSettingsSyncs; // the settings page's controls, patched in place
    std::shared_ptr<PowerSwitch> mBypass;
    std::shared_ptr<HeaderLed> mHeaderLed;

    // The meters, and the labels beside them.
    std::shared_ptr<MeterBar> mInMeter, mOutMeter;
    std::shared_ptr<Text> mLoadText;

    Ptr mOverlay;
    Ptr mDropdown; // a drop-down list: a layer of its own, so one opened from a dialog stacks over it
    Ptr mToast;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    std::function<void()> mOnOverlayClosed;                       // runs when the overlay closes
    std::vector<guitarfx::uiclient::Subscription> mInstrumentSubs; // an open dialog's live feeds
    std::string mPickerCategory;
    std::optional<guitarfx::GraphEdge> mPickerEdge; // the open picker adds on this edge, not after a node
    guitarfx::uiclient::EffectPresentation mPresentation;

    // After adding an effect, the node that appears next is selected and shown.
    std::set<std::string> mNodesBeforeAdd;
    double mSelectAddedUntil = 0.0;

    std::vector<guitarfx::uiclient::Subscription> mSubscriptions;
};
} // namespace nanoq::ui
