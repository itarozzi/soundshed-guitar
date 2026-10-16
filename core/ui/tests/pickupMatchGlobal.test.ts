/**
 * The global chain's Pickup Match in the input levelling flyout: a tab beside the profiles,
 * built from Pickup Match's own pieces; on/off and choices sent as `pickup.<param>`; a picker's
 * popover not closing the flyout; and the control bar indicator. With its feature flag off, none
 * of that shows and a Pickup Match left on is switched off. Also the gate flyout, which now draws
 * its rows with the same shared code.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// The control bar pieces, as ui-components/input-control-group.html has them; the modules
// look them up on import.
document.body.innerHTML = `
  <div class="control-bar">
    <div class="vu-meter">
      <button id="user-input-calibration-toolbar-trigger" type="button" aria-expanded="false"></button>
      <div id="user-input-calibration-toolbar-menu" class="input-calibration-toolbar-menu" role="dialog" hidden></div>
    </div>
    <div class="control-stack-label">IN <button id="pickup-match-indicator" type="button" hidden></button></div>
    <input type="checkbox" id="gate-toggle" />
    <div class="knob" data-param="gate_threshold" data-value="-60"><div class="knob-indicator"></div></div>
    <span id="gate-threshold-value"></span>
    <button id="gate-settings-trigger" type="button"></button>
    <div id="gate-settings-flyout" hidden></div>
  </div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; path?: string; value?: number | boolean };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { uiState } = await import("../ts/state.js");
const { recordAppSetting, replaceAppSettings } = await import("../ts/appSettingsStore.js");
const calibration = await import("../ts/settings/inputCalibration.js");
const { syncPickupMatchFromState } = await import("../ts/pickupMatchGlobal.js");
const picker = await import("../ts/signalPath/paramsPanel/enumPicker.js");
const gate = await import("../ts/gateSettings.js");

const PICKUPS = ["Single Coil (Bridge)", "Custom Design", "Humbucker (Bridge)"];
const PICKUP_INFO = [
  { group: "Single Coil", subgroup: "Generic", icon: "pickup-single" },
  { group: "Custom", subgroup: "Custom", icon: "pickup-custom" },
  { group: "Humbucker", subgroup: "Generic", icon: "pickup-humbucker" },
];
const design = (side: string, group: string) => [
  { key: `${side}DesignType`, name: "Type", default: 2, min: 0, max: 2, unit: "enum", step: 1, group, advanced: true,
    labels: ["Strat-Style Single Coil", "Tele-Style Single Coil", "PAF-Style Humbucker"], control: "picker",
    labelInfo: [{ group: "Single Coil" }, { group: "Single Coil" }, { group: "Humbucker" }] },
  { key: `${side}Resistance`, name: "DC Resistance", default: 8.2, min: 3, max: 20, unit: "kΩ", step: 0.05, group, advanced: true },
];

EffectTypeRegistry.register(EffectGuids.kPickupMatch, {
  type: EffectGuids.kPickupMatch,
  displayName: "Pickup Match",
  category: "eq",
  requiresResource: false,
  parameters: [
    { key: "source", name: "Your Pickup", default: 0, min: 0, max: 2, unit: "enum", step: 1, group: "Pickups",
      labels: PICKUPS, labelInfo: PICKUP_INFO, control: "picker" },
    { key: "target", name: "Sound Like", default: 2, min: 0, max: 2, unit: "enum", step: 1, group: "Pickups",
      labels: PICKUPS, labelInfo: PICKUP_INFO, control: "picker" },
    { key: "amount", name: "Amount", default: 1, min: 0, max: 1, unit: "amount", group: "Pickups" },
    { key: "maxBoost", name: "Max Treble Boost", default: 12, min: 0, max: 24, unit: "dB", group: "Output", advanced: true },
    ...design("source", "Your Pickup Design"),
    ...design("target", "Sound Like Design"),
  ],
});
EffectTypeRegistry.register(EffectGuids.kDynamicsGate, {
  type: EffectGuids.kDynamicsGate,
  displayName: "Noise Gate",
  category: "dynamics",
  requiresResource: false,
  parameters: [
    { key: "threshold", name: "Threshold", default: -60, min: -80, max: 0, unit: "dB" },
    { key: "stereoLink", name: "Stereo Link", default: 1, min: 0, max: 1, unit: "", step: 1, labels: ["Independent", "Linked"] },
  ],
});

function pickupNode(): GraphNode {
  return { id: "global_pickup", type: EffectGuids.kPickupMatch, displayName: "Pickup Match", category: "eq",
    bypassed: true, params: { source: 0, target: 2, amount: 1 }, config: {} } as GraphNode;
}

const menu = () => document.getElementById("user-input-calibration-toolbar-menu") as HTMLElement;
const indicator = () => document.getElementById("pickup-match-indicator") as HTMLButtonElement;
const tab = (name: string) => menu().querySelector<HTMLButtonElement>(`.input-calibration-tabs [data-tab="${name}"]`);
const panel = (name: string) => menu().querySelector<HTMLElement>(`.input-calibration-tab-panel[data-tab="${name}"]`);
const sentPaths = () => sent.filter((m) => m.type === "setGlobalChainParam").map((m) => [m.path, m.value]);
const PICKUP_MATCH_FEATURE = "features.pickupMatch.enabled";

calibration.initUserInputCalibrationControls();
gate.initializeGateControls();

function useGlobalChainWith(node: GraphNode): void {
  uiState.globalSignalChain = {
    inputGain: 0, monoMode: true, inputChannel: 0, outputGain: 0, limiterEnabled: false,
    preChainGraph: { nodes: [node], edges: [] }, postChainGraph: { nodes: [], edges: [] },
  };
}

describe("Pickup Match in the input levelling flyout", () => {
  let node: GraphNode;

  beforeEach(() => {
    recordAppSetting(PICKUP_MATCH_FEATURE, true);
    picker.closeEnumPicker();
    calibration.closeUserInputCalibrationToolbarMenu();
    node = pickupNode();
    useGlobalChainWith(node);
    syncPickupMatchFromState();
    sent = [];
  });

  it("puts a Pickup Match tab beside the levelling profiles", () => {
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    expect(tab("calibration")?.getAttribute("aria-selected")).toBe("true");
    expect(panel("calibration")?.hidden).toBe(false);
    expect(panel("calibration")?.textContent).toContain("No levelling");
    expect(panel("pickup")?.hidden).toBe(true);

    tab("pickup")?.click();
    expect(panel("pickup")?.hidden).toBe(false);
    expect(panel("calibration")?.hidden).toBe(true);
    expect(menu().classList.contains("is-pickup-tab")).toBe(true);
    expect(menu().hidden).toBe(false);
  });

  it("builds the tab from Pickup Match's own pieces", () => {
    calibration.openUserInputCalibrationToolbarMenu("pickup");
    const root = panel("pickup") as HTMLElement;
    expect(root.querySelector(".pickup-response-canvas")).not.toBeNull();
    expect(root.querySelector(".enum-picker-trigger[data-param-key='source']")).not.toBeNull();
    expect(root.querySelector(".enum-picker-trigger[data-param-key='target'] .enum-picker-trigger-label")?.textContent)
      .toBe("Humbucker (Bridge)");
    expect(root.querySelector(".flyout-param-knob[data-param-key='amount']")).not.toBeNull();
    expect(root.querySelector(".flyout-param-knob[data-param-key='maxBoost']")).not.toBeNull();
    // Design controls only in their blocks, which stay hidden until a side is custom.
    const designKnob = root.querySelector(".flyout-param-knob[data-param-key='targetResistance']");
    expect(designKnob?.closest(".pickup-design")).not.toBeNull();
    expect((designKnob?.closest(".pickup-design") as HTMLElement).hidden).toBe(true);
  });

  it("switches it on and off as a global-chain param, and shows the indicator while on", () => {
    calibration.openUserInputCalibrationToolbarMenu("pickup");
    expect(indicator().hidden).toBe(true);
    const toggle = panel("pickup")?.querySelector<HTMLInputElement>(".pickup-match-enable-input") as HTMLInputElement;
    toggle.checked = true;
    toggle.dispatchEvent(new Event("change"));
    expect(sentPaths()).toContainEqual(["pickup.enabled", true]);
    expect(node.bypassed).toBe(false);
    expect(indicator().hidden).toBe(false);
    expect(indicator().title).toBe("Pickup Match on: Single Coil (Bridge) → Humbucker (Bridge)");
    expect(indicator().querySelector("svg")).not.toBeNull();

    toggle.checked = false;
    toggle.dispatchEvent(new Event("change"));
    expect(sentPaths()).toContainEqual(["pickup.enabled", false]);
    expect(indicator().hidden).toBe(true);
  });

  it("sends a picked pickup as pickup.<param>, keeps the flyout open, and shows a custom side's design", () => {
    calibration.openUserInputCalibrationToolbarMenu("pickup");
    panel("pickup")?.querySelector<HTMLButtonElement>(".enum-picker-trigger[data-param-key='target']")?.click();
    const popover = document.querySelector(".enum-picker-popover") as HTMLElement;
    expect(popover).not.toBeNull();

    // A press inside the picker's popover, which hangs from the body, must not close the flyout.
    popover.dispatchEvent(new MouseEvent("mousedown", { bubbles: true }));
    expect(menu().hidden).toBe(false);

    popover.querySelector<HTMLButtonElement>(".enum-picker-group-tile[data-group='']")?.click();
    Array.from(document.querySelectorAll<HTMLButtonElement>(".enum-picker-option"))
      .find((el) => el.textContent?.includes("Custom Design"))?.click();

    expect(sentPaths()).toContainEqual(["pickup.target", 1]);
    expect(sent.some((m) => m.type === "updateSignalPathNodeParam")).toBe(false);
    expect(node.params.target).toBe(1);
    expect(menu().hidden).toBe(false);
    expect(panel("pickup")?.querySelector<HTMLElement>(".pickup-design[data-side='target']")?.hidden).toBe(false);
  });

  it("opens the flyout on the Pickup Match tab from the indicator, and closes it again", () => {
    node.bypassed = false;
    syncPickupMatchFromState();
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    calibration.closeUserInputCalibrationToolbarMenu();

    indicator().click();
    expect(menu().hidden).toBe(false);
    expect(panel("pickup")?.hidden).toBe(false);

    indicator().dispatchEvent(new MouseEvent("mousedown", { bubbles: true }));
    indicator().click();
    expect(menu().hidden).toBe(true);
  });
});

// These run in order: the first is before the engine's settings arrive, and the rest after.
describe("Pickup Match with its feature flag off", () => {
  let node: GraphNode;

  beforeEach(() => {
    recordAppSetting(PICKUP_MATCH_FEATURE, false);
    picker.closeEnumPicker();
    calibration.closeUserInputCalibrationToolbarMenu();
    node = pickupNode();
    useGlobalChainWith(node);
    sent = [];
  });

  it("switches nothing off before the app settings arrive, as the flag reads off until then", () => {
    node.bypassed = false;
    syncPickupMatchFromState();
    expect(sentPaths()).toEqual([]);
    expect(node.bypassed).toBe(false);
  });

  it("switches a Pickup Match left on off, and hides the indicator", () => {
    replaceAppSettings({ ...uiState.appSettings, [PICKUP_MATCH_FEATURE]: false });
    node.bypassed = false;
    syncPickupMatchFromState();
    expect(sentPaths()).toEqual([["pickup.enabled", false]]);
    expect(node.bypassed).toBe(true);
    expect(indicator().hidden).toBe(true);
  });

  it("leaves the flyout to the input profiles, with no tab strip", () => {
    calibration.openUserInputCalibrationToolbarMenu("pickup");
    expect(menu().hidden).toBe(false);
    expect(menu().querySelector<HTMLElement>(".input-calibration-tabs")?.hidden).toBe(true);
    expect(panel("calibration")?.hidden).toBe(false);
    expect(panel("pickup")?.hidden).toBe(true);
    expect(menu().classList.contains("is-pickup-tab")).toBe(false);
    expect(sent).toEqual([]);
  });

  it("brings the tab strip back on an open flyout when the feature is switched on", () => {
    calibration.openUserInputCalibrationToolbarMenu("calibration");
    recordAppSetting(PICKUP_MATCH_FEATURE, true);
    calibration.syncUserInputCalibrationToolbarTabs();
    expect(menu().querySelector<HTMLElement>(".input-calibration-tabs")?.hidden).toBe(false);
    tab("pickup")?.click();
    expect(panel("pickup")?.hidden).toBe(false);

    recordAppSetting(PICKUP_MATCH_FEATURE, false);
    calibration.syncUserInputCalibrationToolbarTabs();
    expect(menu().querySelector<HTMLElement>(".input-calibration-tabs")?.hidden).toBe(true);
    expect(panel("calibration")?.hidden).toBe(false);
    expect(panel("pickup")?.hidden).toBe(true);
  });
});

describe("the gate flyout on the shared rows", () => {
  it("draws its rows with the shared code and sends gate.<param>", () => {
    uiState.globalSignalChain = {
      inputGain: 0, monoMode: true, inputChannel: 0, outputGain: 0, limiterEnabled: false,
      preChainGraph: {
        nodes: [{ id: "global_gate", type: EffectGuids.kDynamicsGate, displayName: "Noise Gate", category: "dynamics",
          bypassed: false, params: { threshold: -50, stereoLink: 1 }, config: {} } as GraphNode],
        edges: [],
      },
      postChainGraph: { nodes: [], edges: [] },
    };
    sent = [];
    document.getElementById("gate-settings-trigger")?.click();
    const flyout = document.getElementById("gate-settings-flyout") as HTMLElement;
    expect(flyout.hidden).toBe(false);
    expect(flyout.querySelector(".flyout-param-knob[data-param-key='threshold']")).not.toBeNull();
    const link = flyout.querySelector<HTMLInputElement>(".flyout-param-toggle[data-param-key='stereoLink']") as HTMLInputElement;
    expect(link.checked).toBe(true);
    link.checked = false;
    link.dispatchEvent(new Event("change"));
    expect(sentPaths()).toContainEqual(["gate.stereoLink", 0]);
    expect(flyout.querySelector(".flyout-param-value[data-param-key='stereoLink']")?.textContent).toBe("Independent");
  });
});
