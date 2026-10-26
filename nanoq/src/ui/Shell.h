#pragma once

// Shell - the frame every page sits in: the top bar, the scene strip, the chain strip, the
// page, and the transport bar. Soundshed Nano's layout, drawn with Elements.
//
// The UI is rebuilt from the client's state, region by region, rather than patched: what a
// region shows is a pure function of ClientState, so a change just marks the regions it
// touches and the next tick (on the UI thread, between events) rebuilds them. Two things are
// updated in place instead, because rebuilding them would be visible: the meters (20 Hz) and
// the values on the sliders of the page being shown (a rebuild would drop a drag).

#include "ui/Graphics.h"
#include "ui/Layout.h"
#include "ui/UiSession.h"
#include "ui/Widgets.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/PresetBrowse.h"

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
    Effect,
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
        kAll = kTopBar | kScenes | kChain | kPage | kTransport
    };

    void Mark(unsigned bits)
    {
        mDirty |= bits;
    }

    void Subscribe();
    void BuildRoot();
    void Rebuild();

    // Shell.cpp
    Ptr BuildTopBar();
    Ptr BuildScenes();
    Ptr BuildTransport();
    Ptr BuildStarting();
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

    [[nodiscard]] std::string SceneSignature() const;

    // EffectPage.cpp
    [[nodiscard]] std::string EffectPageSignature() const;
    Ptr BuildEffectPage();
    void SyncEffectValues();
    void OpenEffectPresets();
    void OpenResourcePicker(const std::string& nodeId, const std::string& resourceType, int resourceIndex);
    void OpenEnumPicker(const std::string& nodeId, const guitarfx::uiclient::EffectParamInfo& info);
    [[nodiscard]] Ptr BuildResourceSlot(const guitarfx::GraphNode& node, const std::string& resourceType, int resourceIndex,
                                        const std::string& label, bool optional);
    [[nodiscard]] Ptr BuildSplitBranches(const guitarfx::GraphNode& splitter);

    // PresetsPage.cpp
    Ptr BuildPresetsPage();
    Ptr BuildPresetListContent();
    void RebuildPresetList();
    void LoadWithConfirm(const std::string& presetId);
    void DeleteWithConfirm(const std::string& presetId);
    void OpenPresetRowMenu(const guitarfx::uiclient::PresetSummary& preset);

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
    void OpenTuner();
    void OpenMetronome();
    void OpenControls();

    // Menus.cpp
    void OpenPresetMenu();
    void OpenNodeMenu(const std::string& nodeId);
    void RenameNodeWithPrompt(const std::string& nodeId);
    void SaveActivePreset(bool asNew);
    [[nodiscard]] std::string MoveTarget(const std::string& nodeId, int direction) const;
    [[nodiscard]] std::string LastNodeBeforeOutput() const;

    UiSession& mSession;
    guitarfx::uiclient::ClientState& mState;
    Theme& mTheme;
    Icons mIcons;
    Images mImages;

    Ptr mRoot;
    SlotPtr mBgSlot, mTopSlot, mSceneSlot, mChainSlot, mPageSlot, mTransportSlot;

    Page mPage = Page::Effect;
    std::string mSelectedNode;
    bool mShowAdvanced = false;
    unsigned mDirty = kAll;
    bool mShowingStarting = false;
    std::string mAppliedTheme;
    std::string mChainSignature;
    std::vector<std::pair<std::string, std::shared_ptr<NodeCard>>> mChainCards; // node id, card: for the lights
    guitarfx::uiclient::PresetQuery mQuery;
    bool mShowSetlists = false;
    std::shared_ptr<el::basic_input_box> mSearchBox; // kept across rebuilds, with what was typed
    Ptr mSearchElement;
    SlotPtr mPresetListSlot;
    std::shared_ptr<Text> mPresetCount;

    // The effect page's controls, kept to move their values without rebuilding.
    struct ParamControl
    {
        guitarfx::uiclient::EffectParamInfo info;
        std::shared_ptr<ParamKnob> knob;
        std::shared_ptr<Button> toggle; // toggles and enums
    };

    std::string mEffectSignature;
    std::string mSceneSignature;
    std::vector<ParamControl> mControls;
    std::vector<std::function<void()>> mSettingsSyncs; // the settings page's controls, patched in place
    std::shared_ptr<Button> mBypass;

    // The meters, and the labels beside them.
    std::shared_ptr<MeterBar> mInMeter, mOutMeter;
    std::shared_ptr<Text> mLoadText;

    Ptr mOverlay;
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
