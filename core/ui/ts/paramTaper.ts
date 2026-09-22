/**
 * Parameter tapers: how a control's travel maps onto a parameter's range.
 *
 * The engine declares a taper on a parameter in the effect catalog (`taper: "log"`); no
 * taper means linear. A log taper spaces the range by ratio, so equal knob travel covers
 * an equal number of octaves: a 1-2000 Hz frequency puts 1-100 Hz in the first 60% of a
 * sweep instead of the first 5%. Values stay in native units everywhere; only travel
 * changes. The mapping matches core/src/dsp/ParamTaper.h, which MIDI and host automation
 * use, so a knob and an expression pedal agree on where the middle is.
 */

export type ParamTaper = "linear" | "log";

/** A taper named in a catalog or data attribute, or undefined for one this UI does not know. */
export function parseParamTaper(value: unknown): ParamTaper | undefined {
  return value === "log" || value === "linear" ? value : undefined;
}

/**
 * The taper actually applied: a log taper needs a positive, increasing range, and over any
 * other range falls back to linear rather than producing NaN (as the engine does).
 */
export function effectiveTaper(taper: ParamTaper | undefined, min: number, max: number): ParamTaper {
  return taper === "log" && Number.isFinite(min) && Number.isFinite(max) && min > 0 && max > min ? "log" : "linear";
}

/** Where `value` sits along min..max, 0..1. */
export function valueToTaperPosition(value: number, min: number, max: number, taper?: ParamTaper): number {
  if (effectiveTaper(taper, min, max) === "log") {
    const clamped = Math.min(max, Math.max(min, value));
    return Math.log(clamped / min) / Math.log(max / min);
  }
  const span = max - min;
  return span !== 0 ? Math.min(1, Math.max(0, (value - min) / span)) : 0;
}

/** The value at `position` (0..1) along min..max: the inverse of valueToTaperPosition. */
export function taperPositionToValue(position: number, min: number, max: number, taper?: ParamTaper): number {
  const p = Number.isFinite(position) ? Math.min(1, Math.max(0, position)) : 0;
  if (effectiveTaper(taper, min, max) === "log") {
    return p >= 1 ? max : min * Math.pow(max / min, p);
  }
  return min + p * (max - min);
}

/**
 * A value on a log taper, to about three significant figures, since it can be anything
 * from 1.25 to 18,000: two decimals would be noise at the top, and none too coarse at the
 * bottom. Hz from 1 kHz up reads in kHz.
 */
export function formatTaperedValue(value: number, unit?: string): string {
  const suffix = unit && unit !== "amount" ? unit : "";
  // 999.6 Hz would otherwise round to "1000Hz"
  if (suffix === "Hz" && Math.abs(value) >= 999.5) {
    const khz = value / 1000;
    return `${khz.toFixed(Math.abs(khz) >= 10 ? 1 : 2)}kHz`;
  }
  const magnitude = Math.abs(value);
  const decimals = magnitude >= 100 ? 0 : magnitude >= 10 ? 1 : 2;
  return `${value.toFixed(decimals)}${suffix}`;
}
