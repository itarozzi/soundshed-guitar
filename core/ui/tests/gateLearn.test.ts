/**
 * The noise gate's Learn in the global gate's flyout: the button switches a bypassed gate on
 * and asks the engine to listen, a finished listen sets Threshold as a turned knob would, and a
 * listen that heard playing leaves Threshold alone.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

document.body.innerHTML = `
  <div class="control-bar">
    <input type="checkbox" id="gate-toggle" />
    <div id="gate-threshold-control">
      <div class="knob" data-param="gate_threshold" data-value="-60"><div class="knob-indicator"></div></div>
      <span id="gate-threshold-value"></span>
      <button id="gate-settings-trigger" type="button"></button>
    </div>
    <div id="gate-settings-flyout" hidden></div>
  </div>
`;
type Sent = { type: string; path?: string; value?: number | boolean; action?: string; scope?: string; nodeId?: string };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { uiState } = await import("../ts/state.js");
const { onGateLearnStatus } = await import("../ts/gateLearn.js");
const gate = await import("../ts/gateSettings.js");

EffectTypeRegistry.register(EffectGuids.kDynamicsGate, {
  type: EffectGuids.kDynamicsGate,
  displayName: "Noise Gate",
  category: "dynamics",
  requiresResource: false,
  parameters: [
    { key: "threshold", name: "Threshold", default: -60, min: -80, max: 0, unit: "dB", step: 0.1 },
    { key: "hysteresis", name: "Hysteresis", default: 4, min: 0, max: 24, unit: "dB", step: 0.1, advanced: true },
  ],
});

gate.initializeGateControls();

describe("Learn in the global gate's flyout", () => {
  let node: GraphNode;
  let flyout: HTMLElement;

  beforeEach(() => {
    node = { id: "global_gate", type: EffectGuids.kDynamicsGate, displayName: "Noise Gate", category: "dynamics",
      bypassed: true, params: { threshold: -50, hysteresis: 4 }, config: {} } as GraphNode;
    uiState.globalSignalChain = {
      inputGain: 0, monoMode: true, inputChannel: 0, outputGain: 0, limiterEnabled: false,
      preChainGraph: { nodes: [node], edges: [] },
      postChainGraph: { nodes: [], edges: [] },
    };
    flyout = document.getElementById("gate-settings-flyout") as HTMLElement;
    if (flyout.hidden) {
      document.getElementById("gate-settings-trigger")?.click();
    }
    sent = [];
  });

  const button = () => flyout.querySelector<HTMLButtonElement>(".gate-learn-button") as HTMLButtonElement;
  const status = () => flyout.querySelector(".gate-learn-status")?.textContent ?? "";

  it("switches a bypassed gate on and starts listening", () => {
    button().click();
    expect(sent).toContainEqual({ type: "setGlobalChainParam", path: "gate.enabled", value: true });
    expect(sent).toContainEqual(expect.objectContaining({ type: "gateLearn", action: "start", scope: "global", nodeId: "global_gate" }));
    expect(sent.findIndex((message) => message.path === "gate.enabled"))
      .toBeLessThan(sent.findIndex((message) => message.type === "gateLearn"));
    expect(button().textContent).toBe("Cancel");
    expect(flyout.querySelector(".gate-learn")?.classList.contains("is-listening")).toBe(true);

    onGateLearnStatus({ type: "gateLearnStatus", scope: "global", nodeId: "global_gate", state: "listening", progress: 0.5, levelDb: -70 });
    const meter = flyout.querySelector<HTMLElement>(".gate-learn-meter") as HTMLElement;
    expect(meter.style.getPropertyValue("--gate-learn-progress")).toBe("50.0%");

    sent = [];
    button().click();
    expect(sent).toContainEqual({ type: "gateLearn", action: "cancel" });
  });

  it("sets Threshold from a finished listen", () => {
    onGateLearnStatus({ type: "gateLearnStatus", scope: "global", nodeId: "global_gate", state: "done",
      noiseFloorDb: -71.6, thresholdDb: -64.6 });
    expect(sent).toContainEqual({ type: "setGlobalChainParam", path: "gate.threshold", value: -64.6 });
    expect(node.params.threshold).toBe(-64.6);
    expect(status()).toContain("threshold set to");
    expect(button().textContent).toBe("Learn");
    const meter = flyout.querySelector<HTMLElement>(".gate-learn-meter") as HTMLElement;
    // -64.6 dB on a -80..0 scale, and the close level 4 dB under it.
    expect(meter.style.getPropertyValue("--gate-learn-threshold")).toBe("19.25%");
    expect(meter.style.getPropertyValue("--gate-learn-close")).toBe("14.25%");
  });

  it("leaves Threshold alone when it heard playing", () => {
    onGateLearnStatus({ type: "gateLearnStatus", scope: "global", nodeId: "global_gate", state: "failed",
      reason: "playing", noiseFloorDb: -20 });
    expect(sent.some((message) => message.path === "gate.threshold")).toBe(false);
    expect(node.params.threshold).toBe(-50);
    expect(status()).toContain("Heard playing");
    expect(flyout.querySelector(".gate-learn")?.classList.contains("is-failed")).toBe(true);
  });
});

describe("Learn on a Multi-band Gate", () => {
  const target = { scope: "preset" as const, nodeId: "mbg", presetId: "p" };
  const labels = ["Lows", "Low Mids", "Mids", "Upper Mids", "Presence", "Air"];
  let thresholds: number[];
  let applied: { thresholdDb?: number }[] | undefined;
  let container: HTMLElement;

  beforeEach(async () => {
    const learn = await import("../ts/gateLearn.js");
    thresholds = [-65, -65, -65, -65, -65, -65];
    applied = undefined;
    learn.registerGateLearnApplier("preset", (_target, status) => {
      applied = status.bands;
    });
    container = document.createElement("div");
    container.innerHTML = learn.gateLearnMeterHtml(target, labels);
    document.body.appendChild(container);
    learn.bindGateLearnView(container, {
      target,
      threshold: () => thresholds[0],
      hysteresis: () => 0,
      bandThresholds: () => thresholds,
      range: () => ({ min: -96, max: 0 }),
      ensureEnabled: () => {},
    });
    sent = [];
  });

  const meters = () => Array.from(container.querySelectorAll<HTMLElement>(".gate-learn-meter"));

  it("draws one labelled meter per band under one Learn button", () => {
    expect(meters()).toHaveLength(6);
    expect(container.querySelectorAll(".gate-learn-button")).toHaveLength(1);
    expect(meters()[5].getAttribute("aria-label")).toContain("Air: threshold");
  });

  it("shows each band's live level while listening, and applies all six thresholds when done", () => {
    container.querySelector<HTMLButtonElement>(".gate-learn-button")?.click();
    expect(sent).toContainEqual(expect.objectContaining({ type: "gateLearn", action: "start", scope: "preset", nodeId: "mbg" }));

    onGateLearnStatus({ type: "gateLearnStatus", scope: "preset", presetId: "p", nodeId: "mbg", state: "listening",
      progress: 0.25, bands: labels.map((_label, index) => ({ levelDb: -96 + 12 * index })) });
    expect(meters()[0].style.getPropertyValue("--gate-learn-level")).toBe("0.00%");
    expect(meters()[4].style.getPropertyValue("--gate-learn-level")).toBe("50.00%");

    const bands = labels.map((_label, index) => ({ noiseFloorDb: -100 + 4 * index, spreadDb: 1, thresholdDb: -92 + 4 * index }));
    onGateLearnStatus({ type: "gateLearnStatus", scope: "preset", presetId: "p", nodeId: "mbg", state: "done", bands });
    expect(applied?.map((band) => band.thresholdDb)).toEqual(bands.map((band) => band.thresholdDb));
    expect(meters()[5].classList.contains("has-noise-floor")).toBe(true);
    expect(container.querySelector(".gate-learn-status")?.textContent).toContain("each band's threshold set");
  });

  it("applies nothing when it heard playing", () => {
    onGateLearnStatus({ type: "gateLearnStatus", scope: "preset", presetId: "p", nodeId: "mbg", state: "failed",
      reason: "playing", bands: labels.map(() => ({ noiseFloorDb: -20, spreadDb: 30 })) });
    expect(applied).toBeUndefined();
    expect(container.querySelector(".gate-learn-status")?.textContent).toContain("Heard playing");
  });
});
