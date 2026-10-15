/**
 * PluginControllerDiagnostics.cpp - Telling someone what the app was doing.
 *
 * Three separate feeds, none of them on the audio path:
 *   - the session log, an append-only text file next to the settings;
 *   - the debug snapshot, a one-shot dump of UI state plus backend state,
 *     scrubbed of anything sensitive and written to disk on request;
 *   - the live signal-diagnostics and performance meters, the spectrum
 *     behind an EQ curve and the loudness in a node's DSP status readout,
 *     which are only forwarded from here — TelemetryPublisher owns their
 *     rate limits.
 */

#include "PluginController.h"

#include <array>
#include <optional>

#include "controller/TelemetryPublisher.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/HostedPluginSupport.h"
#include "util/PathEncoding.h"
#include "util/SessionLog.h"

using namespace guitarfx::controller_detail;

namespace guitarfx
{
void PluginController::AppendSessionLog(const std::string& message) const
{
    util::AppendSessionLog(GetEffectiveSettingsDirectory(), message);
}

void PluginController::HandleCaptureDebugSnapshotRequest(const nlohmann::json& payload)
{
    const std::string source = payload.value("source", "manual");

    if (!mUIReady)
    {
        HandleDebugReportUiStateRequest(nlohmann::json{
            {"source", source + ":backend-only"},
        });
        return;
    }

    SendMessageToUI(nlohmann::json{
        {"type", "captureDebugSnapshot"},
        {"source", source},
    }
                        .dump());
}

void PluginController::HandleDebugReportUiStateRequest(const nlohmann::json& payload)
{
    try
    {
        const std::string source = payload.value("source", "ui-auto");
        nlohmann::json snapshot = nlohmann::json::object();
        snapshot["type"] = "debugSnapshot";
        snapshot["capturedAt"] = FormatTimestamp();
        snapshot["source"] = source;
        snapshot["paths"] = {
            {"sessionLog", util::PathToUtf8(util::SessionLogPath(mFileSystem.ResolveSettingsDirectory()))},
            {"snapshot", util::PathToUtf8(ResolveDebugSnapshotPath(mFileSystem))},
        };
        snapshot["session"] = {
            {"activePresetId", mActivePresetId},
            {"activeSceneId", GetResolvedActiveSceneId()},
            {"uiReady", mUIReady},
            {"pendingStateBroadcast", mPendingStateBroadcast},
            {"activePresetIds", SnapshotActivePresetIds()},
        };

        if (payload.contains("snapshot"))
        {
            snapshot["ui"] = payload["snapshot"];
            ScrubSensitiveJson(snapshot["ui"]);
        }

        nlohmann::json backendState = nlohmann::json::parse(SerializeState());
        ScrubSensitiveJson(backendState);
        snapshot["backend"] = std::move(backendState);

        if (mActivePreset)
        {
            snapshot["activePresetSummary"] = SummarizeHostedPluginState(*mActivePreset);
        }

        const auto snapshotPath = ResolveDebugSnapshotPath(mFileSystem);
        SaveJsonFile(mFileSystem, snapshotPath, snapshot);

        SendMessageToUI(nlohmann::json{
            {"type", "debugSnapshotWritten"},
            {"path", util::PathToUtf8(snapshotPath)},
            {"source", source},
        }
                            .dump());
    }
    catch (const std::exception& e)
    {
        AppendSessionLog("Debug snapshot write failed: " + std::string{e.what()});
    }
}

void PluginController::HandleGetSignalDiagnosticsRequest()
{
    // A pull for tests and scripted debugging; the UI takes the pushed feed. Whoever asks
    // may hold no roster to resolve frames against, so re-send it alongside the next frame.
    mTelemetry->MarkRosterDirty();
    mTelemetry->RequestSignalDiagnostics();
}

void PluginController::HandleGetPerformanceStatsRequest()
{
    mTelemetry->RequestPerformanceStats();
}

void PluginController::HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload)
{
    // No preference to store: diagnostics follow UI visibility (see "uiVisibility"). The
    // message survives as the UI's request for a fresh node roster, which implies it is up.
    (void)payload;
    mPresetMixer.SetSignalDiagnosticsEnabled(mTelemetry->IsUiVisible());
    mTelemetry->MarkRosterDirty();
}

namespace
{
/// The node a tap watch message names, as {scope, presetId, nodeId}, or nothing when it names
/// none: no node, or a scope we do not know, is how the UI says it has stopped looking. A preset
/// node is looked up the way a parameter edit to it is: in the payload's preset, or else the
/// active one.
std::optional<std::array<std::string, 3>> ParseTapWatch(const nlohmann::json& payload,
                                                        const std::string& activePresetId)
{
    const std::string nodeId = payload.value("nodeId", std::string{});
    const std::string scope = payload.value("scope", std::string{});

    if (nodeId.empty() || (scope != "pre" && scope != "post" && scope != "preset"))
    {
        return std::nullopt;
    }

    std::string presetId;

    if (scope == "preset")
    {
        presetId = payload.value("presetId", std::string{});

        if (presetId.empty())
        {
            presetId = activePresetId;
        }
    }

    return std::array<std::string, 3>{scope, presetId, nodeId};
}
} // namespace

void PluginController::HandleSetSpectrumWatchRequest(const nlohmann::json& payload)
{
    if (const auto watch = ParseTapWatch(payload, mActivePresetId))
    {
        mTelemetry->WatchSpectrum((*watch)[0], (*watch)[1], (*watch)[2]);
    }
    else
    {
        mTelemetry->StopSpectrum();
    }
}

void PluginController::HandleSetLoudnessWatchRequest(const nlohmann::json& payload)
{
    if (const auto watch = ParseTapWatch(payload, mActivePresetId))
    {
        mTelemetry->WatchLoudness((*watch)[0], (*watch)[1], (*watch)[2]);
    }
    else
    {
        mTelemetry->StopLoudness();
    }
}
} // namespace guitarfx
