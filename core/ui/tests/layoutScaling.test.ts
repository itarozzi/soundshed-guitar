import { describe, expect, it } from "vitest";
import { computeLayoutScale } from "../ts/layoutScaling.js";

const design = { designWidth: 480, designHeight: 320 };

describe("computeLayoutScale with no scaling block (every layout made before it)", () => {
  it("draws at its own size, centred, when the panel is as wide or wider, and never grows", () => {
    for (const availableWidth of [480, 481, 1200, 5000]) {
      expect(computeLayoutScale({ ...design, availableWidth, availableHeight: 50 })).toEqual({ scale: 1, centred: true, overflows: false });
    }
  });

  it("shrinks to the panel's width, filling it, whatever height there is", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 240, availableHeight: 10 })).toEqual({ scale: 0.5, centred: false, overflows: false });
    expect(computeLayoutScale({ ...design, availableWidth: 479 })).toEqual({ scale: 479 / 480, centred: false, overflows: false });
  });
});

describe("computeLayoutScale with a scaling block", () => {
  it("grows to the panel's width up to maxScale, and centres once it stops", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 960, scaling: { maxScale: 4 } })).toEqual({ scale: 2, centred: false, overflows: false });
    expect(computeLayoutScale({ ...design, availableWidth: 960, scaling: { maxScale: 1.5 } })).toEqual({ scale: 1.5, centred: true, overflows: false });
  });

  it("in fit mode keeps the whole layout within the height too", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 960, availableHeight: 320, scaling: { mode: "fit", maxScale: 4 } }))
      .toEqual({ scale: 1, centred: true, overflows: false });
    expect(computeLayoutScale({ ...design, availableWidth: 960, availableHeight: 160, scaling: { mode: "fit" } }))
      .toEqual({ scale: 0.5, centred: true, overflows: false });
  });

  it("in fit mode with no height to go by, fits the width", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 240, scaling: { mode: "fit" } }).scale).toBe(0.5);
    expect(computeLayoutScale({ ...design, availableWidth: 240, availableHeight: 0, scaling: { mode: "fit" } }).scale).toBe(0.5);
  });

  it("stops shrinking at minScale and reports that it overflows", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 120, scaling: { minScale: 0.5 } })).toEqual({ scale: 0.5, centred: false, overflows: true });
  });

  it("in fixed mode always draws at its own size", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 240, scaling: { mode: "fixed" } })).toEqual({ scale: 1, centred: false, overflows: true });
    expect(computeLayoutScale({ ...design, availableWidth: 960, scaling: { mode: "fixed", maxScale: 3 } })).toEqual({ scale: 1, centred: true, overflows: false });
  });

  it("sits at the start when asked", () => {
    expect(computeLayoutScale({ ...design, availableWidth: 960, scaling: { align: "start" } })).toEqual({ scale: 1, centred: false, overflows: false });
  });

  it("ignores values a hand-edited or imported layout could hold", () => {
    const odd = { mode: "zoom", maxScale: Number.NaN, minScale: -2 } as unknown as Parameters<typeof computeLayoutScale>[0]["scaling"];
    expect(computeLayoutScale({ ...design, availableWidth: 960, scaling: odd })).toEqual({ scale: 1, centred: true, overflows: false });
    expect(computeLayoutScale({ ...design, availableWidth: 96000, scaling: { maxScale: 1000 } }).scale).toBe(8);
    expect(computeLayoutScale({ ...design, availableWidth: 120, scaling: { minScale: 2, maxScale: 1 } }).scale).toBe(1);
  });
});
