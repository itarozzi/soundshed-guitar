/**
 * UiClientPracticeToolHandlers.cpp - The Practice Tool's backing track (Topic::PracticeTool):
 * what the engine reports of it, and the commands that drive it, as the web UI's
 * messages/captureHandlers.ts and bridge.ts use them.
 */

#include "uiclient/Looper.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <algorithm>
#include <cmath>

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
        {
            if (v.is_number())
            {
                peaks.push_back(static_cast<float>(v.get<double>()));
            }
        }
    }

    return peaks;
}

double FiniteOr(const nlohmann::json& m, const char* key, double fallback)
{
    const auto it = m.find(key);

    if (it == m.end() || !it->is_number())
    {
        return fallback;
    }

    const double v = it->get<double>();
    return std::isfinite(v) ? v : fallback;
}
} // namespace

void UiClient::RegisterPracticeToolHandlers()
{
    On("practiceToolFileLoaded", [this](const nlohmann::json& m) {
        auto& t = mState.practiceTool;
        t.filePath = m.value("path", std::string{});
        t.title = m.value("title", std::string{});

        if (t.title.find_first_not_of(" \t") == std::string::npos)
        {
            const auto slash = t.filePath.find_last_of("\\/");
            t.title = slash == std::string::npos ? t.filePath : t.filePath.substr(slash + 1);
        }

        t.durationSec = std::max(0.0, FiniteOr(m, "durationSec", 0.0));
        t.peaksL = PeaksOf(m, "waveformPeaksL");
        t.peaksR = PeaksOf(m, "waveformPeaksR");
        t.playing = false;
        t.positionSec = 0.0;
        t.positionAt = LooperClockNow();
        ++t.loads;
        Notify(Topic::PracticeTool);
    });

    // On every transport change, and a few times a second while it plays.
    On("practiceToolTransportState", [this](const nlohmann::json& m) {
        auto& t = mState.practiceTool;
        t.playing = m.value("state", std::string{}) == "playing";
        t.positionSec = std::max(0.0, FiniteOr(m, "positionSec", t.positionSec));
        t.positionAt = LooperClockNow();
        Notify(Topic::PracticeTool);
    });

    On("practiceToolPlaybackEnded", [this](const nlohmann::json&) {
        auto& t = mState.practiceTool;
        t.playing = false;
        t.positionSec = 0.0;
        t.positionAt = LooperClockNow();
        ++t.endings;
        Notify(Topic::PracticeTool);
    });
}

void UiCommands::BrowsePracticeToolFile()
{
    mClient.Send("browsePracticeToolFile");
}

void UiCommands::LoadPracticeToolFile(const std::string& path)
{
    mClient.Send("loadPracticeToolFile", {{"path", path}});
}

void UiCommands::SetPracticeToolTransport(const std::string& action)
{
    mClient.Send("setPracticeToolTransport", {{"action", action}});
}

void UiCommands::SeekPracticeToolFile(double seconds)
{
    mClient.Send("seekPracticeToolFile", {{"seconds", seconds}});
}

void UiCommands::SetPracticeToolSpeed(double ratio)
{
    mClient.Send("setPracticeToolSpeed", {{"ratio", ratio}});
}

void UiCommands::SetPracticeToolPitch(double semitones)
{
    mClient.Send("setPracticeToolPitch", {{"semitones", semitones}});
}

void UiCommands::SetPracticeToolGain(double gain)
{
    mClient.Send("setPracticeToolGain", {{"gain", gain}});
}

void UiCommands::SetPracticeToolBalance(double balance)
{
    mClient.Send("setPracticeToolBalance", {{"balance", balance}});
}

void UiCommands::SetPracticeToolLoopRegion(double startSec, double endSec, bool restart)
{
    nlohmann::json payload = {{"startSec", startSec}, {"endSec", endSec}};

    if (restart)
    {
        payload["restart"] = true;
    }

    mClient.Send("setPracticeToolLoopRegion", payload);
}

void UiCommands::ClearPracticeToolLoopRegion()
{
    mClient.Send("setPracticeToolLoopRegion");
}

void UiCommands::SetPracticeToolLooping(bool enabled)
{
    mClient.Send("setPracticeToolLooping", {{"enabled", enabled}});
}

void UiCommands::SetPracticeToolEq(const std::optional<bool>& enabled, const nlohmann::json& params)
{
    nlohmann::json payload = nlohmann::json::object();

    if (params.is_object() && !params.empty())
    {
        payload["params"] = params;
    }

    if (enabled)
    {
        payload["enabled"] = *enabled;
    }

    mClient.Send("setPracticeToolEq", payload);
}
} // namespace guitarfx::uiclient
