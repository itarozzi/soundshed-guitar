#pragma once

/**
 * Parameter and preset scaffolding shared by the character delays (`delay_tape`,
 * `delay_analog`).
 *
 * Each effect keeps one `constexpr std::array<ParamSpec, N>` as the single place its
 * parameter ranges live — registration, SetParam's clamping and GetParam all read it — the
 * same arrangement WahEffect uses. What is here is everything about that arrangement that
 * does not depend on which effect it is.
 */

#include "dsp/EffectRegistry.h"
#include "dsp/LevelTargets.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace guitarfx::delay_spec
{
struct ParamSpec
{
    const char* id;
    const char* displayName;
    double defaultValue;
    double minValue;
    double maxValue;
    const char* unit;
    const char* group;
    bool advanced;
    double step;
};

/// The index of `key` in `specs`, or N if it is not one of them. `timeMs` is accepted for
/// `time`, the spelling DelayEffect also answers to. Allocates nothing.
template <std::size_t N>
[[nodiscard]] std::size_t FindParam(const std::array<ParamSpec, N>& specs, const std::string& key)
{
    const std::string_view lookup = (key == "timeMs") ? std::string_view("time") : std::string_view(key);

    for (std::size_t index = 0; index < N; ++index)
    {
        if (lookup == specs[index].id)
        {
            return index;
        }
    }

    return N;
}

/// Clamps to the spec's range and snaps to its step.
[[nodiscard]] inline double ClampToSpec(const ParamSpec& spec, double value)
{
    double clamped = std::clamp(value, spec.minValue, spec.maxValue);

    if (spec.step > 0.0)
    {
        clamped = std::round(clamped / spec.step) * spec.step;
    }

    return clamped;
}

/// Labels for the tempo-sync pair, which both delays share; empty for anything else.
[[nodiscard]] inline std::vector<std::string> TempoLabelsFor(const std::string& id)
{
    if (id == "syncMode")
    {
        return tempo_sync::SyncModeLabels();
    }

    if (id == "syncDivision")
    {
        return tempo_sync::DivisionLabels();
    }

    return {};
}

/// The delay a Time / Sync / Division trio resolves to, clamped to Time's own range.
[[nodiscard]] inline double ResolveDelayMs(const ParamSpec& timeSpec, double timeMs, double syncMode, double division,
                                           double bpm)
{
    if (static_cast<int>(syncMode) != tempo_sync::kSyncModeTempo)
    {
        return timeMs;
    }

    const double synced = tempo_sync::DivisionDelayMs(bpm, static_cast<int>(division));
    return std::clamp(synced, timeSpec.minValue, timeSpec.maxValue);
}

/// A Glide setting is the time to settle, which is about three one-pole time constants.
inline constexpr double kGlideConstantsToSettle = 3.0;

/// Saturator drive for a 0-1 Saturation knob, referenced to the nominal operating level so
/// the knob means the same thing however hot the chain runs into it: at full drive the
/// knee sits at the nominal level itself, and at zero the saturator is bypassed.
[[nodiscard]] inline float SaturationDrive(double saturation)
{
    const double nominal = DbToLinearGain(kDefaultNominalOperatingLevelDbfs);
    return static_cast<float>(std::clamp(saturation, 0.0, 1.0) * (1.0 / nominal - 1.0));
}

[[nodiscard]] inline EffectPresetDefinition MakePreset(const char* id, const char* name, bool isDefault,
                                                       std::initializer_list<std::pair<const char*, double>> values)
{
    EffectPresetDefinition preset;
    preset.id = id;
    preset.displayName = name;
    preset.isFactory = true;
    preset.isDefault = isDefault;

    for (const auto& [key, value] : values)
    {
        preset.parameters[key] = value;
        preset.parameterOrder.emplace_back(key);
    }

    return preset;
}

/// The registry's view of `specs`, with each enum's labels supplied by `labelsFor(id)`.
template <std::size_t N, typename LabelsFor>
[[nodiscard]] std::vector<ParameterDef> BuildParameterDefs(const std::array<ParamSpec, N>& specs, LabelsFor labelsFor)
{
    std::vector<ParameterDef> defs;
    defs.reserve(N);

    for (const auto& spec : specs)
    {
        defs.push_back({spec.id,
                        spec.displayName,
                        spec.defaultValue,
                        spec.minValue,
                        spec.maxValue,
                        spec.unit,
                        spec.group,
                        spec.advanced,
                        spec.step,
                        labelsFor(spec.id),
                        {}});
    }

    return defs;
}
} // namespace guitarfx::delay_spec
