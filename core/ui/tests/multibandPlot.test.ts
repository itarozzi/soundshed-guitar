import { describe, expect, it } from "vitest";
import {
  MB_MIN_SPLIT_RATIO,
  DepthDisplay,
  bandStates,
  crossoverAt,
  draggedCrossover,
  multibandPlotAxis,
  orderedCrossovers,
} from "../ts/multibandPlot.js";

describe("the multi-band effects' crossovers", () => {
  it("defaults where the engine does", () => {
    expect(orderedCrossovers({})).toEqual([200, 800, 3000]);
  });

  it("keeps crossovers set out of order a third of an octave apart, as the engine does", () => {
    const splits = orderedCrossovers({ crossover1: 1000, crossover2: 150, crossover3: 600 });
    expect(splits[0]).toBe(1000);
    expect(splits[1]).toBeCloseTo(1000 * MB_MIN_SPLIT_RATIO, 6);
    expect(splits[2]).toBeCloseTo(1000 * MB_MIN_SPLIT_RATIO ** 2, 6);
  });

  it("clamps a dragged crossover to its range and clear of its neighbours", () => {
    const splits = [200, 800, 3000];
    expect(draggedCrossover(1, 500, splits)).toBe(500);
    expect(draggedCrossover(1, 100, splits)).toBe(Math.round(200 * MB_MIN_SPLIT_RATIO));
    expect(draggedCrossover(1, 2900, splits)).toBe(Math.round(3000 / MB_MIN_SPLIT_RATIO / 10) * 10);
    expect(draggedCrossover(0, 10, splits)).toBe(40);
    expect(draggedCrossover(2, 20000, splits)).toBe(12000);
  });

  it("grabs the crossover under the pointer and nothing between them", () => {
    const axis = multibandPlotAxis(600);
    const splits = [200, 800, 3000];
    expect(crossoverAt(axis.toX(800) + 3, splits, axis)).toBe(1);
    expect(crossoverAt(axis.toX(200) - 5, splits, axis)).toBe(0);
    expect(crossoverAt(axis.toX(400), splits, axis)).toBe(-1);
  });
});

describe("how hard each band works, as drawn", () => {
  it("shows a peak at once and falls back steadily", () => {
    const display = new DepthDisplay();
    const band = (gainReductionDb: number) => ({ lowHz: null, highHz: null, gainReductionDb, levelDb: -20 });
    expect(display.update([band(12), band(0), band(0), band(0)], 1000)[0]).toBe(12);
    // A tenth of a second later with nothing new: 3 dB down at 30 dB/s.
    expect(display.update([band(0), band(0), band(0), band(0)], 1100)[0]).toBeCloseTo(9, 6);
    expect(display.update([band(10), band(0), band(0), band(0)], 1200)[0]).toBe(10);
    expect(display.update(undefined, 5000)[0]).toBe(0);
  });
});

describe("each band's On and Solo, as drawn", () => {
  it("has every band on and heard for an effect without them", () => {
    expect(bandStates({ drive1: 30 })).toEqual(Array(4).fill({ on: true, soloed: false, heard: true }));
  });

  it("hears only the soloed bands while any is soloed, whether or not they are on", () => {
    const states = bandStates({ on2: 0, solo2: 1, solo4: 1 });
    expect(states.map((state) => state.heard)).toEqual([false, true, false, true]);
    expect(states.map((state) => state.on)).toEqual([true, false, true, true]);
    expect(states.map((state) => state.soloed)).toEqual([false, true, false, true]);
  });
});
