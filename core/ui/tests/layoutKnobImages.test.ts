import { describe, expect, it } from "vitest";
import { filmstripBackground, filmstripFrame, filmstripFrameCount, knobFaceAngle } from "../ts/layoutKnobImages.js";

describe("filmstrip frames", () => {
  it("accepts a whole number of frames from 2 to 1024 only", () => {
    expect(filmstripFrameCount(128)).toBe(128);
    for (const odd of [1, 0, -5, 2.5, 5000, "64", undefined, Number.NaN]) {
      expect(filmstripFrameCount(odd)).toBe(0);
    }
  });

  it("maps the knob's travel onto the frames, ends included", () => {
    expect(filmstripFrame(0, 101)).toBe(0);
    expect(filmstripFrame(0.5, 101)).toBe(50);
    expect(filmstripFrame(1, 101)).toBe(100);
    expect(filmstripFrame(1.7, 101)).toBe(100);
    expect(filmstripFrame(Number.NaN, 101)).toBe(0);
  });

  it("positions a vertical strip", () => {
    expect(filmstripBackground(0, 64)).toEqual({ size: "100% 6400%", position: "0% 0%" });
    expect(filmstripBackground(63, 64)).toEqual({ size: "100% 6400%", position: "0% 100%" });
    expect(filmstripBackground(21, 64).position).toBe(`0% ${Math.round((21 / 63) * 100 * 10000) / 10000}%`);
  });

  it("positions a sheet left to right, then down", () => {
    // 10 frames in 4 columns: rows of 4, 4, 2.
    expect(filmstripBackground(0, 10, 4)).toEqual({ size: "400% 300%", position: "0% 0%" });
    expect(filmstripBackground(5, 10, 4).position).toBe(`${Math.round((1 / 3) * 100 * 10000) / 10000}% 50%`);
    expect(filmstripBackground(9, 10, 4).position).toBe(`${Math.round((1 / 3) * 100 * 10000) / 10000}% 100%`);
  });

  it("treats a horizontal strip as one row", () => {
    expect(filmstripBackground(3, 4, 4)).toEqual({ size: "400% 100%", position: "100% 0%" });
  });
});

describe("knobFaceAngle", () => {
  it("turns -135 to 135 degrees by default, as a custom knob face always did", () => {
    expect(knobFaceAngle(0)).toBe(-135);
    expect(knobFaceAngle(0.5)).toBe(0);
    expect(knobFaceAngle(1)).toBe(135);
  });

  it("follows a face's own range", () => {
    expect(knobFaceAngle(0.5, -150, 150)).toBe(0);
    expect(knobFaceAngle(1, 0, 300)).toBe(300);
  });
});
