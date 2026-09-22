#pragma once

#include "dsp/EffectRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace guitarfx
{
/**
 * One parameter as an effect declares it, in plain constants.
 *
 * An effect keeps a `constexpr std::array<EffectParamSpec, N>`, in the order its controls
 * should appear, as the single place its parameter ranges live: registration builds the
 * registry's ParameterDefs from it, and SetParam and GetParam find keys in it. Nothing here
 * allocates, so a lookup is safe on the audio thread.
 */
struct EffectParamSpec
{
    const char* id;
    const char* displayName;
    double defaultValue;
    double minValue;
    double maxValue;
    const char* unit;
    const char* group;
    bool advanced = false;
    double step = 0.0;                        ///< 0 = continuous
    std::span<const char* const> labels = {}; ///< an enum's choices, in value order
    ParamTaper taper = ParamTaper::Linear;    ///< set with LogTaper(), which checks the range
};

/// `spec` on a log taper, for a range that spans decades, a frequency most often: equal
/// travel then moves the value by an equal ratio. Compile-time only, so a range a log taper
/// cannot map (not positive and increasing) fails the build rather than a knob.
[[nodiscard]] consteval EffectParamSpec LogTaper(EffectParamSpec spec)
{
    if (!IsTaperRangeValid(ParamTaper::Log, spec.minValue, spec.maxValue))
    {
        throw "a log taper needs 0 < minValue < maxValue";
    }

    spec.taper = ParamTaper::Log;
    return spec;
}

/// The index of `key` in `specs`, or N when it is not one of them.
template <std::size_t N>
[[nodiscard]] constexpr std::size_t FindParamSpec(const std::array<EffectParamSpec, N>& specs,
                                                  std::string_view key) noexcept
{
    for (std::size_t index = 0; index < N; ++index)
    {
        if (key == specs[index].id)
        {
            return index;
        }
    }

    return N;
}

/// Clamps `value` into the spec's range and snaps a stepped parameter onto its step.
[[nodiscard]] inline double NormaliseParamValue(const EffectParamSpec& spec, double value) noexcept
{
    value = std::clamp(value, spec.minValue, spec.maxValue);

    if (spec.step > 0.0)
    {
        value = spec.minValue + std::round((value - spec.minValue) / spec.step) * spec.step;
    }

    return value;
}

template <std::size_t N>
[[nodiscard]] constexpr std::array<double, N> DefaultParamValues(const std::array<EffectParamSpec, N>& specs) noexcept
{
    std::array<double, N> values = {};

    for (std::size_t index = 0; index < N; ++index)
    {
        values[index] = specs[index].defaultValue;
    }

    return values;
}

/// The registry's view of `specs`, for EffectTypeInfo::parameters.
template <std::size_t N>
[[nodiscard]] std::vector<ParameterDef> BuildParameterDefs(const std::array<EffectParamSpec, N>& specs)
{
    std::vector<ParameterDef> defs;
    defs.reserve(N);

    for (const auto& spec : specs)
    {
        ParameterDef def;
        def.id = spec.id;
        def.displayName = spec.displayName;
        def.defaultValue = spec.defaultValue;
        def.minValue = spec.minValue;
        def.maxValue = spec.maxValue;
        def.unit = spec.unit;
        def.group = spec.group;
        def.advanced = spec.advanced;
        def.step = spec.step;
        def.labels.assign(spec.labels.begin(), spec.labels.end());
        def.taper = spec.taper;
        defs.push_back(std::move(def));
    }

    return defs;
}
} // namespace guitarfx
