#pragma once

// The preset inspector behind the save dialog's Advanced tab: the active preset's JSON, a
// validation report, and the clean-up pass offered beside it. A port of the web UI's
// presets/validate.ts (validatePresetForUi, cleanupPresetForUi) so both UIs report the same
// things for the same preset.

#include "presets/PresetTypes.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// The preset as the inspector shows it: the engine's own serialisation, without the global
/// signal chain (a preset is saved without it).
[[nodiscard]] nlohmann::json InspectablePresetJson(const Preset& preset);

/// What is wrong with a preset's JSON, as sentences; empty when nothing is.
[[nodiscard]] std::vector<std::string> ValidatePresetJson(const nlohmann::json& preset);

struct PresetCleanup
{
    nlohmann::json cleaned;
    std::vector<std::string> removedKeys; // in the order they were removed, each once
    int normalizedAliases = 0;            // legacy id/type keys dropped from resource refs
    bool removedGlobalEq = false;         // a disabled, default global EQ node taken out
};

/// Drops unknown and empty fields, folds legacy resource-ref aliases into resourceId and
/// resourceType, and takes out a stray default global EQ node, relinking around it.
[[nodiscard]] PresetCleanup CleanupPresetJson(const nlohmann::json& preset);

/// The clean-up's report, one line each, as the web UI words it.
[[nodiscard]] std::vector<std::string> CleanupReport(const PresetCleanup& cleanup);

/// The raw input's peak over the last ten seconds, as the save dialog shows it beside the
/// preset's designed peak (the web UI's signalPeakHold.rawInput): a new window starts once the
/// current one is ten seconds old, and a louder reading raises the hold at once.
class PeakHold
{
  public:
    static constexpr double kWindowSeconds = 10.0;

    void Push(double peakDb, double nowSeconds);

    /// Nothing until the first reading.
    [[nodiscard]] std::optional<double> Peak() const;

  private:
    double mPeak = 0.0;
    double mWindowStart = 0.0;
    bool mHas = false;
};

/// A peak as the dialog writes it: "-12.3 dBFS", or a dash when there is none.
[[nodiscard]] std::string FormatPeakDb(std::optional<double> peakDb);
} // namespace guitarfx::uiclient
