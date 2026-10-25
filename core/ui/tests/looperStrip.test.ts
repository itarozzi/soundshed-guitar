/**
 * The looper strip: the Loop button appears with the Looper flag in the standalone app, the
 * engine is switched on and off with the flag, the strip sends the transport, the modes and the
 * layer edits, shows a count-in in large type, and collapses at compact density.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

document.body.innerHTML = `
  <div class="looper-strip" id="looper-strip" hidden></div>
  <button id="footer-looper-btn" type="button" hidden></button>
  <button id="footer-metronome-btn" type="button"></button>
`;

type Sent = Record<string, unknown> & { type: string };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { uiState } = await import("../ts/state.js");
const { FEATURE_FLAGS_CHANGED_EVENT } = await import("../ts/featureFlags.js");
const { initializeLooper } = await import("../ts/looper.js");
const { onLooperLayers, onLooperState } = await import("../ts/messages/looperHandlers.js");
const { showNotification } = await import("../ts/notifications.js");

const strip = document.getElementById("looper-strip") as HTMLElement;
const button = document.getElementById("footer-looper-btn") as HTMLButtonElement;

function setFlag(on: boolean): void {
  uiState.appSettings = { ...(uiState.appSettings ?? {}), "features.looper.enabled": on };
  document.dispatchEvent(new Event(FEATURE_FLAGS_CHANGED_EVENT));
}

function state(fields: Record<string, unknown> = {}, settings: Record<string, unknown> = {}): void {
  onLooperState({
    type: "looperState",
    available: true,
    enabled: true,
    state: "empty",
    playing: false,
    positionSec: 0,
    lengthSec: 0,
    canUndo: false,
    canRedo: false,
    layerCount: 0,
    memoryMb: 70,
    tempoLocked: false,
    settings: { enabled: true, start: "press", length: "free", bars: 4, countInBars: 1, ...settings },
    ...fields,
  });
}

initializeLooper();

describe("the looper strip", () => {
  beforeEach(() => {
    sent = [];
  });

  it("asks for the looper's state when it starts", () => {
    initializeLooper();
    expect(sent.some((m) => m.type === "getLooperState")).toBe(true);
  });

  it("switches the engine on with the flag, and shows the Loop button", () => {
    setFlag(true);
    state({ enabled: false }, { enabled: false });
    expect(sent).toContainEqual({ type: "setLooper", enabled: true });
    expect(button.hidden).toBe(false);
    expect(strip.hidden).toBe(true);
  });

  it("opens from the Loop button and sends the transport", () => {
    setFlag(true);
    state();
    button.click();
    expect(strip.hidden).toBe(false);
    const rec = strip.querySelector<HTMLButtonElement>('[data-looper-command="recPlayDub"]');
    expect(rec?.textContent).toBe("Rec");
    rec?.click();
    expect(sent).toContainEqual({ type: "looperCommand", command: "recPlayDub" });
  });

  it("sends the record modes and the bar count", () => {
    state({}, { length: "bars", bars: 4 });
    strip.querySelector<HTMLButtonElement>('[data-looper-set="start"][data-value="armed"]')?.click();
    strip.querySelector<HTMLButtonElement>('[data-looper-bars="1"]')?.click();
    expect(sent).toContainEqual({ type: "setLooper", start: "armed" });
    expect(sent).toContainEqual({ type: "setLooper", bars: 5 });
  });

  it("shows the count-in in large type and the state the loop is in", () => {
    state({ state: "countIn", countInBeatsLeft: 3 }, { length: "bars" });
    expect(strip.querySelector(".looper-countin")?.textContent).toBe("3");
    state({ state: "playing", playing: true, lengthSec: 8, positionSec: 2, bars: 4, barIndex: 2, layerCount: 1 });
    expect(strip.dataset.looperState).toBe("playing");
    expect(strip.querySelector(".looper-time")?.textContent).toBe("0:02.0 / 0:08.0");
    expect(strip.querySelector(".looper-bar")?.textContent).toBe("bar 2 of 4");
    expect(strip.querySelector('[data-looper-command="recPlayDub"]')?.textContent).toBe("Dub");
  });

  it("edits a layer from the layers panel", () => {
    state({ state: "playing", playing: true, lengthSec: 8, layerCount: 1 });
    onLooperLayers({
      type: "looperLayers",
      layers: [{ id: "L1", title: "Layer 1", order: 1, level: 1, balance: 0, muted: false, solo: false, tone: { presetName: "Clean <Amp>" } }],
    });
    strip.querySelector<HTMLButtonElement>("[data-looper-layers]")?.click();
    const row = strip.querySelector<HTMLElement>('.looper-layer-row[data-layer-id="L1"]');
    expect(row?.querySelector(".looper-tone-chip")?.innerHTML).toContain("Clean &lt;Amp&gt;");
    row?.querySelector<HTMLButtonElement>("[data-looper-layer-mute]")?.click();
    const level = row?.querySelector<HTMLInputElement>("[data-looper-layer-level]");

    if (level) {
      level.value = "0.5";
      level.dispatchEvent(new Event("input", { bubbles: true }));
    }

    row?.querySelector<HTMLButtonElement>("[data-looper-layer-remove]")?.click();
    expect(sent).toContainEqual({ type: "setLooperLayer", layerId: "L1", muted: true });
    expect(sent).toContainEqual({ type: "setLooperLayer", layerId: "L1", level: 0.5 });
    expect(sent).toContainEqual({ type: "removeLooperLayer", layerId: "L1" });
  });

  it("explains a refused press once", () => {
    state({ notice: "noSpare" });
    state({ notice: "noSpare" });
    expect(vi.mocked(showNotification)).toHaveBeenCalledTimes(1);
  });

  it("switches the engine off and hides everything with the flag", () => {
    setFlag(false);
    state();
    expect(sent).toContainEqual({ type: "setLooper", enabled: false });
    expect(button.hidden).toBe(true);
    expect(strip.hidden).toBe(true);
  });
});
