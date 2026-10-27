#pragma once

/**
 * UpdateCheck - whether a newer Soundshed build is out (the web UI's updateCheck.ts), for the
 * native UIs: the version comparison here, the request in ToneService::CheckForUpdate. The check
 * runs once a session, a few seconds after the first state, unless app.updateCheckEnabled is
 * false; the service answers with its newest release, which counts only when it is newer.
 */

#include <array>
#include <optional>
#include <string>

namespace guitarfx::uiclient
{
inline constexpr const char* kUpdateCheckEnabledSetting = "app.updateCheckEnabled";
inline constexpr const char* kInstanceIdSetting = "app.instanceId";

struct AvailableUpdate
{
    std::string version;
    std::string downloadUrl; // http(s) only; empty otherwise
    std::string releaseNotes;
};

/// "v1.7.2-beta" as {1, 7, 2}; nothing for text that is not a version.
[[nodiscard]] std::optional<std::array<int, 3>> ParseVersion(const std::string& version);

/// True only when both parse and `candidate` is higher than `current`.
[[nodiscard]] bool IsVersionNewer(const std::string& candidate, const std::string& current);
} // namespace guitarfx::uiclient
