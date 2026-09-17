/**
 * Pitch Shift's Semitones knob follows the node's own settings. It spans Range Min
 * to Range Max, the interval an expression pedal sweeps, and moves in whole
 * semitones only while Snap to Semitone is on. The engine applies the same rules
 * (PitchShiftEffect.h), so the knob shows what is heard.
 */

import { EffectGuids, resolveEffectType } from "../../effectGuids.js";
import { clampValue } from "../../utils.js";

const HARD_MIN = -12;
const HARD_MAX = 12;
/** A pedal needs something to sweep, so the bounds are kept at least this far apart. */
const MIN_RANGE_WIDTH = 1;

const RANGE_SETTING_KEYS = new Set(["stepMode", "minSemitones", "maxSemitones"]);

type Params = Record<string, number>;

export interface SemitoneKnobRange {
  min: number;
  max: number;
  /** Undefined leaves the knob continuous. */
  step: number | undefined;
}

export function isPitchShiftType(type: string): boolean {
  return resolveEffectType(type) === EffectGuids.kPitchShift;
}

/** The settings that change what the Semitones knob allows. */
export function isPitchShiftRangeSetting(key: string): boolean {
  return RANGE_SETTING_KEYS.has(key);
}

function readParam(params: Params, key: string, fallback: number): number {
  const value = params[key];
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function isSnapped(params: Params): boolean {
  return readParam(params, "stepMode", 1) >= 0.5;
}

function readBounds(params: Params): { min: number; max: number } {
  return {
    min: clampValue(readParam(params, "minSemitones", HARD_MIN), HARD_MIN, HARD_MAX),
    max: clampValue(readParam(params, "maxSemitones", HARD_MAX), HARD_MIN, HARD_MAX),
  };
}

/** Rounds halves away from zero, as std::round does in the engine. */
function roundSemitone(value: number): number {
  return Math.sign(value) * Math.round(Math.abs(value));
}

export function semitoneKnobRange(params: Params): SemitoneKnobRange {
  const { min, max } = readBounds(params);
  // The engine reads crossed bounds as one range, so the knob does too.
  return { min: Math.min(min, max), max: Math.max(min, max), step: isSnapped(params) ? 1 : undefined };
}

/**
 * The other settings that have to move once `changedKey` has been set: the bounds
 * stay a semitone apart, with the one just set winning unless it is pinned at the
 * end of the scale, and the semitone setting stays inside them, on a whole
 * semitone while snapped. Empty when nothing else moves.
 */
export function reconcilePitchShiftParams(params: Params, changedKey: string): Params {
  const bounds = readBounds(params);
  let { min, max } = bounds;

  if (changedKey === "minSemitones" && max - min < MIN_RANGE_WIDTH) {
    max = Math.min(HARD_MAX, min + MIN_RANGE_WIDTH);
    min = Math.min(min, max - MIN_RANGE_WIDTH);
  } else if (changedKey === "maxSemitones" && max - min < MIN_RANGE_WIDTH) {
    min = Math.max(HARD_MIN, max - MIN_RANGE_WIDTH);
    max = Math.max(max, min + MIN_RANGE_WIDTH);
  }

  const updates: Params = {};
  if (min !== bounds.min) {
    updates.minSemitones = min;
  }
  if (max !== bounds.max) {
    updates.maxSemitones = max;
  }

  const current = readParam(params, "semitones", 0);
  const requested = isSnapped(params) ? roundSemitone(current) : current;
  const semitones = clampValue(requested, Math.min(min, max), Math.max(min, max));
  if (semitones !== current) {
    updates.semitones = semitones;
  }

  return updates;
}
