/**
 * The DSP status readouts: the tiles the user picks (a × on each, a + that adds one), the choice
 * kept as an app setting, the output meter's flyout that holds the second readout, its loudness
 * watch, and the load peak's window.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as ReadoutModule from "../ts/dspStatusReadout.js";
import type * as StateModule from "../ts/state.js";

vi.mock("../ts/compactMode.js", () => ({ isCompact: () => true }));
vi.mock("../ts/settings.js", () => ({
  initSettingsPanel: vi.fn(),
  refreshTone3000ApiKeyField: vi.fn(),
  activateEquipmentTab: vi.fn(),
  activateLibraryTab: vi.fn(),
  activateAdvancedSubTab: vi.fn(),
  setSettingsViewStateSuppressed: vi.fn(),
}));
vi.mock("../ts/tone3000.js", () => ({ ensureTone3000Session: vi.fn() }));
vi.mock("../ts/jam.js", () => ({ handleJamPanelActivated: vi.fn(), initializeJamPanel: vi.fn() }));
vi.mock("../ts/toneSharingPanel.js", () => ({ initializeToneSharingPanel: vi.fn() }));
vi.stubGlobal("matchMedia", () => ({ matches: false, addEventListener: () => {}, removeEventListener: () => {} }));

const OUTPUT_GROUP = readFileSync(join(__dirname, "..", "ui-components", "output-control-group.html"), "utf8");

type SentMessage = { type: string; key?: string; value?: unknown; slot?: number; scope?: string };

let readout: typeof ReadoutModule;
let state: typeof StateModule;
let sent: SentMessage[];

/** Runs the animation frame a tile change waits for. */
function flushFrames(): void {
  vi.advanceTimersByTime(20);
}

function tileIds(container: ParentNode): string[] {
  return Array.from(container.querySelectorAll<HTMLElement>(".dsp-status-tile:not([hidden])")).map((tile) => tile.dataset.dspItem ?? "");
}

function menu(): HTMLElement | null {
  return document.querySelector<HTMLElement>(".dsp-status-add-menu");
}

function menuItems(): string[] {
  return Array.from(document.querySelectorAll<HTMLElement>(".dsp-status-add-menu [data-dsp-add-item]")).map((item) => item.dataset.dspAddItem ?? "");
}

beforeEach(async () => {
  vi.useFakeTimers();
  window.requestAnimationFrame = (callback: FrameRequestCallback) => window.setTimeout(() => callback(0), 0);
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  document.body.innerHTML = "";
  vi.resetModules();
  state = await import("../ts/state.js");
  state.uiState.appSettings = {};
  readout = await import("../ts/dspStatusReadout.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.useRealTimers();
});

/** A readout container the way an effect's panel draws one, bound to the effect readout. */
function mountEffectReadout(onChange: () => void = () => {}): HTMLElement {
  document.body.insertAdjacentHTML("beforeend", `<div id="readout"><div class="dsp-status-tiles">${readout.renderDspStatusTiles(readout.EFFECT_DSP_STATUS)}</div></div>`);
  const container = document.getElementById("readout") as HTMLElement;
  readout.bindDspStatusTiles(container, readout.EFFECT_DSP_STATUS, () => {
    container.querySelector(".dsp-status-tiles")!.innerHTML = readout.renderDspStatusTiles(readout.EFFECT_DSP_STATUS);
    onChange();
  });
  return container;
}

describe("the shown figures", () => {
  it("start as the readout's basic set, each with a × and a + after them", () => {
    const container = mountEffectReadout();
    expect(tileIds(container)).toEqual(["peak", "rms", "processing", "latency"]);
    expect(container.querySelectorAll("[data-dsp-remove]")).toHaveLength(4);
    expect(container.querySelector("[data-dsp-add]")).not.toBeNull();
  });

  it("follow the stored choice, keeping only what the readout can show, once each", () => {
    state.uiState.appSettings = { "ui.dspStatus.items": ["integrated", "dspLoad", "bogus", "integrated", "peak"] };
    expect(readout.shownDspStatusItems(readout.EFFECT_DSP_STATUS)).toEqual(["integrated", "peak"]);
    expect(readout.showsLoudness(readout.EFFECT_DSP_STATUS)).toBe(true);
  });

  it("can be emptied, which leaves only the +", () => {
    state.uiState.appSettings = { "ui.dspStatus.items": [] };
    const container = mountEffectReadout();
    expect(tileIds(container)).toEqual([]);
    expect(container.querySelector("[data-dsp-add]")).not.toBeNull();
  });

  it("lose the + once every figure is shown", () => {
    state.uiState.appSettings = { "ui.dspStatus.items": [...readout.EFFECT_DSP_STATUS.available] };
    const container = mountEffectReadout();
    expect(container.querySelector("[data-dsp-add]")).toBeNull();
  });
});

describe("removing a figure", () => {
  it("hides it at once, stores the choice, and redraws after the click", () => {
    const onChange = vi.fn();
    const container = mountEffectReadout(onChange);
    const remove = container.querySelector<HTMLButtonElement>('[data-dsp-remove="rms"]')!;

    remove.click();
    expect(remove.isConnected).toBe(true);
    expect(tileIds(container)).toEqual(["peak", "processing", "latency"]);
    expect(sent.at(-1)).toEqual({ type: "setSetting", key: "ui.dspStatus.items", value: ["peak", "processing", "latency"] });
    expect(onChange).not.toHaveBeenCalled();

    flushFrames();
    expect(onChange).toHaveBeenCalledTimes(1);
    expect(tileIds(container)).toEqual(["peak", "processing", "latency"]);
  });
});

describe("the + menu", () => {
  it("lists only what is not shown, by group, and hangs from the body", () => {
    const container = mountEffectReadout();
    container.querySelector<HTMLButtonElement>("[data-dsp-add]")!.click();

    expect(menu()?.hidden).toBe(false);
    expect(menu()?.parentElement).toBe(document.body);
    expect(menu()?.hasAttribute("data-control-bar-popover")).toBe(true);
    expect(menuItems()).toEqual(["headroom", "momentary", "shortTerm", "integrated"]);
    expect(Array.from(document.querySelectorAll(".dsp-status-add-heading")).map((h) => h.textContent)).toEqual(["Levels", "Loudness"]);
  });

  it("adds the chosen figure at the end, closing first so the click stays inside it", () => {
    const onChange = vi.fn();
    const container = mountEffectReadout(onChange);
    container.querySelector<HTMLButtonElement>("[data-dsp-add]")!.click();
    const option = document.querySelector<HTMLButtonElement>('[data-dsp-add-item="shortTerm"]')!;

    option.click();
    expect(option.isConnected).toBe(true);
    expect(menu()?.hidden).toBe(true);
    expect(sent.at(-1)).toEqual({ type: "setSetting", key: "ui.dspStatus.items", value: ["peak", "rms", "processing", "latency", "shortTerm"] });

    flushFrames();
    expect(onChange).toHaveBeenCalledTimes(1);
    expect(tileIds(container)).toEqual(["peak", "rms", "processing", "latency", "shortTerm"]);
  });

  it("closes on a press elsewhere, on Escape, and on a second press of the +", () => {
    const container = mountEffectReadout();
    const add = container.querySelector<HTMLButtonElement>("[data-dsp-add]")!;

    add.click();
    document.body.dispatchEvent(new Event("pointerdown", { bubbles: true }));
    expect(menu()?.hidden).toBe(true);

    add.click();
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
    expect(menu()?.hidden).toBe(true);

    add.click();
    expect(add.getAttribute("aria-expanded")).toBe("true");
    add.click();
    expect(menu()?.hidden).toBe(true);
    expect(add.getAttribute("aria-expanded")).toBe("false");
  });
});

describe("averages and formats", () => {
  it("average, and redraw at most twice a second", () => {
    const averages = new readout.DspStatusAverager();
    expect(averages.add("x", 10)).toBe(10);
    expect(averages.add("x", 20)).toBeCloseTo(10.1);
    expect(averages.add("x", null)).toBeNull();
    expect(averages.takeRenderTurn(1000)).toBe(true);
    expect(averages.takeRenderTurn(1200)).toBe(false);
    expect(averages.takeRenderTurn(1500)).toBe(true);
  });

  it("format each kind of figure, and a dash for none", () => {
    expect(readout.formatDspStatusTime(12.34, 100)).toBe("12.3 μs (12.3%)");
    expect(readout.formatDspStatusTime(12.34)).toBe("12.3 μs");
    expect(readout.formatDspStatusLatency(480, 48000)).toBe("480 smp (10.00 ms)");
    expect(readout.formatDspStatusLatency(0, 48000)).toBe("—");
    expect(readout.formatDspStatusLatency(0, 48000, true)).toBe("0 smp (0.00 ms)");
    expect(readout.formatDspStatusPercent(11.56)).toBe("11.6%");
    expect(readout.formatDspStatusLufs(-14.04)).toBe("-14.0 LUFS");
    expect(readout.formatDspStatusLufs(null)).toBe("—");
    expect(readout.integratedLoudnessTitle(83)).toContain("1:23");
  });
});

describe("the load peak", () => {
  it("is the worst frame peak of the last ten seconds", async () => {
    const perf = await import("../ts/dspPerformance.js");
    expect(perf.recentPeakDspLoadPercent(0)).toBeNull();
    perf.recordDspPeakSample(30, 1000);
    perf.recordDspPeakSample(12, 2000);
    perf.recordDspPeakSample(undefined, 2500);
    expect(perf.recentPeakDspLoadPercent(3000)).toBe(30);
    expect(perf.recentPeakDspLoadPercent(1000 + perf.DSP_PEAK_WINDOW_MS + 1)).toBe(12);
  });
});

describe("the output meter's readout", () => {
  async function mountOutput() {
    document.body.innerHTML = `<section class="control-bar"><div class="control-bar-group">${OUTPUT_GROUP}</div></section><button type="button" id="elsewhere"></button>`;
    const output = await import("../ts/outputStatus.js");
    output.initializeOutputStatusReadout();
    return output;
  }

  const trigger = () => document.getElementById("output-status-trigger") as HTMLButtonElement;
  const flyout = () => document.getElementById("output-status-flyout") as HTMLElement;
  const loudnessWatches = () => sent.filter((message) => message.type === "setLoudnessWatch");

  it("opens from the chevron under the mute button, hung from the body", async () => {
    await mountOutput();
    expect(trigger().previousElementSibling?.id).toBe("output-mute-toggle");
    expect(flyout().parentElement).toBe(document.body);

    trigger().click();
    expect(flyout().hidden).toBe(false);
    expect(trigger().getAttribute("aria-expanded")).toBe("true");
    expect(tileIds(flyout())).toEqual(["dspLoad", "dspPeak", "latency", "peak", "rms"]);
  });

  it("shows the output's levels and the whole chain's load, peak and latency", async () => {
    const output = await mountOutput();
    const perf = await import("../ts/dspPerformance.js");
    perf.recordDspPeakSample(42.5);
    state.uiState.dspPerformance = {
      totalProcessingTimeUs: 300,
      realTimeUs: 2666.7,
      dspLoadPercent: 11.25,
      nodeProcessingTimesUs: {},
      nodeLatencySamples: {},
      totalLatencySamples: 96,
      sampleRate: 48000,
    };
    state.uiState.signalDiagnostics = {
      rawInput: null,
      input: null,
      output: { peakDbfs: -6, rmsDbfs: -18, headroomDb: 6, clipped: false, clipCount: 0 },
      nodes: [],
    } as unknown as typeof state.uiState.signalDiagnostics;

    trigger().click();
    output.updateOutputStatusReadout();
    const value = (id: string) => flyout().querySelector(`[data-dsp-value="${id}"]`)?.textContent;
    expect(value("dspLoad")).toBe("11.3%");
    expect(value("dspPeak")).toBe("42.5%");
    expect(value("latency")).toBe("96 smp (2.00 ms)");
    expect(value("peak")).toBe("-6.0 dBFS");
    expect(value("rms")).toBe("-18.0 dBFS");
  });

  it("measures the output's loudness only while open with a loudness figure", async () => {
    await mountOutput();
    trigger().click();
    expect(loudnessWatches()).toEqual([]);

    flyout().querySelector<HTMLButtonElement>("[data-dsp-add]")!.click();
    document.querySelector<HTMLButtonElement>('[data-dsp-add-item="momentary"]')!.click();
    flushFrames();
    expect(loudnessWatches().at(-1)).toEqual({ type: "setLoudnessWatch", slot: 1, scope: "output" });

    document.getElementById("elsewhere")!.dispatchEvent(new Event("pointerdown", { bubbles: true }));
    expect(flyout().hidden).toBe(true);
    expect(loudnessWatches().at(-1)).toEqual({ type: "setLoudnessWatch", slot: 1 });
  });

  it("stays open while its + menu is used, and closes on Escape", async () => {
    await mountOutput();
    trigger().click();
    flyout().querySelector<HTMLButtonElement>("[data-dsp-add]")!.click();
    const option = document.querySelector<HTMLButtonElement>('[data-dsp-add-item="headroom"]')!;
    option.dispatchEvent(new Event("pointerdown", { bubbles: true }));
    option.click();
    flushFrames();
    expect(flyout().hidden).toBe(false);
    expect(tileIds(flyout())).toContain("headroom");

    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
    expect(flyout().hidden).toBe(true);
  });

  it("routes output frames to the output reading, apart from a node's", async () => {
    await mountOutput();
    const loudness = await import("../ts/nodeLoudness.js");
    loudness.setOutputLoudnessWatch(true);
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });

    expect(loudness.applyNodeLoudnessFrame({ type: "sldL", slot: 1, scope: "output", l: [-14, -15, -16, 3] })).toBe(true);
    expect(loudness.getOutputLoudness()?.momentaryLufs).toBe(-14);
    expect(loudness.getNodeLoudness()).toBeNull();

    expect(loudness.applyNodeLoudnessFrame({ type: "sldL", scope: "preset", id: "amp1", l: [-20, null, -21, 1] })).toBe(true);
    expect(loudness.getNodeLoudness()?.momentaryLufs).toBe(-20);
    expect(loudness.getOutputLoudness()?.momentaryLufs).toBe(-14);
  });
});

describe("in the compact layout", () => {
  it("keeps the controls sheet open while a figure is removed or added", async () => {
    window.IPlugSendMsg = () => {};
    document.body.innerHTML = `
      <section class="control-bar">
        <button type="button" data-control-bar-tab="preset"></button>
        <button type="button" data-control-bar-tab="controls"></button>
        <div class="control-bar-panel" id="control-bar-preset-panel"></div>
        <div class="control-bar-panel" id="control-bar-controls-panel"><div class="control-bar-group">${OUTPUT_GROUP}</div></div>
      </section>`;
    const { initializeControlBarTabs } = await import("../ts/navigation.js");
    initializeControlBarTabs();
    document.querySelector<HTMLButtonElement>('[data-control-bar-tab="controls"]')?.click();
    const bar = document.querySelector<HTMLElement>(".control-bar")!;
    const output = await import("../ts/outputStatus.js");
    output.initializeOutputStatusReadout();

    trigger().click();
    flyout().querySelector<HTMLButtonElement>('[data-dsp-remove="rms"]')!.click();
    expect(bar.dataset.compactControls).toBe("open");

    flushFrames();
    flyout().querySelector<HTMLButtonElement>("[data-dsp-add]")!.click();
    document.querySelector<HTMLButtonElement>('[data-dsp-add-item="momentary"]')!.click();
    expect(bar.dataset.compactControls).toBe("open");
  });

  function trigger(): HTMLButtonElement {
    return document.getElementById("output-status-trigger") as HTMLButtonElement;
  }

  function flyout(): HTMLElement {
    return document.getElementById("output-status-flyout") as HTMLElement;
  }
});
