/**
 * UiClientLooperHandlers.cpp - The looper (Topic::Looper): the engine's state, layers, waveforms
 * and projects, and the commands that drive it, as the web UI's looper.ts and
 * messages/looperHandlers.ts use them.
 */

#include "uiclient/FeatureFlags.h"
#include "uiclient/Looper.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <chrono>

namespace guitarfx::uiclient
{
namespace
{
std::vector<float> PeaksOf(const nlohmann::json& m, const char* key)
{
    std::vector<float> peaks;
    if (const auto it = m.find(key); it != m.end() && it->is_array())
    {
        peaks.reserve(it->size());
        for (const auto& v : *it)
            peaks.push_back(v.is_number() ? static_cast<float>(v.get<double>()) : 0.0f);
    }
    return peaks;
}

LooperSettings ParseSettings(const nlohmann::json& m)
{
    LooperSettings s;
    s.enabled = m.value("enabled", s.enabled);
    s.maxSeconds = m.value("maxSeconds", s.maxSeconds);
    s.start = m.value("start", s.start);
    s.length = m.value("length", s.length);
    s.bars = m.value("bars", s.bars);
    s.countInBars = m.value("countInBars", s.countInBars);
    s.clickWhileRecording = m.value("clickWhileRecording", s.clickWhileRecording);
    s.afterRecord = m.value("afterRecord", s.afterRecord);
    s.alignOffsetMs = m.value("alignOffsetMs", s.alignOffsetMs);
    return s;
}
} // namespace

double LooperClockNow()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void UiClient::RegisterLooperHandlers()
{
    // The whole state each time; while it runs, the position too, a few times a second.
    On("looperState", [this](const nlohmann::json& m) {
        auto& l = mState.looper;
        l.known = true;
        l.available = m.value("available", false);
        l.state = m.value("state", std::string("empty"));
        l.playing = m.value("playing", false);
        l.positionSec = m.value("positionSec", 0.0);
        l.positionAt = LooperClockNow();
        l.lengthSec = m.value("lengthSec", 0.0);
        l.canUndo = m.value("canUndo", false);
        l.canRedo = m.value("canRedo", false);
        l.layerCount = m.value("layerCount", 0);
        l.memoryMb = m.value("memoryMb", 0.0);
        l.tempoLocked = m.value("tempoLocked", false);
        l.click = m.value("click", false);
        l.clickBar = m.value("clickBar", 0.0);
        l.bars = m.value("bars", 0);
        l.barIndex = m.value("barIndex", 0);
        l.tempoBpm = m.value("tempoBpm", 0.0);
        l.countInBeatsLeft = m.value("countInBeatsLeft", 0);
        l.recordedSec = m.value("recordedSec", 0.0);
        if (m.contains("settings") && m["settings"].is_object())
            l.settings = ParseSettings(m["settings"]);
        l.projectId = m.value("projectId", std::string{});
        l.projectTitle = m.value("projectTitle", std::string{});
        l.dirty = m.value("dirty", false);
        l.recoverable = m.value("recoverable", false);

        // An emptied loop has no waveform left to show, and only a take in progress has a live one.
        if (l.state == "empty" && l.lengthSec <= 0.0)
        {
            for (auto& layer : l.layers)
            {
                layer.peaksL.clear();
                layer.peaksR.clear();
            }
        }
        if (l.state != "recording" && l.state != "overdubbing")
            l.livePeaks.clear();

        // A notice comes once.
        const auto notice = m.value("notice", std::string{});
        if (const auto text = LooperNoticeText(notice); !text.empty())
        {
            PostNotification({notice == "exported" ? Notification::Kind::Info : Notification::Kind::Error, text,
                              m.value("noticeDetail", std::string{})});
        }
        SyncLooperEnabled();
        Notify(Topic::Looper);
    });

    // The layers, newest first, the one being recorded among them. A layer's peaks arrive apart
    // (looperWaveforms) and are kept across this list's updates.
    On("looperLayers", [this](const nlohmann::json& m) {
        auto& l = mState.looper;
        std::vector<LooperLayer> layers;
        if (const auto list = m.find("layers"); list != m.end() && list->is_array())
        {
            for (const auto& item : *list)
            {
                if (!item.is_object())
                    continue;
                LooperLayer layer;
                layer.id = item.value("id", std::string{});
                for (const auto& old : l.layers)
                {
                    if (old.id == layer.id)
                    {
                        layer.peaksL = old.peaksL;
                        layer.peaksR = old.peaksR;
                    }
                }
                layer.title = item.value("title", std::string{});
                layer.order = item.value("order", 0);
                layer.recording = item.value("recording", false);
                layer.level = item.value("level", 1.0);
                layer.balance = item.value("balance", 0.0);
                layer.muted = item.value("muted", false);
                layer.solo = item.value("solo", false);
                if (const auto tone = item.find("tone"); tone != item.end() && tone->is_object())
                    layer.presetName = tone->value("presetName", std::string{});
                layer.toneChanged = item.value("toneChanged", false);
                layer.canRevert = item.value("canRevert", false);
                const auto retone = item.find("retone");
                layer.retoneProgress = retone != item.end() && retone->is_object() ? retone->value("progress", 0.0) : -1.0;
                layers.push_back(std::move(layer));
            }
        }
        l.layers = std::move(layers);
        Notify(Topic::Looper);
    });

    // Finished layers' peaks (merged: one message may carry only some), and the take in progress.
    On("looperWaveforms", [this](const nlohmann::json& m) {
        auto& l = mState.looper;
        if (const auto layers = m.find("layers"); layers != m.end() && layers->is_object())
        {
            for (auto& layer : l.layers)
            {
                const auto it = layers->find(layer.id);
                if (it == layers->end() || !it->is_object())
                    continue;
                layer.peaksL = PeaksOf(*it, "peaksL");
                layer.peaksR = PeaksOf(*it, "peaksR");
            }
        }
        if (const auto live = m.find("live"); live != m.end() && live->is_object())
            l.livePeaks = PeaksOf(*live, "peaks");
        Notify(Topic::Looper);
    });

    On("looperProjects", [this](const nlohmann::json& m) {
        auto& l = mState.looper;
        l.projects.clear();
        if (const auto list = m.find("projects"); list != m.end() && list->is_array())
        {
            for (const auto& item : *list)
            {
                if (!item.is_object())
                    continue;
                LooperProject p;
                p.id = item.value("id", std::string{});
                p.title = item.value("title", std::string{});
                p.modified = item.value("modified", std::string{});
                p.lengthSec = item.value("lengthSec", 0.0);
                p.layerCount = item.value("layerCount", 0);
                p.tempoBpm = item.value("tempoBpm", 0.0);
                p.bars = item.value("bars", 0);
                l.projects.push_back(std::move(p));
            }
        }
        Notify(Topic::Looper);
    });

    On("looperProjectSaved", [this](const nlohmann::json& m) {
        PostNotification({Notification::Kind::Info, "Saved \"" + m.value("title", std::string{}) + "\" in the riff library", ""});
    });
}

void UiClient::SyncLooperEnabled()
{
    const auto& l = mState.looper;
    if (!l.known || !l.available)
        return;
    const bool on = IsFeatureEnabled(mState, Feature::Looper);
    if (l.settings.enabled == on)
    {
        mLooperEnabledPending = false;
        return;
    }
    // Once per change of mind, until the engine's state agrees.
    if (mLooperEnabledPending && mLooperEnabledSent == on)
        return;
    mLooperEnabledPending = true;
    mLooperEnabledSent = on;
    Send("setLooper", {{"enabled", on}});
}

// ── Commands ────────────────────────────────────────────────────────────────────

void UiCommands::LooperCommand(const std::string& command)
{
    mClient.Send("looperCommand", {{"command", command}});
}

void UiCommands::SetLooper(const nlohmann::json& patch)
{
    mClient.Send("setLooper", patch.is_object() ? patch : nlohmann::json::object());
}

void UiCommands::SetLooperLayer(const std::string& layerId, const nlohmann::json& patch)
{
    nlohmann::json payload = patch.is_object() ? patch : nlohmann::json::object();
    payload["layerId"] = layerId;
    mClient.Send("setLooperLayer", payload);
}

void UiCommands::RemoveLooperLayer(const std::string& layerId)
{
    mClient.Send("removeLooperLayer", {{"layerId", layerId}});
}

void UiCommands::RetoneLooperLayers(const std::vector<std::string>& layerIds, bool revert)
{
    nlohmann::json payload = {{"layerIds", layerIds.empty() ? nlohmann::json("all") : nlohmann::json(layerIds)}};
    if (revert)
        payload["revert"] = true;
    mClient.Send("retoneLooperLayer", payload);
}

void UiCommands::ExportLooperLayer(const std::string& layerId, const std::string& as)
{
    mClient.Send("exportLooperLayer", {{"layerId", layerId}, {"as", as}});
}

void UiCommands::GetLooperState()
{
    mClient.Send("getLooperState");
}

void UiCommands::GetLooperProjects()
{
    mClient.Send("getLooperProjects");
}

void UiCommands::SaveLooperProject(const std::string& title)
{
    mClient.Send("saveLooperProject", {{"title", title}});
}

void UiCommands::LoadLooperProject(const std::string& projectId)
{
    mClient.Send("loadLooperProject", {{"projectId", projectId}});
}

void UiCommands::DeleteLooperProject(const std::string& projectId)
{
    mClient.Send("deleteLooperProject", {{"projectId", projectId}});
}

void UiCommands::RecoverLooperSession()
{
    mClient.Send("recoverLooperSession");
}

void UiCommands::DismissLooperRecovery()
{
    mClient.Send("dismissLooperRecovery");
}

void UiCommands::AddRiffAsLooperLayer(const std::string& takeId)
{
    mClient.Send("addRiffAsLooperLayer", {{"takeId", takeId}});
}
} // namespace guitarfx::uiclient
