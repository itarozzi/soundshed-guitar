/**
 * Pickup Match's custom design in the params panel: the design parameters stay out of the
 * generic controls and live in a block per side, shown only while that side is Custom Design;
 * the block's type is a picker; and the figures the engine sends with the curve are shown.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; requestId?: string; nodeId?: string; paramKey?: string; value?: number };
let sent: Sent[] = [];
const allSent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  const message = JSON.parse(payload) as Sent;
  sent.push(message);
  allSent.push(message);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const picker = await import("../ts/signalPath/paramsPanel/enumPicker.js");
const { applyEffectResponse } = await import("../ts/effectResponse.js");
const { describeDesign, PICKUP_DESIGN_PARAM_KEYS } = await import("../ts/signalPath/paramsPanel/pickupResponse.js");

const PICKUPS = ["Single Coil (Neck)", "Custom Design", "Humbucker (Bridge)"];
const PICKUP_INFO = [
  { group: "Single Coil", subgroup: "Generic", icon: "pickup-single" },
  { group: "Custom", subgroup: "Custom", icon: "pickup-custom" },
  { group: "Humbucker", subgroup: "Generic", icon: "pickup-humbucker" },
];
const TYPES = ["Strat-Style Single Coil", "Tele-Style Single Coil", "PAF-Style Humbucker"];
const TYPE_INFO = [
  { group: "Single Coil", icon: "pickup-single", detail: "Typical 6.4 kΩ" },
  { group: "Single Coil", icon: "pickup-single", detail: "Typical 7.4 kΩ" },
  { group: "Humbucker", icon: "pickup-humbucker", detail: "Typical 8.2 kΩ" },
];

function designDefs(side: "source" | "target", group: string) {
  return [
    { key: `${side}DesignType`, name: "Type", default: 0, min: 0, max: 2, unit: "enum", step: 1, group, advanced: true,
      labels: TYPES, labelInfo: TYPE_INFO, control: "picker" },
    { key: `${side}Resistance`, name: "DC Resistance", default: 6.5, min: 3, max: 20, unit: "kΩ", step: 0.05, group, advanced: true },
    { key: `${side}Gauge`, name: "Wire Gauge", default: 42, min: 42, max: 44, unit: "enum", step: 1, group, advanced: true,
      labels: ["42 AWG", "43 AWG", "44 AWG"] },
    { key: `${side}Magnet`, name: "Magnet", default: 2, min: 0, max: 3, unit: "enum", step: 1, group, advanced: true,
      labels: ["Alnico 2", "Alnico 3", "Alnico 5", "Ceramic"] },
    { key: `${side}Cover`, name: "Cover", default: 0, min: 0, max: 2, unit: "enum", step: 1, group, advanced: true,
      labels: ["None", "Nickel Silver", "Brass"] },
    { key: `${side}Wiring`, name: "Coil Wiring", default: 0, min: 0, max: 2, unit: "enum", step: 1, group, advanced: true,
      labels: ["Series", "Parallel", "Single Coil"] },
    { key: `${side}CapacitanceTrim`, name: "Capacitance Trim", default: 0, min: -50, max: 50, unit: "%", step: 1, group, advanced: true },
  ];
}

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
    ...designDefs("source", "Your Pickup Design"),
    ...designDefs("target", "Sound Like Design"),
  ],
});

function node(params: Record<string, number> = { source: 0, target: 2 }): GraphNode {
  return { id: "pm1", type: EffectGuids.kPickupMatch, displayName: "Pickup Match", category: "eq", bypassed: false,
    params, config: {} } as GraphNode;
}

function show(n: GraphNode): void {
  showNodeParamsPanel(n, { id: "user-test", name: "Test", graph: { nodes: [n], edges: [] } } as Preset);
}

const block = (side: string) => document.querySelector<HTMLElement>(`.pickup-design[data-side="${side}"]`);

/** Answers every curve request still out, oldest first, until none is left: requests are
 * coalesced, so one an earlier test left unanswered holds the newest back. */
function answerResponses(summary: Record<string, number>): void {
  const answered = new Set<string>();
  for (;;) {
    const request = allSent.find((m) => m.type === "getEffectResponse" && m.requestId && !answered.has(m.requestId));
    if (!request?.requestId) return;
    answered.add(request.requestId);
    applyEffectResponse({ type: "effectResponse", requestId: request.requestId, supported: true,
      frequencies: [20, 20000], magnitudesDb: [0, 0], summary });
  }
}

describe("pickup design blocks", () => {
  beforeEach(() => {
    picker.closeEnumPicker();
    sent = [];
  });

  it("keeps the design parameters out of the generic controls", () => {
    show(node());
    for (const key of PICKUP_DESIGN_PARAM_KEYS) {
      const controls = Array.from(document.querySelectorAll(`[data-param-key="${key}"]`));
      expect(controls.length, key).toBeGreaterThan(0);
      expect(controls.every((el) => el.closest(".pickup-design")), key).toBe(true);
    }
    // And out of the Advanced tab, which then has nothing left to show.
    expect(document.querySelector(".node-param-tab[data-tab='advanced']")).toBeNull();
  });

  it("hides both blocks until a side is Custom Design", () => {
    show(node());
    expect(block("source")?.hidden).toBe(true);
    expect(block("target")?.hidden).toBe(true);
    show(node({ source: 0, target: 1 }));
    expect(block("source")?.hidden).toBe(true);
    expect(block("target")?.hidden).toBe(false);
  });

  it("shows a side's block as soon as it is set to Custom Design in the picker", () => {
    const n = node();
    show(n);
    document.querySelector<HTMLButtonElement>(".enum-picker-trigger[data-param-key='source']")?.click();
    document.querySelector<HTMLButtonElement>(".enum-picker-group-tile[data-group='']")?.click();
    Array.from(document.querySelectorAll<HTMLButtonElement>(".enum-picker-option"))
      .find((el) => el.textContent?.includes("Custom Design"))?.click();
    expect(n.params.source).toBe(1);
    expect(block("source")?.hidden).toBe(false);
    expect(block("target")?.hidden).toBe(true);
  });

  it("draws the design type as a picker with pictures", () => {
    show(node({ source: 1, target: 2 }));
    const trigger = block("source")?.querySelector<HTMLButtonElement>(".enum-picker-trigger[data-param-key='sourceDesignType']");
    expect(trigger?.querySelector(".enum-picker-trigger-label")?.textContent).toBe("Strat-Style Single Coil");
    trigger?.click();
    expect(document.querySelectorAll(".enum-picker-option")).toHaveLength(2);
    expect(document.querySelector(".enum-picker-option-detail")?.textContent).toContain("6.4");
  });

  it("shows the figures the engine sends with the curve", () => {
    show(node({ source: 1, target: 2 }));
    answerResponses({
      sourceWireLengthM: 1191.4, sourceInductanceH: 2.655, sourceResistanceKohm: 6.5, sourceCapacitancePf: 118,
      sourcePeakHz: 3399, sourceQ: 1.42, sourceLevelDb: 0.4, targetPeakHz: 2343, targetQ: 0.97, targetLevelDb: 9.7,
    });
    const text = block("source")?.querySelector(".pickup-design-summary")?.textContent ?? "";
    expect(text).toContain("2.66 H");
    expect(text).toContain("118 pF");
    expect(text).toContain("peak 3.40 kHz");
    expect(text).toContain("+0.4 dB");
  });
});

describe("describeDesign", () => {
  it("says nothing without the full set of figures", () => {
    expect(describeDesign({ sourcePeakHz: 3000 }, "source")).toBe("");
  });
});
