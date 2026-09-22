#pragma once

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace guitarfx
{
/**
 * How a parameter's control travel maps onto its range.
 *
 * A knob, a MIDI controller and a DAW lane all move through 0..1; the taper decides where
 * in min..max each point of that travel lands. Linear spaces the range evenly, and suits
 * most parameters. Log spaces it by ratio, so every stretch of travel covers the same
 * number of octaves: a 1-2000 Hz frequency then puts 1-100 Hz in the first 60% of the
 * sweep instead of the first 5%. A log taper needs a range that is positive and increasing.
 *
 * The taper is only about travel. Values are stored, sent and set in native units either
 * way, so choosing one changes no preset.
 */
enum class ParamTaper
{
    Linear,
    Log
};

/// Whether `taper` can map minValue..maxValue: a log taper needs 0 < minValue < maxValue.
[[nodiscard]] constexpr bool IsTaperRangeValid(ParamTaper taper, double minValue, double maxValue) noexcept
{
    return taper == ParamTaper::Linear || (minValue > 0.0 && maxValue > minValue);
}

/// The taper that is actually applied: a log taper over a range it cannot map (a node may
/// narrow a range, or a composite declare one) falls back to linear rather than to NaN.
[[nodiscard]] constexpr ParamTaper EffectiveTaper(ParamTaper taper, double minValue, double maxValue) noexcept
{
    return IsTaperRangeValid(taper, minValue, maxValue) ? taper : ParamTaper::Linear;
}

/// The value at `position` (0..1) along minValue..maxValue. A non-finite position, from a
/// host or a controller, reads as the bottom of the range.
[[nodiscard]] inline double TaperPositionToValue(ParamTaper taper, double minValue, double maxValue,
                                                 double position) noexcept
{
    position = IsFinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;

    if (EffectiveTaper(taper, minValue, maxValue) == ParamTaper::Log)
    {
        // Exact at both ends, which pow of a ratio alone is not.
        if (position >= 1.0)
        {
            return maxValue;
        }

        return minValue * std::pow(maxValue / minValue, position);
    }

    return minValue + position * (maxValue - minValue);
}

/// Where `value` sits (0..1) along minValue..maxValue: the inverse of TaperPositionToValue.
[[nodiscard]] inline double TaperValueToPosition(ParamTaper taper, double minValue, double maxValue,
                                                 double value) noexcept
{
    if (EffectiveTaper(taper, minValue, maxValue) == ParamTaper::Log)
    {
        const double clamped = std::clamp(value, minValue, maxValue);
        return std::log(clamped / minValue) / std::log(maxValue / minValue);
    }

    const double span = maxValue - minValue;
    return span != 0.0 ? std::clamp((value - minValue) / span, 0.0, 1.0) : 0.0;
}

/// The name the effect catalog and composite definitions use for a taper.
[[nodiscard]] constexpr const char* ParamTaperName(ParamTaper taper) noexcept
{
    return taper == ParamTaper::Log ? "log" : "linear";
}

/// The taper a name stands for, or nothing for a name this build does not know.
[[nodiscard]] constexpr std::optional<ParamTaper> ParseParamTaper(std::string_view name) noexcept
{
    if (name == "linear")
    {
        return ParamTaper::Linear;
    }

    if (name == "log")
    {
        return ParamTaper::Log;
    }

    return std::nullopt;
}
} // namespace guitarfx
