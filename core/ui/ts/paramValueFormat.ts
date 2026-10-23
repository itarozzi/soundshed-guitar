/**
 * How a parameter's value is shown under its control: the same text in the params panel, a
 * custom layout and the layout designer's preview.
 */

import { enumLabel, wholeStepLabel } from "./paramLabels.js";
import { formatTaperedValue, type ParamTaper } from "./paramTaper.js";

/**
 * Format a parameter value for display.
 * Returns just the number for generic "amount" units to keep labels compact.
 * Exported so the signal-path renderer and designer preview share identical formatting.
 * A log-taper value, which can run from 1.25 to 18,000, is shown to about three figures, and
 * one that moves in whole steps as a whole number with its unit (wholeStepLabel).
 */
export function formatParamValue(
  value: number,
  unit?: string,
  labels?: string[],
  taper?: ParamTaper,
  min = 0,
  step?: number,
): string {
  if (unit === "toggle") {
    return value >= 0.5 ? "On" : "Off";
  }
  if (unit === "enum" && Array.isArray(labels)) {
    return enumLabel(value, labels, min, step) ?? `${Math.round(value)}`;
  }
  if (taper === "log") {
    return formatTaperedValue(value, unit);
  }
  const whole = wholeStepLabel(value, unit, step);
  if (whole !== undefined) {
    return whole;
  }
  if (unit === "dB" || unit === "ms" || unit === "Hz") {
    return `${value.toFixed(1)}${unit}`;
  }
  return value.toFixed(2);
}
