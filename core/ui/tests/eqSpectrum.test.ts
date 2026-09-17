import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  EQ_CURVE_MAX_FREQ,
  EQ_CURVE_MIN_FREQ,
  EQ_SPECTRUM_DISPLAY_CEILING_DB,
  EQ_SPECTRUM_DISPLAY_FLOOR_DB,
  anchoredFrequencyAxis,
  eqCurveFreqToX,
  eqSpectrumPlotPoints,
  logFrequencyAxis,
} from "../ts/eqPlot.js";
import type { EqSpectrum } from "../ts/eqSpectrum.js";
import type * as EqSpectrumModule from "../ts/eqSpectrum.js";

type SpectrumModule = typeof EqSpectrumModule;
type SentMessage = { type: string; scope?: string; nodeId?: string; presetId?: string };

const BIN_COUNT = 128;
const RANGE = [20, 20000, -96, 0];

let spectrum: SpectrumModule;
let sent: SentMessage[];

/** A frame as the backend sends it: every bin at `db`. */
function frame(id: string, scope = "post", db = -30, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return { type: "sldS", scope, id, r: RANGE, s: new Array(BIN_COUNT).fill(db), ...extra };
}

function watches(): SentMessage[] {
  return sent.filter((message) => message.type === "setSpectrumWatch");
}

beforeEach(async () => {
  vi.useFakeTimers();
  // Frames are delivered on the next animation frame; run it straight away.
  window.requestAnimationFrame = (callback: FrameRequestCallback) => {
    callback(0);
    return 0;
  };
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  // The module holds the one backend watch for the whole UI, so every test gets a fresh copy.
  vi.resetModules();
  spectrum = await import("../ts/eqSpectrum.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.useRealTimers();
});

describe("the spectrum watch", () => {
  it("asks the backend to tap the subscribed node", () => {
    spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1", presetId: "p7" }, () => {});
    expect(watches()).toEqual([{ type: "setSpectrumWatch", scope: "preset", nodeId: "eq1", presetId: "p7" }]);
  });

  it("delivers frames for the watched node and ignores others", () => {
    const listener = vi.fn();
    spectrum.subscribeEqSpectrum({ scope: "post", nodeId: "global_eq" }, listener);

    expect(spectrum.applyEqSpectrumFrame(frame("some_other_node"))).toBe(false);
    expect(spectrum.applyEqSpectrumFrame(frame("global_eq", "pre"))).toBe(false);
    expect(listener).not.toHaveBeenCalled();

    expect(spectrum.applyEqSpectrumFrame(frame("global_eq"))).toBe(true);
    expect(listener).toHaveBeenCalledTimes(1);
    const delivered = listener.mock.calls[0][0] as EqSpectrum;
    expect(delivered.binsDb).toHaveLength(BIN_COUNT);
    expect(delivered).toMatchObject({ minFrequencyHz: 20, maxFrequencyHz: 20000, floorDb: -96, ceilingDb: 0 });
  });

  it("accepts the preset id the backend filled in when the UI named none", () => {
    const listener = vi.fn();
    spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1" }, listener);
    expect(spectrum.applyEqSpectrumFrame(frame("eq1", "preset", -30, { presetId: "active" }))).toBe(true);
    expect(listener).toHaveBeenCalledTimes(1);
  });

  it("rejects a frame from a different preset", () => {
    spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1", presetId: "a" }, () => {});
    expect(spectrum.applyEqSpectrumFrame(frame("eq1", "preset", -30, { presetId: "b" }))).toBe(false);
  });

  it("rejects malformed frames", () => {
    spectrum.subscribeEqSpectrum({ scope: "post", nodeId: "global_eq" }, () => {});
    expect(spectrum.applyEqSpectrumFrame({ ...frame("global_eq"), s: "nope" })).toBe(false);
    expect(spectrum.applyEqSpectrumFrame({ ...frame("global_eq"), r: [20, 20000] })).toBe(false);
    expect(spectrum.applyEqSpectrumFrame({ ...frame("global_eq"), r: [20000, 20, -96, 0] })).toBe(false);
  });

  it("replaces a non-numeric bin with the floor rather than dropping the frame", () => {
    const listener = vi.fn();
    spectrum.subscribeEqSpectrum({ scope: "post", nodeId: "global_eq" }, listener);
    const bins = new Array(BIN_COUNT).fill(-20);
    bins[3] = null;
    spectrum.applyEqSpectrumFrame({ ...frame("global_eq"), s: bins });
    expect((listener.mock.calls[0][0] as EqSpectrum).binsDb[3]).toBe(-96);
  });

  it("hands the feed to the newest subscriber and back again", () => {
    const panel = vi.fn();
    const modal = vi.fn();
    spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1" }, panel);
    spectrum.applyEqSpectrumFrame(frame("eq1", "preset"));
    expect(panel).toHaveBeenLastCalledWith(expect.objectContaining({ binsDb: expect.any(Array) }));

    // The Global EQ modal opens over the node's panel: the panel is told it has nothing.
    const closeModal = spectrum.subscribeEqSpectrum({ scope: "post", nodeId: "global_eq" }, modal);
    expect(panel).toHaveBeenLastCalledWith(null);
    expect(watches().at(-1)).toMatchObject({ scope: "post", nodeId: "global_eq" });
    expect(spectrum.applyEqSpectrumFrame(frame("eq1", "preset"))).toBe(false);
    spectrum.applyEqSpectrumFrame(frame("global_eq"));
    expect(modal).toHaveBeenCalledTimes(1);

    // The modal closes: the panel's node is watched again.
    closeModal();
    expect(watches().at(-1)).toMatchObject({ scope: "preset", nodeId: "eq1" });
    panel.mockClear();
    spectrum.applyEqSpectrumFrame(frame("eq1", "preset"));
    expect(panel).toHaveBeenCalledTimes(1);
    expect(modal).toHaveBeenCalledTimes(1);
  });

  it("does not stop the tap when a panel rebuilds and subscribes again", () => {
    const unsubscribe = spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1" }, () => {});
    spectrum.applyEqSpectrumFrame(frame("eq1", "preset"));
    unsubscribe();

    // The rebuilt panel gets the last spectrum straight away, with no gap to fill.
    const rebuilt = vi.fn();
    spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1" }, rebuilt);
    expect(rebuilt).toHaveBeenCalledTimes(1);

    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_RELEASE_DELAY_MS * 2);
    expect(watches()).toHaveLength(1);
  });

  it("stops the tap once nothing is subscribed", () => {
    const unsubscribe = spectrum.subscribeEqSpectrum({ scope: "preset", nodeId: "eq1" }, () => {});
    unsubscribe();
    expect(watches()).toHaveLength(1);

    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_RELEASE_DELAY_MS);
    expect(watches()).toEqual([
      { type: "setSpectrumWatch", scope: "preset", nodeId: "eq1" },
      { type: "setSpectrumWatch" },
    ]);

    // And the renewals stop with it.
    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_RENEW_MS * 3);
    expect(watches()).toHaveLength(2);
  });

  it("renews the lease while the watch is held", () => {
    spectrum.subscribeEqSpectrum({ scope: "post", nodeId: "global_eq" }, () => {});
    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_RENEW_MS * 3);
    expect(watches()).toHaveLength(4);
    expect(new Set(watches().map((message) => message.nodeId))).toEqual(new Set(["global_eq"]));
  });

  it("clears a spectrum that stops arriving", () => {
    const listener = vi.fn();
    spectrum.subscribeEqSpectrum({ scope: "post", nodeId: "global_eq" }, listener);
    spectrum.applyEqSpectrumFrame(frame("global_eq"));

    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_STALE_MS - 1);
    expect(listener).toHaveBeenCalledTimes(1);
    vi.advanceTimersByTime(1);
    expect(listener).toHaveBeenLastCalledWith(null);
  });
});

describe("EqSpectrumWatcher", () => {
  type ObserverCallback = (entries: Array<{ isIntersecting: boolean }>) => void;
  let observers: Array<{ callback: ObserverCallback; disconnected: boolean }>;

  beforeEach(() => {
    observers = [];
    vi.stubGlobal("IntersectionObserver", class {
      private readonly entry: { callback: ObserverCallback; disconnected: boolean };
      constructor(callback: ObserverCallback) {
        this.entry = { callback, disconnected: false };
        observers.push(this.entry);
      }
      observe(): void {}
      disconnect(): void {
        this.entry.disconnected = true;
      }
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("only watches while its element is on screen", () => {
    const listener = vi.fn();
    const canvas = document.createElement("canvas");
    const watcher = new spectrum.EqSpectrumWatcher(canvas, () => ({ scope: "post", nodeId: "global_eq" }), listener);
    expect(watches()).toHaveLength(0);

    observers[0].callback([{ isIntersecting: true }]);
    expect(watches()).toHaveLength(1);
    spectrum.applyEqSpectrumFrame(frame("global_eq"));
    expect(watcher.spectrum?.binsDb).toHaveLength(BIN_COUNT);

    // Hidden: the curve is cleared at once and the tap let go shortly after.
    observers[0].callback([{ isIntersecting: false }]);
    expect(listener).toHaveBeenLastCalledWith(null);
    expect(watcher.spectrum).toBeNull();
    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_RELEASE_DELAY_MS);
    expect(watches().at(-1)).toEqual({ type: "setSpectrumWatch" });
  });

  it("asks for its source each time it comes into view", () => {
    let nodeId = "eq_a";
    const canvas = document.createElement("canvas");
    new spectrum.EqSpectrumWatcher(canvas, () => ({ scope: "preset", nodeId }), () => {});
    observers[0].callback([{ isIntersecting: true }]);
    observers[0].callback([{ isIntersecting: false }]);
    nodeId = "eq_b";
    observers[0].callback([{ isIntersecting: true }]);
    expect(watches().at(-1)).toMatchObject({ nodeId: "eq_b" });
  });

  it("shows nothing for a source that resolves to null", () => {
    const canvas = document.createElement("canvas");
    new spectrum.EqSpectrumWatcher(canvas, () => null, () => {});
    observers[0].callback([{ isIntersecting: true }]);
    expect(watches()).toHaveLength(0);
  });

  it("goes quiet for good once destroyed", () => {
    const listener = vi.fn();
    const canvas = document.createElement("canvas");
    const watcher = new spectrum.EqSpectrumWatcher(canvas, () => ({ scope: "post", nodeId: "global_eq" }), listener);
    observers[0].callback([{ isIntersecting: true }]);
    watcher.destroy();
    expect(observers[0].disconnected).toBe(true);

    spectrum.applyEqSpectrumFrame(frame("global_eq"));
    observers[0].callback([{ isIntersecting: true }]);
    expect(listener).not.toHaveBeenCalled();
    vi.advanceTimersByTime(spectrum.EQ_SPECTRUM_RELEASE_DELAY_MS);
    expect(watches().at(-1)).toEqual({ type: "setSpectrumWatch" });
  });
});

describe("spectrum placement on the EQ plot", () => {
  const spectrumAt = (binsDb: number[]): EqSpectrum => ({
    binsDb, minFrequencyHz: 20, maxFrequencyHz: 20000, floorDb: -96, ceilingDb: 0,
  });

  it("puts each bin under the band handle for the same frequency", () => {
    // 128 bins over 20 Hz-20 kHz: bin i sits at 20 * 1000^(i/127).
    const bins = new Array(BIN_COUNT).fill(-40);
    const points = eqSpectrumPlotPoints(spectrumAt(bins), logFrequencyAxis(8, 600), 8, 200);
    expect(points).toHaveLength(BIN_COUNT);
    expect(points[0].x).toBeCloseTo(eqCurveFreqToX(EQ_CURVE_MIN_FREQ, 8, 600));
    expect(points[BIN_COUNT - 1].x).toBeCloseTo(eqCurveFreqToX(EQ_CURVE_MAX_FREQ, 8, 600));
    const bin = 42;
    const binHz = 20 * Math.pow(1000, bin / (BIN_COUNT - 1));
    expect(points[bin].x).toBeCloseTo(eqCurveFreqToX(binHz, 8, 600));
    expect(eqCurveFreqToX(1000, 0, 300)).toBeCloseTo(300 * Math.log10(50) / 3);
  });

  it("maps the display range onto the plot height and clamps outside it", () => {
    const bins = [EQ_SPECTRUM_DISPLAY_FLOOR_DB - 20, EQ_SPECTRUM_DISPLAY_FLOOR_DB, EQ_SPECTRUM_DISPLAY_CEILING_DB, 12];
    const points = eqSpectrumPlotPoints(spectrumAt(bins), logFrequencyAxis(0, 100), 10, 200);
    expect(points.map((point) => point.y)).toEqual([210, 210, 10, 10]);
  });

  it("inverts the standard axis", () => {
    const axis = logFrequencyAxis(8, 600);
    for (const freq of [20, 82.4, 1000, 12345, 20000]) {
      expect(axis.toFreq(axis.toX(freq))).toBeCloseTo(freq, 6);
    }
  });

  it("lines a graphic EQ's evenly spaced sliders up with their frequencies", () => {
    // Five sliders 100 px apart, as the flex row lays them out.
    const anchors = [60, 250, 1000, 4000, 10000].map((freq, index) => ({ freq, x: 50 + index * 100 }));
    const axis = anchoredFrequencyAxis(anchors)!;
    expect(axis).not.toBeNull();
    anchors.forEach(({ freq, x }) => {
      expect(axis.toX(freq)).toBeCloseTo(x);
      expect(axis.toFreq(x)).toBeCloseTo(freq);
    });
    // Halfway between two sliders is halfway in octaves.
    expect(axis.toX(500)).toBeCloseTo(200);
    // Past the ends it carries on at the end segments' rate.
    expect(axis.toX(30)).toBeCloseTo(50 - 100 * Math.log10(2) / Math.log10(250 / 60));
    expect(axis.toX(20000)).toBeCloseTo(450 + 100 * Math.log10(2) / Math.log10(10000 / 4000));
    expect(axis.toFreq(axis.toX(15))).toBeCloseTo(15);

    const bins = new Array(BIN_COUNT).fill(-40);
    const points = eqSpectrumPlotPoints(spectrumAt(bins), axis, 0, 100);
    const bin1k = 72; // 20 * 1000^(72/127) is within 1% of 1 kHz
    expect(points[bin1k].x).toBeCloseTo(axis.toX(20 * Math.pow(1000, bin1k / (BIN_COUNT - 1))));
    expect(Math.abs(points[bin1k].x - 250)).toBeLessThan(2);
  });

  it("refuses anchors that cannot make an axis", () => {
    expect(anchoredFrequencyAxis([])).toBeNull();
    expect(anchoredFrequencyAxis([{ freq: 100, x: 10 }])).toBeNull();
    // Sliders that wrapped onto a second row, or frequencies out of order.
    expect(anchoredFrequencyAxis([{ freq: 100, x: 10 }, { freq: 1000, x: 10 }])).toBeNull();
    expect(anchoredFrequencyAxis([{ freq: 1000, x: 10 }, { freq: 100, x: 50 }])).toBeNull();
    expect(anchoredFrequencyAxis([{ freq: 100, x: 10 }, { freq: Number.NaN, x: 30 }, { freq: 400, x: 50 }])).not.toBeNull();
  });

  it("draws nothing from fewer than two bins", () => {
    expect(eqSpectrumPlotPoints(spectrumAt([-20]), logFrequencyAxis(0, 100), 0, 100)).toEqual([]);
  });
});
