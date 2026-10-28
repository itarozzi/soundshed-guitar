/**
 * PluginControllerInputLevelling.cpp - Input levelling: one trim for each guitar.
 *
 * A profile records the loudest peak a guitar produced while it was trained and the trim that
 * brings that peak to the level the amp models expect (target − captured, within ±24 dB). The
 * active profile's trim is applied to the raw input ahead of everything else
 * (ApplyUserInputCalibrationSettingsFromAppSettings). The settings keep the older
 * "userInputCalibration" names, so saved profiles carry over.
 *
 * Training bypasses the active trim and arms the mixer's InputLevelHold, which follows the raw
 * input's peak on the audio thread; the idle loop reports it as "inputLevellingTraining" until
 * training stops. Saving takes the captured peak from the hold, so a UI only sends a name.
 */

#include "PluginController.h"

#include "controller/internal/ControllerUtils.h"
#include "controller/internal/SettingsKeys.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace guitarfx
{
using namespace controller_detail;

namespace
{
/// The level a trained peak is brought to, and the most a profile may move it.
constexpr double kInputLevellingTargetPeakDbfs = -12.0;
constexpr double kInputLevellingMaxTrimDb = 24.0;
constexpr auto kTrainingStatusInterval = std::chrono::milliseconds(66);

double PeakDbfs(float linear)
{
    return linear > 0.0f ? 20.0 * std::log10(static_cast<double>(linear)) : -std::numeric_limits<double>::infinity();
}

nlohmann::json DbOrNull(double value)
{
    return std::isfinite(value) ? nlohmann::json(std::round(value * 100.0) / 100.0) : nlohmann::json();
}

double TrimFor(double targetDbfs, double capturedDbfs)
{
    return std::clamp(targetDbfs - capturedDbfs, -kInputLevellingMaxTrimDb, kInputLevellingMaxTrimDb);
}

std::string Trimmed(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string StringField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

double NumberField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : std::numeric_limits<double>::quiet_NaN();
}
} // namespace

nlohmann::json PluginController::ReadInputLevellingProfiles() const
{
    nlohmann::json profiles = nlohmann::json::array();
    const auto it = mAppSettings.find(kUserInputCalibrationProfilesSettingKey);

    if (it == mAppSettings.end() || !it->is_array())
    {
        return profiles;
    }

    for (const auto& entry : *it)
    {
        if (!entry.is_object())
        {
            continue;
        }

        const std::string id = Trimmed(StringField(entry, "id"));
        const std::string name = Trimmed(StringField(entry, "name"));

        if (id.empty() || name.empty())
        {
            continue;
        }

        const double captured = NumberField(entry, "capturedPeakDbfs");
        double target = NumberField(entry, "targetPeakDbfs");
        target = std::isfinite(target) ? target : kInputLevellingTargetPeakDbfs;
        double gain = NumberField(entry, "gainDb");

        if (!std::isfinite(gain))
        {
            gain = std::isfinite(captured) ? target - captured : 0.0;
        }

        nlohmann::json profile = {{"id", id},
                                  {"name", name},
                                  {"description", StringField(entry, "description")},
                                  {"capturedPeakDbfs", DbOrNull(captured)},
                                  {"targetPeakDbfs", target},
                                  {"gainDb", std::clamp(gain, -kInputLevellingMaxTrimDb, kInputLevellingMaxTrimDb)}};

        for (const char* key : {"createdAt", "updatedAt"})
        {
            if (const std::string stamp = StringField(entry, key); !stamp.empty())
            {
                profile[key] = stamp;
            }
        }

        profiles.push_back(std::move(profile));
    }

    return profiles;
}

void PluginController::SendInputLevellingState()
{
    const nlohmann::json profiles = ReadInputLevellingProfiles();
    std::string activeId;

    if (const auto active = mAppSettings.find(kUserInputCalibrationActiveProfileIdSettingKey);
        active != mAppSettings.end() && active->is_string())
    {
        activeId = Trimmed(active->get<std::string>());
    }

    const bool activeKnown = std::any_of(profiles.begin(), profiles.end(),
                                         [&](const nlohmann::json& p) { return p.value("id", "") == activeId; });

    SendMessageToUI(nlohmann::json{{"type", "inputLevelling"},
                                   {"profiles", profiles},
                                   {"activeId", activeKnown ? nlohmann::json(activeId) : nlohmann::json()},
                                   {"trainingActive", mUserInputCalibrationTrainingActive},
                                   {"targetPeakDbfs", kInputLevellingTargetPeakDbfs},
                                   {"maxTrimDb", kInputLevellingMaxTrimDb}}
                        .dump());
}

void PluginController::SendInputLevellingTraining()
{
    auto& hold = mPresetMixer.GetInputLevelHold();
    const bool active = mUserInputCalibrationTrainingActive && hold.IsArmed();
    const double live = active ? PeakDbfs(hold.TakeLivePeak()) : -std::numeric_limits<double>::infinity();
    const double captured = active ? PeakDbfs(hold.CapturedPeak()) : -std::numeric_limits<double>::infinity();
    const bool clipped = active && hold.Clipped();

    mInputLevellingLastSent = std::chrono::steady_clock::now();
    SendMessageToUI(nlohmann::json{
        {"type", "inputLevellingTraining"},
        {"active", active},
        {"livePeakDbfs", DbOrNull(live)},
        {"capturedPeakDbfs", DbOrNull(captured)},
        {"clipped", clipped},
        {"recommendedTrimDb",
         std::isfinite(captured) ? DbOrNull(TrimFor(kInputLevellingTargetPeakDbfs, captured)) : nlohmann::json()},
        {"targetPeakDbfs", kInputLevellingTargetPeakDbfs}}
                        .dump());
}

void PluginController::SetInputLevellingTraining(bool active)
{
    mUserInputCalibrationTrainingActive = active;
    // Arming resets the hold, so each training starts from silence.
    mPresetMixer.GetInputLevelHold().Arm(active);
    ApplyUserInputCalibrationSettingsFromAppSettings();
    SendInputLevellingTraining();
}

void PluginController::PollInputLevellingTraining()
{
    if (!mUserInputCalibrationTrainingActive ||
        std::chrono::steady_clock::now() - mInputLevellingLastSent < kTrainingStatusInterval)
    {
        return;
    }

    SendInputLevellingTraining();
}

void PluginController::HandleResetInputLevellingTraining()
{
    if (!mUserInputCalibrationTrainingActive)
    {
        return;
    }

    mPresetMixer.GetInputLevelHold().Reset();
    SendInputLevellingTraining();
}

void PluginController::CommitInputLevellingSettings(bool profilesChanged)
{
    // First, so an active id that now names no profile is cleared before the save. It sends
    // "inputLevelling" as well.
    ApplyUserInputCalibrationSettingsFromAppSettings();
    SaveAppSettings();

    if (profilesChanged)
    {
        SendMessageToUI(nlohmann::json{{"type", "appSettingChanged"},
                                       {"key", kUserInputCalibrationProfilesSettingKey},
                                       {"value", mAppSettings[kUserInputCalibrationProfilesSettingKey]}}
                            .dump());
    }

    SendMessageToUI(
        nlohmann::json{{"type", "appSettingChanged"},
                       {"key", kUserInputCalibrationActiveProfileIdSettingKey},
                       {"value", mAppSettings.value(kUserInputCalibrationActiveProfileIdSettingKey, nlohmann::json())}}
            .dump());
}

void PluginController::HandleSelectInputLevellingProfile(const nlohmann::json& payload)
{
    const auto it = payload.find("id");
    const std::string id = it != payload.end() && it->is_string() ? Trimmed(it->get<std::string>()) : std::string{};
    mAppSettings[kUserInputCalibrationActiveProfileIdSettingKey] = id.empty() ? nlohmann::json() : nlohmann::json(id);
    CommitInputLevellingSettings(false);
}

void PluginController::HandleSaveInputLevellingProfile(const nlohmann::json& payload)
{
    const auto fail = [this](const char* error, const char* message) {
        SendMessageToUI(
            nlohmann::json{{"type", "inputLevellingSaved"}, {"ok", false}, {"error", error}, {"message", message}}
                .dump());
    };

    const std::string name = Trimmed(payload.value("name", std::string{}));

    if (name.empty())
    {
        fail("nameRequired", "Input levelling profile name required");
        return;
    }

    auto& hold = mPresetMixer.GetInputLevelHold();
    const double captured = mUserInputCalibrationTrainingActive && hold.IsArmed()
                                ? PeakDbfs(hold.CapturedPeak())
                                : -std::numeric_limits<double>::infinity();

    if (!std::isfinite(captured))
    {
        fail("noPeak", "No training level captured. Play your guitar first.");
        return;
    }

    if (hold.Clipped())
    {
        fail("clipped", "Input clipped during training. Reduce interface gain and capture again.");
        return;
    }

    const std::string timestamp = BuildUtcIsoTimestamp();
    const double capturedRounded = std::round(captured * 100.0) / 100.0;
    nlohmann::json profile = {{"id", GenerateGuidV4String()},
                              {"name", name},
                              {"description", Trimmed(payload.value("description", std::string{}))},
                              {"capturedPeakDbfs", capturedRounded},
                              {"targetPeakDbfs", kInputLevellingTargetPeakDbfs},
                              {"gainDb", TrimFor(kInputLevellingTargetPeakDbfs, capturedRounded)},
                              {"createdAt", timestamp},
                              {"updatedAt", timestamp}};

    auto& stored = mAppSettings[kUserInputCalibrationProfilesSettingKey];

    if (!stored.is_array())
    {
        stored = nlohmann::json::array();
    }

    stored.push_back(profile);
    mAppSettings[kUserInputCalibrationActiveProfileIdSettingKey] = profile["id"];

    // Saving ends the training: the new profile's trim takes over from the bypass.
    mUserInputCalibrationTrainingActive = false;
    hold.Arm(false);
    CommitInputLevellingSettings(true);
    SendInputLevellingTraining();
    SendMessageToUI(nlohmann::json{{"type", "inputLevellingSaved"}, {"ok", true}, {"profile", profile}}.dump());
}

void PluginController::HandleRenameInputLevellingProfile(const nlohmann::json& payload)
{
    const std::string id = Trimmed(payload.value("id", std::string{}));
    const std::string name = Trimmed(payload.value("name", std::string{}));
    auto it = mAppSettings.find(kUserInputCalibrationProfilesSettingKey);

    if (id.empty() || name.empty() || it == mAppSettings.end() || !it->is_array())
    {
        return;
    }

    for (auto& entry : *it)
    {
        if (entry.is_object() && Trimmed(StringField(entry, "id")) == id)
        {
            entry["name"] = name;

            if (payload.contains("description") && payload["description"].is_string())
            {
                entry["description"] = Trimmed(payload["description"].get<std::string>());
            }

            entry["updatedAt"] = BuildUtcIsoTimestamp();
            CommitInputLevellingSettings(true);
            return;
        }
    }
}

void PluginController::HandleDeleteInputLevellingProfile(const nlohmann::json& payload)
{
    const std::string id = Trimmed(payload.value("id", std::string{}));
    auto it = mAppSettings.find(kUserInputCalibrationProfilesSettingKey);

    if (id.empty() || it == mAppSettings.end() || !it->is_array())
    {
        return;
    }

    const auto before = it->size();
    it->erase(std::remove_if(it->begin(), it->end(),
                             [&](const nlohmann::json& entry) {
                                 return entry.is_object() && Trimmed(StringField(entry, "id")) == id;
                             }),
              it->end());

    if (it->size() == before)
    {
        return;
    }

    // The active id now names no profile; the re-apply clears it.
    CommitInputLevellingSettings(true);
}
} // namespace guitarfx
