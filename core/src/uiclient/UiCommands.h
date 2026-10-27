#pragma once

/**
 * UiCommands.h - Everything the native UI asks the engine to do.
 *
 * One method per user intent, each sending the same message the WebView UI sends for it.
 * Anything that changes a persisted document goes through an engine-owned command
 * (selectScene, addScene, newPreset, setPresetFavorite, setOutputMuted, ...), never by
 * editing a copy here and sending it back whole (docs/plans/native-ui.md, rule C3).
 */

#include "uiclient/UiClient.h"

#include <optional>
#include <string>

namespace guitarfx::uiclient
{
class UiCommands
{
public:
    explicit UiCommands(UiClient& client) : mClient(client)
    {
    }

    // ── Presets and scenes ──────────────────────────────────────────────────
    void LoadPreset(const std::string& presetId, const std::string& sceneId = {});
    /// Steps through the library in the engine's order, wrapping, as the web UI's
    /// previous/next buttons do. `delta` is +1 or -1.
    void StepPreset(int delta);
    void SelectScene(const std::string& sceneId);
    void AddScene(const std::string& title = {});
    void RenameScene(const std::string& sceneId, const std::string& title);
    void RemoveScene(const std::string& sceneId);
    void SavePreset();
    void SavePresetAs(const std::string& name, const std::string& category, const std::string& description);
    void RenamePreset(const std::string& name, const std::string& category, const std::string& description);
    void NewPreset();
    void DeletePreset(const std::string& presetId);
    void SetPresetFavorite(const std::string& presetId, bool favorite);
    /// 1 to 5 stars, or 0 to clear.
    void SetPresetRating(const std::string& presetId, int rating);

    // ── Preset folders, edited by the engine; each is answered by presetFolders ──
    /// A new folder, at the top or inside `parentId`; it becomes the folder shown.
    void CreatePresetFolder(const std::string& name, const std::string& parentId = {});
    void RenamePresetFolder(const std::string& folderId, const std::string& name);
    /// Into `parentId`, or to the top level with an empty one; never into itself.
    void MovePresetFolder(const std::string& folderId, const std::string& parentId);
    /// The folder and its subfolders go; their presets stay in the library.
    void DeletePresetFolder(const std::string& folderId);
    /// A preset into a folder (out of any other); an empty folder takes it out of all.
    void MovePresetToFolder(const std::string& presetId, const std::string& folderId);
    /// The folder the preset list shows (a real one, or __all__, __favorites__, __recents__).
    void SelectPresetFolder(const std::string& folderId);
    // ── Setlists, edited by the engine; each is answered by setlists and setlistCursorChanged ──
    /// A new setlist (with a bank number, or -1 for none); it becomes the active one.
    void CreateSetlist(const std::string& name, int bank = -1);
    /// A setlist's name and bank (-1 for none); an empty id is the active setlist.
    void UpdateSetlist(const std::string& setlistId, const std::string& name, int bank);
    void DeleteSetlist(const std::string& setlistId);
    /// The slot commands work on the active setlist when `setlistId` is empty.
    void AddSetlistSlot(const std::string& setlistId, const std::string& presetId);
    /// Puts a preset in a slot (a pad), padding with empty slots; an empty id clears it.
    void SetSetlistSlot(const std::string& setlistId, int index, const std::string& presetId);
    void MoveSetlistSlot(const std::string& setlistId, int fromIndex, int toIndex);
    void RemoveSetlistSlot(const std::string& setlistId, int index);

    // ── Multi-Rig (awareness only: rigs are built in Soundshed Guitar) ──────
    void FocusMixerPreset(const std::string& presetId);
    void RemoveActivePreset(const std::string& presetId);

    // ── Setlists ────────────────────────────────────────────────────────────
    void SetSetlistCursor(int index);
    void SelectSetlist(const std::string& setlistId);

    // ── Signal chain ────────────────────────────────────────────────────────
    void SetNodeBypassed(const std::string& nodeId, bool bypassed);
    void SetNodeParam(const std::string& nodeId, const std::string& key, double value);
    /// A node's string config (updateSignalPathNodeConfig), mirrored at once as SetNodeParam is.
    void SetNodeConfig(const std::string& nodeId, const std::string& key, const std::string& value);
    /// Loads the active preset with `graph` as its chain, in its scene and its own: what the
    /// chain's undo does when the change it reverses rebuilt the chain (history.ts,
    /// applyWholeGraph). The engine compares the result with the preset as saved for dirty.
    void LoadActivePresetWithGraph(const SignalGraph& graph);
    void AddNode(const std::string& effectType, const std::string& afterNodeId);
    /// Adds an effect on one edge of the graph: the way into a parallel branch, which "after a
    /// node" cannot name.
    void AddNodeOnEdge(const std::string& effectType, const GraphEdge& edge);
    void RemoveNode(const std::string& nodeId);
    void MoveNode(const std::string& nodeId, const std::string& afterNodeId);
    /// Moves a node onto an edge of the graph: a cable, which may be inside a parallel branch.
    void MoveNodeToEdge(const std::string& nodeId, const GraphEdge& edge);
    void CollapseSplit(const std::string& splitterId);
    /// Gives a split 2 to 4 branches; the engine adds empty ones and removes only empty ones.
    void SetSplitBranchCount(const std::string& splitterId, int count);
    /// Puts another effect in the node's place, as the web UI's FX library replace does.
    void ReplaceNode(const std::string& nodeId, const std::string& effectType);
    /// The node's own name; empty hands it back its automatic one.
    void RenameNode(const std::string& nodeId, const std::string& title);
    /// Which channels a node takes: "" (follow what arrives), "mono" (summed), "monoLeft" or
    /// "monoRight".
    void SetNodeChannelMode(const std::string& nodeId, const std::string& channelMode);
    /// Loads a library resource (a NAM model, an IR) into one of the node's slots. An empty
    /// `resourceId` clears the slot. `resourceIndex` -1 is the node's only (first) slot.
    void SetNodeResource(const std::string& nodeId, const std::string& resourceType, const std::string& resourceId,
                         int resourceIndex = -1);
    /// Asks the engine to show a file dialog and load the chosen file into the node's slot,
    /// adding it to the library.
    void BrowseNodeResource(const std::string& nodeId, const std::string& resourceType, int resourceIndex = -1);
    /// Loads a file into a node's slot by path, as the resource browser's Folder tab previews
    /// one, without adding it to the library.
    void SetNodeResourceFile(const std::string& nodeId, const std::string& resourceType, const std::string& filePath,
                             int resourceIndex = -1);

    // ── The resource library ────────────────────────────────────────────────
    /// Stores downloaded bytes in the library (importRemoteResource: provider, resourceType,
    /// resourceId, name, category, subfolder, fileName, metadata, data as base64). Returns the
    /// request id the answer comes back under (ClientState::resourceImports).
    std::string ImportRemoteResource(nlohmann::json payload);
    /// Adds a file where it is to the library (saveLocalLibraryResource), so it is referenced in
    /// place rather than copied; with a node, loads it into that node's slot too.
    void SaveLocalLibraryResource(const std::string& resourceType, const std::string& filePath, const std::string& name,
                                  const nlohmann::json& metadata, const std::string& nodeId = {}, int resourceIndex = -1);
    /// Lists a folder's model and IR files (resourceFolderListing, then resourceFolderMetadata).
    void ListResourceFolder(const std::string& path);
    /// Asks the engine for a folder dialog; the folder picked comes back as resourceFolderPicked.
    void BrowseResourceFolder();
    /// A factory preset is sent as parameter changes; a saved one is applied by the engine,
    /// which restores its resources and config as well.
    void ApplyEffectPreset(const std::string& nodeId, const EffectPresetInfo& preset);
    /// Saves a node's current settings as a user preset for its effect type.
    void SaveEffectPreset(const std::string& nodeId, const std::string& name);
    /// Deletes one of the user's own presets of an effect type.
    void DeleteEffectPreset(const std::string& effectType, const std::string& presetId);

    // ── Global chain and output ─────────────────────────────────────────────
    void SetGlobalChainParam(const std::string& path, double value);
    void SetGlobalChainToggle(const std::string& path, bool enabled);
    void SetOutputMuted(bool muted);
    /// inputChannel: which input mono takes, 0 input 1, 1 input 2, 2 both summed. dualMono: with
    /// mono off, run a stereo input as two mono chains.
    void SetInputMode(bool monoMode, int inputChannel, bool dualMono = false);

    // ── Tuner, metronome, demo audio ────────────────────────────────────────
    void SetTunerActive(bool active);
    void SetTunerLiveMode(bool liveMode);
    void SetTunerReference(double frequency);
    void SetMetronomeEnabled(bool enabled);
    void SetMetronomeBpm(double bpm);
    void SetMetronomeVolume(double volumeDb);
    void SetMetronomeClickType(const std::string& clickType);
    void SetMetronomeTimeSignature(int numerator, int denominator);
    void SetMetronomeBeatPattern(const std::string& pattern);
    void SetMetronomeSubdivision(const std::string& subdivision);
    void PreviewDemoClip(const std::string& clipId, bool repeat);
    void PreviewRiff(const std::string& takeId);
    void StopDemo();
    void RenderDemoClip(const std::string& clipId, int sampleRate);
    void RenderRiffTake(const std::string& takeId, int sampleRate);

    // ── Settings and device ─────────────────────────────────────────────────
    void SetSetting(const std::string& key, const nlohmann::json& value);
    void SetTheme(const std::string& theme);
    /// Merges into the "native" part of the UI settings, leaving the web UI's own alone.
    void PatchNativeUiSettings(const nlohmann::json& patch);
    /// Merges `patch` into the shared UI settings, at the top level, for what every UI reads
    /// (the web UI's own keys, such as the effect chooser's fxFavorites). An array replaces
    /// the stored one whole.
    void PatchUiSettings(const nlohmann::json& patch);
    void AudioDevice(const std::string& action, nlohmann::json payload = nlohmann::json::object());
    void OpenUrl(const std::string& url);

    // ── Tone sharing ────────────────────────────────────────────────────────────
    /// Installs downloaded tone sharing archives as one entry of the installedPacks setting.
    /// `entry` is {id, title, source, packId?}; each archive is {title, data: base64 zip}; a
    /// pack's presets go in a folder named `folder`. Answered by presetArchivesInstalled or
    /// presetArchivesInstallFailed.
    void InstallPresetArchives(const nlohmann::json& entry, const std::string& folder, nlohmann::json archives);
    /// Takes an install out again: its presets, folder, and the models only it used.
    void DeleteInstalledPresetArchive(const std::string& entryId);

private:
    UiClient& mClient;
};
} // namespace guitarfx::uiclient
