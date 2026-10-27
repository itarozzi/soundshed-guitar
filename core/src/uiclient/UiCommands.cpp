/**
 * UiCommands.cpp - The native UI's requests to the engine.
 */

#include "uiclient/UiCommands.h"

#include "presets/PresetStorage.h"

#include <algorithm>
#include <set>

namespace guitarfx::uiclient
{
// ── Presets and scenes ─────────────────────────────────────────────────────

void UiCommands::LoadPreset(const std::string& presetId, const std::string& sceneId)
{
    if (presetId.empty())
    {
        return;
    }

    // By id: the engine reads the preset from its own store, so this UI never sends a
    // preset body it might have read differently.
    nlohmann::json payload = {{"presetId", presetId}};

    if (!sceneId.empty())
    {
        payload["sceneId"] = sceneId;
    }

    mClient.Send("loadPreset", std::move(payload));
}

void UiCommands::StepPreset(int delta)
{
    const auto& state = mClient.State();
    const auto& list = state.presetList;

    if (list.empty())
    {
        return;
    }

    const auto it = std::find_if(list.begin(), list.end(),
                                 [&state](const PresetSummary& p) { return p.id == state.activePresetId; });
    const auto size = static_cast<int>(list.size());
    int index = it == list.end() ? -1 : static_cast<int>(it - list.begin());

    if (delta < 0)
    {
        index = index <= 0 ? size - 1 : index - 1;
    }
    else
    {
        index = index < 0 || index >= size - 1 ? 0 : index + 1;
    }

    LoadPreset(list[static_cast<std::size_t>(index)].id);
}

void UiCommands::SelectScene(const std::string& sceneId)
{
    if (!sceneId.empty() && sceneId != mClient.State().activeSceneId)
    {
        mClient.Send("selectScene", {{"sceneId", sceneId}});
    }
}

void UiCommands::AddScene(const std::string& title)
{
    nlohmann::json payload = nlohmann::json::object();
    payload["fromSceneId"] = mClient.State().activeSceneId;

    if (!title.empty())
    {
        payload["title"] = title;
    }

    mClient.Send("addScene", std::move(payload));
}

void UiCommands::RenameScene(const std::string& sceneId, const std::string& title)
{
    mClient.Send("renameScene", {{"sceneId", sceneId}, {"title", title}});
}

void UiCommands::RemoveScene(const std::string& sceneId)
{
    mClient.Send("removeScene", {{"sceneId", sceneId}});
}

void UiCommands::SavePreset()
{
    const auto& state = mClient.State();

    if (!state.activePreset)
    {
        return;
    }

    // No preset body: the engine saves its own working copy.
    mClient.Send("savePreset", {{"saveMode", "overwrite"},
                                {"presetId", state.activePresetId},
                                {"name", state.activePreset->name},
                                {"category", state.activePreset->category},
                                {"description", state.activePreset->description},
                                {"includeGlobalSignalChain", false}});
}

void UiCommands::SavePresetAs(const std::string& name, const std::string& category, const std::string& description)
{
    mClient.Send("savePreset", {{"saveMode", "save-as"},
                                {"name", name},
                                {"category", category.empty() ? std::string("User") : category},
                                {"description", description},
                                {"includeGlobalSignalChain", false}});
}

void UiCommands::SavePresetWithDetails(const PresetDetails& details, bool asNew)
{
    const auto& state = mClient.State();

    if (!state.activePreset || details.name.empty())
    {
        return;
    }

    nlohmann::json payload = {{"saveMode", asNew ? "save-as" : "overwrite"},
                              {"name", details.name},
                              {"category", details.category.empty() ? std::string("User") : details.category},
                              {"description", details.description},
                              {"tags", details.tags},
                              {"includeGlobalSignalChain", false}};

    if (!asNew)
    {
        payload["presetId"] = state.activePresetId;
    }

    if (details.folderId)
    {
        payload["folderId"] = *details.folderId;
    }

    mClient.Send("savePreset", std::move(payload));
}

void UiCommands::RenamePreset(const std::string& name, const std::string& category, const std::string& description)
{
    const auto& state = mClient.State();

    if (!state.activePreset)
    {
        return;
    }

    mClient.Send("savePreset", {{"saveMode", "overwrite"},
                                {"presetId", state.activePresetId},
                                {"name", name},
                                {"category", category},
                                {"description", description},
                                {"includeGlobalSignalChain", false}});
}

void UiCommands::NewPreset()
{
    mClient.Send("newPreset");
}

void UiCommands::DeletePreset(const std::string& presetId)
{
    mClient.Send("deletePreset", {{"presetId", presetId}});
}

void UiCommands::SetPresetRating(const std::string& presetId, int rating)
{
    mClient.Send("setPresetRating", {{"presetId", presetId}, {"rating", std::clamp(rating, 0, 5)}});
}

void UiCommands::CreatePresetFolder(const std::string& name, const std::string& parentId)
{
    mClient.Send("createPresetFolder", {{"name", name}, {"parentId", parentId}});
}

void UiCommands::RenamePresetFolder(const std::string& folderId, const std::string& name)
{
    mClient.Send("renamePresetFolder", {{"folderId", folderId}, {"name", name}});
}

void UiCommands::MovePresetFolder(const std::string& folderId, const std::string& parentId)
{
    mClient.Send("movePresetFolder", {{"folderId", folderId}, {"parentId", parentId}});
}

void UiCommands::DeletePresetFolder(const std::string& folderId)
{
    mClient.Send("deletePresetFolder", {{"folderId", folderId}});
}

void UiCommands::MovePresetToFolder(const std::string& presetId, const std::string& folderId)
{
    mClient.Send("movePresetToFolder", {{"presetId", presetId}, {"folderId", folderId}});
}

void UiCommands::SelectPresetFolder(const std::string& folderId)
{
    auto& state = mClient.MutableState();
    state.activePresetFolderId = folderId.empty() ? "__all__" : folderId;
    mClient.Send("selectPresetFolder", {{"folderId", state.activePresetFolderId}});
}

void UiCommands::CreateSetlist(const std::string& name, int bank)
{
    mClient.Send("createSetlist", {{"name", name}, {"bank", bank >= 0 ? nlohmann::json(bank) : nlohmann::json()}});
}

void UiCommands::UpdateSetlist(const std::string& setlistId, const std::string& name, int bank)
{
    mClient.Send("updateSetlist",
                 {{"setlistId", setlistId}, {"name", name}, {"bank", bank >= 0 ? nlohmann::json(bank) : nlohmann::json()}});
}

void UiCommands::DeleteSetlist(const std::string& setlistId)
{
    mClient.Send("deleteSetlist", {{"setlistId", setlistId}});
}

void UiCommands::AddSetlistSlot(const std::string& setlistId, const std::string& presetId)
{
    mClient.Send("addSetlistSlot", {{"setlistId", setlistId}, {"presetId", presetId}});
}

void UiCommands::SetSetlistSlot(const std::string& setlistId, int index, const std::string& presetId)
{
    mClient.Send("setSetlistSlot", {{"setlistId", setlistId}, {"index", index}, {"presetId", presetId}});
}

void UiCommands::MoveSetlistSlot(const std::string& setlistId, int fromIndex, int toIndex)
{
    mClient.Send("moveSetlistSlot", {{"setlistId", setlistId}, {"fromIndex", fromIndex}, {"toIndex", toIndex}});
}

void UiCommands::RemoveSetlistSlot(const std::string& setlistId, int index)
{
    mClient.Send("removeSetlistSlot", {{"setlistId", setlistId}, {"index", index}});
}

void UiCommands::SetPresetFavorite(const std::string& presetId, bool favorite)
{
    mClient.Send("setPresetFavorite", {{"presetId", presetId}, {"favorite", favorite}});
}

// ── Multi-Rig ──────────────────────────────────────────────────────────

void UiCommands::FocusMixerPreset(const std::string& presetId)
{
    mClient.Send("focusMixerPreset", {{"presetId", presetId}});
}

void UiCommands::RemoveActivePreset(const std::string& presetId)
{
    mClient.Send("removeActivePreset", {{"presetId", presetId}});
}

// ── Setlists ───────────────────────────────────────────────────────────────

void UiCommands::SetSetlistCursor(int index)
{
    mClient.Send("setSetlistCursor", {{"cursorIndex", index}});
}

void UiCommands::SelectSetlist(const std::string& setlistId)
{
    mClient.Send("selectSetlist", {{"setlistId", setlistId}});
}

// ── Signal chain ───────────────────────────────────────────────────────────

void UiCommands::SetNodeBypassed(const std::string& nodeId, bool bypassed)
{
    mClient.Send("updateSignalPathNodeBypass", {{"nodeId", nodeId}, {"bypassed", bypassed}});
}

void UiCommands::SetNodeConfig(const std::string& nodeId, const std::string& key, const std::string& value)
{
    auto& state = mClient.MutableState();
    if (state.activePreset)
        if (auto* node = state.activePreset->graph.FindNode(nodeId))
            node->config[key] = value;

    mClient.Send("updateSignalPathNodeConfig", {{"nodeId", nodeId}, {"key", key}, {"value", value}, {"persist", true}, {"capture", false}});
}

void UiCommands::LoadActivePresetWithGraph(const SignalGraph& graph)
{
    const auto& state = mClient.State();
    if (!state.activePreset)
        return;

    Preset preset = *state.activePreset;
    preset.graph = graph;
    for (auto& scene : preset.scenes)
        if (scene.id == state.activeSceneId)
            scene.graph = graph;

    const auto body = nlohmann::json::parse(PresetStorage::SerializeToJson(preset), nullptr, false);
    if (body.is_discarded())
        return;
    nlohmann::json payload = {{"preset", body}};
    if (!state.activeSceneId.empty())
        payload["sceneId"] = state.activeSceneId;
    mClient.Send("loadPreset", payload);
}

void UiCommands::SetNodeParam(const std::string& nodeId, const std::string& key, double value)
{
    // Mirror the change at once, as the web UI's optimistic knob does; the engine does not
    // echo a param change it was asked for.
    auto& state = mClient.MutableState();

    if (state.activePreset)
    {
        if (auto* node = state.activePreset->graph.FindNode(nodeId))
        {
            node->params[key] = value;
        }
    }

    mClient.Send("updateSignalPathNodeParam", {{"nodeId", nodeId}, {"paramKey", key}, {"value", value}});
}

void UiCommands::AddNode(const std::string& effectType, const std::string& afterNodeId)
{
    mClient.Send("addSignalPathNode", {{"effectType", effectType}, {"insertAfter", afterNodeId}});
}

void UiCommands::AddNodeOnEdge(const std::string& effectType, const GraphEdge& edge)
{
    mClient.Send("addSignalPathNode",
                 {{"effectType", effectType},
                  {"edge", {{"from", edge.from}, {"to", edge.to}, {"fromPort", edge.fromPort}, {"toPort", edge.toPort}}}});
}

void UiCommands::RemoveNode(const std::string& nodeId)
{
    mClient.Send("deleteSignalPathNode", {{"nodeId", nodeId}});
}

void UiCommands::MoveNode(const std::string& nodeId, const std::string& afterNodeId)
{
    if (nodeId != afterNodeId)
    {
        mClient.Send("reorderSignalPathNode", {{"nodeId", nodeId}, {"targetNodeId", afterNodeId}});
    }
}

void UiCommands::MoveNodeToEdge(const std::string& nodeId, const GraphEdge& edge)
{
    mClient.Send("reorderSignalPathNode",
                 {{"nodeId", nodeId},
                  {"edge", {{"from", edge.from}, {"to", edge.to}, {"fromPort", edge.fromPort}, {"toPort", edge.toPort}}}});
}

void UiCommands::CollapseSplit(const std::string& splitterId)
{
    mClient.Send("collapseSignalPathSplit", {{"splitterId", splitterId}});
}

void UiCommands::SetSplitBranchCount(const std::string& splitterId, int count)
{
    mClient.Send("setSplitBranchCount", {{"splitterId", splitterId}, {"count", count}});
}

void UiCommands::ReplaceNode(const std::string& nodeId, const std::string& effectType)
{
    mClient.Send("replaceSignalPathNode", {{"nodeId", nodeId}, {"newEffectType", effectType}});
}

void UiCommands::SetNodeChannelMode(const std::string& nodeId, const std::string& channelMode)
{
    mClient.Send("setSignalPathNodeChannelMode", {{"nodeId", nodeId}, {"channelMode", channelMode}});
}

void UiCommands::DeleteEffectPreset(const std::string& effectType, const std::string& presetId)
{
    mClient.Send("deleteEffectPreset", {{"effectType", effectType}, {"presetId", presetId}});
}

void UiCommands::RenameNode(const std::string& nodeId, const std::string& title)
{
    mClient.Send("renameSignalPathNode", {{"nodeId", nodeId}, {"title", title}});
}

void UiCommands::SetNodeResource(const std::string& nodeId, const std::string& resourceType, const std::string& resourceId,
                                 int resourceIndex)
{
    // As the web UI's resource picker sends it: a library entry by id, with no file path (the
    // engine resolves it); an empty id and path clears the slot.
    nlohmann::json payload = {{"nodeId", nodeId}, {"resourceType", resourceType}, {"resourceId", resourceId}, {"filePath", ""}};

    if (resourceIndex >= 0)
    {
        payload["resourceIndex"] = resourceIndex;
    }

    // A third-party plugin takes a while to load; the engine says when it has.
    if (resourceType == "plugin" && !resourceId.empty())
    {
        mClient.MutableState().pluginLoadsPending.insert(nodeId);
    }

    mClient.Send("updateNodeResource", std::move(payload));
}

void UiCommands::SetNodeResourceFile(const std::string& nodeId, const std::string& resourceType, const std::string& filePath,
                                     int resourceIndex)
{
    nlohmann::json payload = {{"nodeId", nodeId}, {"resourceType", resourceType}, {"resourceId", ""}, {"filePath", filePath}};
    if (resourceIndex >= 0)
        payload["resourceIndex"] = resourceIndex;
    mClient.Send("updateNodeResource", std::move(payload));
}

std::string UiCommands::ImportRemoteResource(nlohmann::json payload)
{
    static int sequence = 0;
    auto requestId = payload.value("requestId", std::string());
    if (requestId.empty())
    {
        requestId = "nativeImport-" + std::to_string(++sequence);
        payload["requestId"] = requestId;
    }
    mClient.MutableState().resourceImports[requestId] = {ResourceImport::Status::Importing, payload.value("resourceType", ""),
                                                         payload.value("resourceId", ""), {}};
    mClient.Send("importRemoteResource", std::move(payload));
    return requestId;
}

void UiCommands::SaveLocalLibraryResource(const std::string& resourceType, const std::string& filePath, const std::string& name,
                                          const nlohmann::json& metadata, const std::string& nodeId, int resourceIndex)
{
    nlohmann::json payload = {{"resourceType", resourceType},
                              {"filePath", filePath},
                              {"name", name},
                              {"category", ""},
                              {"tags", nlohmann::json::array()},
                              {"description", ""},
                              {"metadata", metadata}};
    if (!nodeId.empty())
    {
        payload["nodeId"] = nodeId;
        if (resourceIndex >= 0)
            payload["resourceIndex"] = resourceIndex;
    }
    mClient.Send("saveLocalLibraryResource", std::move(payload));
}

void UiCommands::ListResourceFolder(const std::string& path)
{
    mClient.Send("listResourceFolder", {{"path", path}});
}

void UiCommands::BrowseResourceFolder()
{
    mClient.Send("browseResourceFolder");
}

void UiCommands::BrowseNodeResource(const std::string& nodeId, const std::string& resourceType, int resourceIndex)
{
    nlohmann::json payload = {{"nodeId", nodeId}, {"resourceType", resourceType}};

    if (resourceIndex >= 0)
    {
        payload["resourceIndex"] = resourceIndex;
    }

    if (resourceType == "plugin")
    {
        mClient.MutableState().pluginLoadsPending.insert(nodeId);
    }

    mClient.Send("browseNodeResource", std::move(payload));
}

void UiCommands::ApplyEffectPreset(const std::string& nodeId, const EffectPresetInfo& preset)
{
    // A saved preset, or one a factory archive ships, can carry a model, an IR, a blend, a plugin
    // and its state as well as parameters, and only the engine holds all of that, so it applies
    // those itself. Only the registry's own factory presets are parameters alone.
    if (preset.source != "factory")
    {
        const auto& state = mClient.State();
        const auto* node = state.activePreset ? state.activePreset->graph.FindNode(nodeId) : nullptr;

        if (node != nullptr)
        {
            mClient.Send("applyEffectPreset",
                         {{"nodeId", nodeId}, {"effectType", node->type}, {"presetId", preset.id}});
        }

        return;
    }

    // In the preset's own parameterOrder, then whatever it leaves out, as the web UI's
    // applyEffectPresetParams sends them: the order is the effect's to choose, not the map's.
    std::set<std::string> applied;

    for (const auto& key : preset.parameterOrder)
    {
        const auto value = preset.parameters.find(key);

        if (value != preset.parameters.end() && applied.insert(key).second)
        {
            SetNodeParam(nodeId, key, value->second);
        }
    }

    for (const auto& [key, value] : preset.parameters)
    {
        if (applied.count(key) == 0)
        {
            SetNodeParam(nodeId, key, value);
        }
    }
}

void UiCommands::SaveEffectPreset(const std::string& nodeId, const std::string& name)
{
    const auto& state = mClient.State();
    const auto* node = state.activePreset ? state.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr || name.empty())
    {
        return;
    }

    nlohmann::json parameters = nlohmann::json::object();

    for (const auto& [key, value] : node->params)
    {
        parameters[key] = value;
    }

    // The engine snapshots its own copy of the node (resources and config too); the
    // parameters are what it falls back on if it cannot find it.
    mClient.Send("saveEffectPreset",
                 {{"effectType", node->type}, {"nodeId", nodeId}, {"name", name}, {"parameters", parameters}});
}

// ── Global chain and output ───────────────────────────────────────────────

void UiCommands::SetGlobalChainParam(const std::string& path, double value)
{
    mClient.Send("setGlobalChainParam", {{"path", path}, {"value", value}});
}

void UiCommands::SetGlobalChainToggle(const std::string& path, bool enabled)
{
    mClient.Send("setGlobalChainParam", {{"path", path}, {"value", enabled}});
}

void UiCommands::SetOutputMuted(bool muted)
{
    mClient.Send("setOutputMuted", {{"muted", muted}});
}

void UiCommands::SetInputMode(bool monoMode, int inputChannel, bool dualMono)
{
    mClient.Send("setInputMode", {{"monoMode", monoMode}, {"inputChannel", inputChannel}, {"dualMono", dualMono}});
}

// ── Tuner, metronome, demo audio ─────────────────────────────────────────────

void UiCommands::SetTunerActive(bool active)
{
    mClient.Send("tuner", {{"action", active ? "start" : "stop"}});
}

void UiCommands::SetTunerLiveMode(bool liveMode)
{
    mClient.Send("tuner", {{"action", "setLiveMode"}, {"liveMode", liveMode}});
}

void UiCommands::SetTunerReference(double frequency)
{
    mClient.Send("tuner", {{"action", "setReference"}, {"referenceFrequency", frequency}});
}

void UiCommands::SetMetronomeEnabled(bool enabled)
{
    mClient.Send("setMetronome", {{"enabled", enabled}});
}

void UiCommands::SetMetronomeBpm(double bpm)
{
    mClient.Send("setMetronome", {{"bpm", std::clamp(bpm, 20.0, 300.0)}});
}

void UiCommands::SetMetronomeVolume(double volumeDb)
{
    mClient.Send("setMetronome", {{"volumeDb", volumeDb}});
}

void UiCommands::SetMetronomeClickType(const std::string& clickType)
{
    mClient.Send("setMetronome", {{"clickType", clickType}});
}

void UiCommands::SetMetronomeTimeSignature(int numerator, int denominator)
{
    mClient.Send("setMetronome", {{"timeSigNum", numerator}, {"timeSigDen", denominator}});
}

void UiCommands::SetMetronomeBeatPattern(const std::string& pattern)
{
    mClient.Send("setMetronome", {{"beatPattern", pattern}});
}

void UiCommands::SetMetronomeSubdivision(const std::string& subdivision)
{
    mClient.Send("setMetronome", {{"subdivision", subdivision}});
}

void UiCommands::PreviewDemoClip(const std::string& clipId, bool repeat)
{
    mClient.Send("previewDemoAudio", {{"clipId", clipId}, {"repeat", repeat}});
}

void UiCommands::PreviewRiff(const std::string& takeId)
{
    mClient.Send("previewRiffTake", {{"takeId", takeId}, {"enableGuidance", false}});
}

void UiCommands::StopDemo()
{
    mClient.Send("stopDemoAudio");
}

void UiCommands::RenderDemoClip(const std::string& clipId, int sampleRate)
{
    mClient.Send("renderDemoAudio", {{"clipId", clipId}, {"renderSampleRate", sampleRate}});
}

void UiCommands::RenderRiffTake(const std::string& takeId, int sampleRate)
{
    mClient.Send("renderDemoAudio", {{"takeId", takeId}, {"renderSampleRate", sampleRate}});
}

// ── Settings and device ──────────────────────────────────────────────────────

void UiCommands::SetSetting(const std::string& key, const nlohmann::json& value)
{
    auto& state = mClient.MutableState();
    state.appSettings[key] = value;
    mClient.Send("setSetting", {{"key", key}, {"value", value}});

    // The engine does not echo a setting back to the UI that set it, so the views that read
    // it (the UI scale, the demo clip) hear of it here.
    mClient.Notify(Topic::Session);
}

void UiCommands::SetTheme(const std::string& theme)
{
    auto& state = mClient.MutableState();
    state.theme = theme;
    state.appSettings["theme"] = theme;
    mClient.Send("setTheme", {{"theme", theme}});
    mClient.Notify(Topic::Session);
}

void UiCommands::PatchNativeUiSettings(const nlohmann::json& patch)
{
    auto& state = mClient.MutableState();

    if (!state.uiSettings.is_object())
    {
        state.uiSettings = nlohmann::json::object();
    }

    auto& native = state.uiSettings["native"];

    if (!native.is_object())
    {
        native = nlohmann::json::object();
    }

    native.merge_patch(patch);
    mClient.Send("uiSettingsChanged", {{"patch", {{"native", patch}}}});
}

void UiCommands::PatchUiSettings(const nlohmann::json& patch)
{
    auto& state = mClient.MutableState();

    if (!state.uiSettings.is_object())
    {
        state.uiSettings = nlohmann::json::object();
    }

    state.uiSettings.merge_patch(patch);
    mClient.Send("uiSettingsChanged", {{"patch", patch}});
}

void UiCommands::SetResourceFavorite(const std::string& resourceId, bool favorite)
{
    mClient.Send("setResourceFavorite", {{"resourceId", resourceId}, {"favorite", favorite}});
}

void UiCommands::UpdateLibraryResource(const std::string& resourceType, const std::string& resourceId, const std::string& name,
                                       const std::string& category, const std::vector<std::string>& tags)
{
    mClient.Send("updateLibraryResource",
                 {{"resourceType", resourceType}, {"resourceId", resourceId}, {"name", name}, {"category", category}, {"tags", tags}});
}

void UiCommands::DeleteLibraryResource(const std::string& resourceType, const std::string& resourceId)
{
    mClient.Send("deleteLibraryResource", {{"resourceType", resourceType}, {"resourceId", resourceId}});
}

void UiCommands::QueryResourceUsage(const std::string& resourceType, const std::string& resourceId)
{
    mClient.Send("queryResourceUsage", {{"resourceType", resourceType}, {"resourceId", resourceId}});
}

void UiCommands::PreviewRemoteResource(const std::string& resourceType, const std::string& tempResourceId, const std::string& nodeId,
                                       int resourceIndex, bool isZip, const std::string& base64)
{
    mClient.Send("previewRemoteResource", {{"resourceType", resourceType},
                                           {"tempResourceId", tempResourceId},
                                           {"nodeId", nodeId},
                                           {"resourceIndex", std::max(0, resourceIndex)},
                                           {"isZip", isZip},
                                           {"data", base64}});
}

void UiCommands::CancelPreviewResource(const std::string& nodeId, int resourceIndex, bool restoreOriginal)
{
    mClient.Send("cancelPreviewResource",
                 {{"nodeId", nodeId}, {"resourceIndex", std::max(0, resourceIndex)}, {"restoreOriginal", restoreOriginal}});
}

void UiCommands::AudioDevice(const std::string& action, nlohmann::json payload)
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["action"] = action;
    mClient.Send("audioDevice", std::move(payload));
}

void UiCommands::OpenUrl(const std::string& url)
{
    mClient.Send("openUrl", {{"url", url}});
}

// ── Tone sharing ─────────────────────────────────────────────────────────────

void UiCommands::InstallPresetArchives(const nlohmann::json& entry, const std::string& folder, nlohmann::json archives)
{
    const auto entryId = entry.value("id", std::string{});
    mClient.MutableState().toneInstalls[entryId] = ToneInstall{};
    mClient.Notify(Topic::Tones);

    nlohmann::json payload = {{"requestId", entryId}, {"entry", entry}, {"archives", std::move(archives)}};

    if (!folder.empty())
    {
        payload["folder"] = folder;
    }

    mClient.Send("installPresetArchives", std::move(payload));
}

void UiCommands::DeleteInstalledPresetArchive(const std::string& entryId)
{
    mClient.Send("deleteInstalledPresetArchive", {{"id", entryId}});
}
} // namespace guitarfx::uiclient
