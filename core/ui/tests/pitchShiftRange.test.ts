import { describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import {
  isPitchShiftRangeSetting,
  isPitchShiftType,
  reconcilePitchShiftParams,
  semitoneKnobRange,
} from "../ts/signalPath/paramsPanel/pitchShiftRange.js";

describe("isPitchShiftType", () => {
  it("accepts the canonical id and the legacy alias", () => {
    expect(isPitchShiftType(EffectGuids.kPitchShift)).toBe(true);
    expect(isPitchShiftType("pitch_shift")).toBe(true);
    expect(isPitchShiftType(EffectGuids.kTranspose)).toBe(false);
  });
});

describe("isPitchShiftRangeSetting", () => {
  it("covers the settings that reshape the Semitones knob, not the knob itself", () => {
    expect(["stepMode", "minSemitones", "maxSemitones"].every(isPitchShiftRangeSetting)).toBe(true);
    expect(isPitchShiftRangeSetting("semitones")).toBe(false);
    expect(isPitchShiftRangeSetting("mix")).toBe(false);
  });
});

describe("semitoneKnobRange", () => {
  it("defaults to the full scale in whole semitones, as the engine does", () => {
    expect(semitoneKnobRange({})).toEqual({ min: -12, max: 12, step: 1 });
  });

  it("follows the node's range and snap setting", () => {
    expect(semitoneKnobRange({ minSemitones: 0, maxSemitones: 7, stepMode: 0 })).toEqual({ min: 0, max: 7, step: undefined });
  });

  it("reads crossed bounds as one range and clamps to the scale", () => {
    expect(semitoneKnobRange({ minSemitones: 5, maxSemitones: -30 })).toEqual({ min: -12, max: 5, step: 1 });
  });
});

describe("reconcilePitchShiftParams", () => {
  it("moves nothing when the settings already agree", () => {
    expect(reconcilePitchShiftParams({ semitones: 3, minSemitones: 0, maxSemitones: 7, stepMode: 1 }, "maxSemitones")).toEqual({});
  });

  it("pulls the semitone setting inside a narrowed range", () => {
    expect(reconcilePitchShiftParams({ semitones: 9, minSemitones: 0, maxSemitones: 7 }, "maxSemitones")).toEqual({ semitones: 7 });
    expect(reconcilePitchShiftParams({ semitones: -4, minSemitones: 0, maxSemitones: 7 }, "minSemitones")).toEqual({ semitones: 0 });
  });

  it("rounds a free value when snapping is turned on, halves away from zero like the engine", () => {
    expect(reconcilePitchShiftParams({ semitones: 2.4, stepMode: 1 }, "stepMode")).toEqual({ semitones: 2 });
    expect(reconcilePitchShiftParams({ semitones: -2.5, stepMode: 1 }, "stepMode")).toEqual({ semitones: -3 });
    expect(reconcilePitchShiftParams({ semitones: 2.4, stepMode: 0 }, "stepMode")).toEqual({});
  });

  it("pushes the other bound away so the range keeps a semitone of travel", () => {
    expect(reconcilePitchShiftParams({ semitones: 7, minSemitones: 7, maxSemitones: 5 }, "minSemitones")).toEqual({ maxSemitones: 8 });
    expect(reconcilePitchShiftParams({ semitones: 3, minSemitones: 3, maxSemitones: 3 }, "maxSemitones")).toEqual({ minSemitones: 2 });
  });

  it("gives way at the end of the scale rather than leave it", () => {
    expect(reconcilePitchShiftParams({ semitones: 12, minSemitones: 12, maxSemitones: 12 }, "minSemitones")).toEqual({ minSemitones: 11 });
    expect(reconcilePitchShiftParams({ semitones: -12, minSemitones: -12, maxSemitones: -12 }, "maxSemitones")).toEqual({ maxSemitones: -11 });
  });

  it("moves an unset semitone setting (0 st) into a range that excludes it", () => {
    expect(reconcilePitchShiftParams({ minSemitones: 3, maxSemitones: 7 }, "minSemitones")).toEqual({ semitones: 3 });
  });

  it("keeps a free value inside the range without rounding it", () => {
    expect(reconcilePitchShiftParams({ semitones: 7.6, minSemitones: 0, maxSemitones: 7, stepMode: 0 }, "maxSemitones")).toEqual({ semitones: 7 });
    expect(reconcilePitchShiftParams({ semitones: 3.3, minSemitones: 0, maxSemitones: 7, stepMode: 0 }, "maxSemitones")).toEqual({});
  });
});
