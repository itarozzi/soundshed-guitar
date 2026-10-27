#pragma once

/**
 * InputLevelling.h - The words a UI shows for input levelling. The profiles, the training and
 * the trim maths are the engine's (PluginControllerInputLevelling.cpp); these only format what
 * it reports, as the web UI's settings/inputCalibration.ts does.
 */

#include "uiclient/ClientState.h"

#include <optional>
#include <string>

namespace guitarfx::uiclient
{
/// "-12.0 dBFS", or "-" when there is no level.
[[nodiscard]] std::string FormatInputLevelDbfs(std::optional<double> value);
/// "+6.0 dB", or "-".
[[nodiscard]] std::string FormatInputTrimDb(std::optional<double> value);

/// The training dialog's status line: clipped, waiting for a peak, or what saving would apply.
[[nodiscard]] std::string InputLevellingTrainingStatus(const InputLevellingState& state);

/// Whether a profile can be saved from the training now (a name is the caller's to check).
[[nodiscard]] bool CanSaveInputLevellingTraining(const InputLevellingState& state);

/// One profile's second line in the list: what it applies when active, else its description
/// or captured peak.
[[nodiscard]] std::string InputLevellingProfileNote(const InputLevellingProfile& profile, bool active);
} // namespace guitarfx::uiclient
