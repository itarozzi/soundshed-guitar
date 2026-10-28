/**
 * UiClientInputLevellingHandlers.cpp - Input levelling's profiles and training
 * (Topic::InputLevelling): the engine's messages, the commands that answer them, and the words
 * a UI shows for them.
 */

#include "uiclient/InputLevelling.h"
#include "uiclient/UiClient.h"
#include "uiclient/UiCommands.h"

#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient
{
namespace
{
std::optional<double> OptionalNumber(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);

    if (it == object.end() || !it->is_number())
    {
        return std::nullopt;
    }

    const double value = it->get<double>();
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::string StringOf(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
} // namespace

const InputLevellingProfile* InputLevellingState::Find(const std::string& id) const
{
    if (id.empty())
    {
        return nullptr;
    }

    for (const auto& profile : profiles)
    {
        if (profile.id == id)
        {
            return &profile;
        }
    }

    return nullptr;
}

std::string FormatInputLevelDbfs(std::optional<double> value)
{
    if (!value || !std::isfinite(*value))
    {
        return "-";
    }

    char text[32];
    std::snprintf(text, sizeof(text), "%.1f dBFS", *value);
    return text;
}

std::string FormatInputTrimDb(std::optional<double> value)
{
    if (!value || !std::isfinite(*value))
    {
        return "-";
    }

    char text[32];
    std::snprintf(text, sizeof(text), "%s%.1f dB", *value >= 0.0 ? "+" : "", *value);
    return text;
}

bool CanSaveInputLevellingTraining(const InputLevellingState& state)
{
    return state.training.active && state.training.capturedPeakDbfs.has_value() && !state.training.clipped;
}

std::string InputLevellingTrainingStatus(const InputLevellingState& state)
{
    if (state.training.clipped)
    {
        return "Input clipped during training. Back the interface gain down and capture again.";
    }

    if (!state.training.capturedPeakDbfs)
    {
        return "Play your loudest picking or strumming to capture a training peak.";
    }

    return "Captured " + FormatInputLevelDbfs(state.training.capturedPeakDbfs) + ". Saving this profile will apply " +
           FormatInputTrimDb(state.training.recommendedTrimDb) + " toward a " +
           FormatInputLevelDbfs(state.targetPeakDbfs) + " target.";
}

std::string InputLevellingProfileNote(const InputLevellingProfile& profile, bool active)
{
    if (active)
    {
        return "Applies " + FormatInputTrimDb(profile.gainDb) + " toward " +
               FormatInputLevelDbfs(profile.targetPeakDbfs);
    }

    return !profile.description.empty() ? profile.description
                                        : "Captured " + FormatInputLevelDbfs(profile.capturedPeakDbfs);
}

void UiClient::RegisterInputLevellingHandlers()
{
    // The profiles and the active one: an answer to getInputLevelling, and after every change,
    // whoever made it.
    On("inputLevelling", [this](const nlohmann::json& m) {
        auto& levelling = mState.inputLevelling;
        levelling.known = true;
        levelling.profiles.clear();

        if (const auto it = m.find("profiles"); it != m.end() && it->is_array())
        {
            for (const auto& entry : *it)
            {
                if (!entry.is_object())
                {
                    continue;
                }

                InputLevellingProfile profile;
                profile.id = StringOf(entry, "id");
                profile.name = StringOf(entry, "name");
                profile.description = StringOf(entry, "description");
                profile.capturedPeakDbfs = OptionalNumber(entry, "capturedPeakDbfs");
                profile.targetPeakDbfs = OptionalNumber(entry, "targetPeakDbfs").value_or(-12.0);
                profile.gainDb = OptionalNumber(entry, "gainDb").value_or(0.0);
                profile.createdAt = StringOf(entry, "createdAt");
                profile.updatedAt = StringOf(entry, "updatedAt");

                if (!profile.id.empty())
                {
                    levelling.profiles.push_back(std::move(profile));
                }
            }
        }

        levelling.activeId = StringOf(m, "activeId");
        levelling.trainingActive = m.value("trainingActive", false);
        levelling.targetPeakDbfs = OptionalNumber(m, "targetPeakDbfs").value_or(-12.0);
        levelling.maxTrimDb = OptionalNumber(m, "maxTrimDb").value_or(24.0);
        Notify(Topic::InputLevelling);
    });

    // ~15 Hz while a profile is trained, and once when training stops.
    On("inputLevellingTraining", [this](const nlohmann::json& m) {
        auto& training = mState.inputLevelling.training;
        training.active = m.value("active", false);
        training.livePeakDbfs = OptionalNumber(m, "livePeakDbfs");
        training.capturedPeakDbfs = OptionalNumber(m, "capturedPeakDbfs");
        training.recommendedTrimDb = OptionalNumber(m, "recommendedTrimDb");
        training.clipped = m.value("clipped", false);
        mState.inputLevelling.trainingActive = training.active;
        Notify(Topic::InputLevelling);
    });

    // A save's outcome, told as the web UI tells it.
    On("inputLevellingSaved", [this](const nlohmann::json& m) {
        auto& levelling = mState.inputLevelling;
        ++levelling.saveSerial;
        levelling.saveOk = m.value("ok", false);
        levelling.saveMessage = StringOf(m, "message");

        if (levelling.saveOk && m.contains("profile") && m["profile"].is_object())
        {
            const auto& profile = m["profile"];
            PostNotification(
                {Notification::Kind::Info, "Input levelling profile saved",
                 StringOf(profile, "name") + " • " + FormatInputTrimDb(OptionalNumber(profile, "gainDb"))});
        }
        else if (!levelling.saveOk)
        {
            PostNotification({Notification::Kind::Error, "Input levelling profile not saved", levelling.saveMessage});
        }

        Notify(Topic::InputLevelling);
    });
}

// ── Senders ─────────────────────────────────────────────────────────────────────────────

void UiCommands::GetInputLevelling()
{
    mClient.Send("getInputLevelling");
}

void UiCommands::SelectInputLevellingProfile(const std::string& id)
{
    // Shown at once; the engine's "inputLevelling" stands.
    mClient.MutableState().inputLevelling.activeId = id;
    mClient.Send("selectInputLevellingProfile", {{"id", id.empty() ? nlohmann::json() : nlohmann::json(id)}});
    mClient.Notify(Topic::InputLevelling);
}

void UiCommands::StartInputLevellingTraining()
{
    mClient.Send("startInputLevellingTraining");
}

void UiCommands::StopInputLevellingTraining()
{
    mClient.Send("stopInputLevellingTraining");
}

void UiCommands::ResetInputLevellingTraining()
{
    mClient.Send("resetInputLevellingTraining");
}

void UiCommands::SaveInputLevellingProfile(const std::string& name, const std::string& description)
{
    mClient.Send("saveInputLevellingProfile", {{"name", name}, {"description", description}});
}

void UiCommands::RenameInputLevellingProfile(const std::string& id, const std::string& name,
                                             const std::string& description)
{
    mClient.Send("renameInputLevellingProfile", {{"id", id}, {"name", name}, {"description", description}});
}

void UiCommands::DeleteInputLevellingProfile(const std::string& id)
{
    mClient.Send("deleteInputLevellingProfile", {{"id", id}});
}
} // namespace guitarfx::uiclient
