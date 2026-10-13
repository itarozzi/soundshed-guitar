import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import type * as AppSettingsStore from "../ts/appSettingsStore";
import type * as SignalFlow from "../ts/signalPath/signalFlow";
import type * as State from "../ts/state";
import type { SignalLevelDiagnostics, SignalLevelMetrics } from "../ts/types";

// signalPath/state.ts looks the chain element up when it loads, so the DOM has to
// exist before the module does.
document.body.innerHTML = `<div id="signal-path-nodes"></div>`;

let flow: typeof SignalFlow;
let state: typeof State;
let store: typeof AppSettingsStore;

beforeAll(async () => {
  flow = await import("../ts/signalPath/signalFlow");
  state = await import("../ts/state");
  store = await import("../ts/appSettingsStore");
});

const levels = (rmsDbfs: number): SignalLevelMetrics => ({
  peakDbfs: rmsDbfs + 6,
  rmsDbfs,
  headroomDb: -rmsDbfs - 6,
  clipped: false,
  clipCount: 0,
});

function diagnostics(inputDbfs: number, nodes: Record<string, number>): SignalLevelDiagnostics {
  return {
    input: levels(inputDbfs),
    output: levels(inputDbfs),
    nodes: Object.entries(nodes).map(([nodeId, rms]) => ({ scope: "preset", nodeId, nodeType: "x", levels: levels(rms) })),
  };
}

const connector = (from: string, to: string): string =>
  `<div class="signal-connector-wrapper" data-edge-from="${from}" data-edge-to="${to}"></div>`;

const liveIds = (): string[] => Array.from(document.querySelectorAll(".signal-live"))
  .map((element) => (element as HTMLElement).dataset.nodeId ?? `${(element as HTMLElement).dataset.edgeFrom}->`);

describe("updateSignalPathFlow", () => {
  beforeEach(() => {
    const root = document.getElementById("signal-path-nodes")!;
    // input -> drive -> split -> output, where the splitter reports no level of its own.
    root.innerHTML = [
      `<div class="signal-node" data-node-id="__input__"></div>`,
      connector("__input__", "drive"),
      `<div class="signal-node" data-node-id="drive"></div>`,
      connector("drive", "split"),
      `<div class="signal-node" data-node-id="split"></div>`,
      connector("split", "__output__"),
    ].join("");
    store.replaceAppSettings({});
    state.uiState.signalDiagnostics = null;
  });

  it("is off unless the setting is turned on", () => {
    expect(flow.isSignalFlowAnimationEnabled()).toBe(false);
    state.uiState.signalDiagnostics = diagnostics(-20, { drive: -18 });
    flow.updateSignalPathFlow();
    expect(liveIds()).toEqual([]);
  });

  it("lights what passes signal, and a node without a reading from its inputs", () => {
    store.replaceAppSettings({ [flow.SIGNAL_FLOW_ANIMATION_SETTING]: true });
    state.uiState.signalDiagnostics = diagnostics(-20, { drive: -18 });
    flow.updateSignalPathFlow();
    expect(liveIds().sort()).toEqual(["__input__", "__input__->", "drive", "drive->", "split", "split->"].sort());
  });

  it("leaves silent connectors dark", () => {
    store.replaceAppSettings({ [flow.SIGNAL_FLOW_ANIMATION_SETTING]: true });
    state.uiState.signalDiagnostics = diagnostics(-90, { drive: -90 });
    flow.updateSignalPathFlow();
    expect(liveIds()).toEqual([]);
  });

  it("clears the chain when the setting is turned off", () => {
    store.replaceAppSettings({ [flow.SIGNAL_FLOW_ANIMATION_SETTING]: true });
    state.uiState.signalDiagnostics = diagnostics(-20, { drive: -18 });
    flow.updateSignalPathFlow();
    expect(liveIds().length).toBeGreaterThan(0);

    store.replaceAppSettings({ [flow.SIGNAL_FLOW_ANIMATION_SETTING]: false });
    flow.updateSignalPathFlow();
    expect(liveIds()).toEqual([]);
  });
});
