#pragma once

#include "dsp/EffectGuids.h"

#include <map>
#include <string>

namespace guitarfx::pitch_engine_legacy
{
/**
 * Keeps Pitch Shift and Octave nodes from before their Low Latency default on High Quality.
 *
 * Both effects default to the Low Latency engine now. Pitch Shift's Engine switch shipped with
 * High Quality as its default, and Octave had only that engine, so a stored node without an
 * `engine` key was playing High Quality. It is given `engine` 0 here, so it sounds, and reports
 * its latency, as it did. Saving writes every parameter, so every node stored since carries the
 * key and is left alone, as is one this has already migrated.
 */

constexpr double kHighQuality = 0.0;

/// Pitch Shift or Octave, by canonical id or legacy alias; the registry need not be populated.
[[nodiscard]] inline bool HasEngineSwitch(const std::string& type) noexcept
{
    return type == EffectGuids::kPitchShift || type == "pitch_shift" || type == EffectGuids::kOctave ||
           type == "octave";
}

inline void MigrateParams(const std::string& type, std::map<std::string, double>& params)
{
    if (HasEngineSwitch(type) && params.find("engine") == params.end())
    {
        params["engine"] = kHighQuality;
    }
}
} // namespace guitarfx::pitch_engine_legacy
