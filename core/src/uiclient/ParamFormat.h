#pragma once

/**
 * ParamFormat.h - How a parameter's value is shown and how a control's travel maps onto it.
 *
 * Formatting follows the web UI's knob (core/ui/ts/signalPath/paramsPanel/paramControls.ts):
 * an enum shows its label, pan shows L/C/R, a log-taper value shows about three significant
 * figures (kHz from 1 kHz up), one that moves in whole steps a whole number, and anything else
 * two decimals, each with its unit. Travel goes through
 * the engine's own taper (dsp/ParamTaper.h), which is what MIDI and host automation use, so
 * a control here and an expression pedal agree on where the middle is.
 */

#include "uiclient/ClientState.h"

#include <cstddef>
#include <optional>
#include <string>

namespace guitarfx::uiclient
{
[[nodiscard]] std::string FormatParamValue(const EffectParamInfo& param, double value);

/// The web UI's formatTaperedValue: about three significant figures, kHz from 1 kHz up.
[[nodiscard]] std::string FormatTaperedValue(double value, const std::string& unit);

[[nodiscard]] double ParamValueToPosition(const EffectParamInfo& param, double value);
[[nodiscard]] double ParamPositionToValue(const EffectParamInfo& param, double position);

/// Clamps to the range and snaps to the parameter's step, if it has one.
[[nodiscard]] double SnapParamValue(const EffectParamInfo& param, double value);

/// A value as a knob's number field shows it for typing over: in the parameter's own unit, with
/// as many decimals as its step has (two without one), as the web UI's knob fills its input.
[[nodiscard]] std::string EditableParamText(const EffectParamInfo& param, double value);
/// The number typed into a value field, if it reads as one ("440", "-6.5", "1.2k").
[[nodiscard]] std::optional<double> ParseTypedNumber(const std::string& text);

[[nodiscard]] bool IsToggleParam(const EffectParamInfo& param);
[[nodiscard]] bool IsEnumParam(const EffectParamInfo& param);

/// A settings flyout's switch (flyoutParamControls.ts isSwitchParam): a toggle, or any 0..1
/// parameter with two labels, as the gate's Stereo Link is ({0,1}, "Independent"/"Linked").
[[nodiscard]] bool IsSwitchParam(const EffectParamInfo& param);

/// A settings flyout's value (formatFlyoutParamValue): a switch's label, an enum's label, or
/// the number to the decimals its span calls for (2 within 2, 1 within 60, else 0) with its unit.
[[nodiscard]] std::string FormatFlyoutParamValue(const EffectParamInfo& param, double value);

/// Which of an enum's labels a value shows, or -1 for none. Labels are listed in value order from
/// the minimum, one per step, so the Auto Arpeggiator's Steps (2 to 8) shows "4" at 4, where
/// indexing by the value showed "6".
[[nodiscard]] int EnumLabelIndex(const EffectParamInfo& param, double value);

/// The value an enum's label stands for: the minimum plus `index` steps.
[[nodiscard]] double EnumLabelValue(const EffectParamInfo& param, std::size_t index);

/// The value a node holds for a parameter, or the parameter's default.
[[nodiscard]] double NodeParamValue(const GraphNode& node, const EffectParamInfo& param);

/// Whether the parameter's control is shown for the node's current values: its `visibleWhen`
/// holds, as the web UI's isParamVisible decides it. `type` supplies the controlling
/// parameter's default when the node does not hold a value for it.
[[nodiscard]] bool IsParamVisible(const EffectParamInfo& param, const GraphNode& node, const EffectTypeInfo* type);
} // namespace guitarfx::uiclient
