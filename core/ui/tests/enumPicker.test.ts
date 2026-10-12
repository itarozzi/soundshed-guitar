/**
 * The enum picker: an enum the engine marks `control: "picker"` is drawn as a dropdown trigger
 * instead of a knob, and its flyout filters by group tile and search, sends the chosen label's
 * value, closes before it applies the choice, and escapes everything it shows.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; nodeId?: string; paramKey?: string; value?: number };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry, parseParamLabelInfo } = await import("../ts/presetV2.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const picker = await import("../ts/signalPath/paramsPanel/enumPicker.js");

const LABELS = ["Single Coil (Neck)", "Humbucker (Bridge)", "Acme <b>Hot</b> (bridge)", "Acme Vintage (neck)", "Other One"];
const LABEL_INFO = [
  { group: "Single Coil", subgroup: "Generic", icon: "pickup-single", detail: "Neck · peak 3.6 kHz" },
  { group: "Humbucker", subgroup: "Generic", icon: "pickup-humbucker", detail: "Bridge · peak 2.3 kHz" },
  { group: "Humbucker", subgroup: "Acme", icon: "pickup-humbucker", detail: "Bridge · <i>hot</i>" },
  { group: "Single Coil", subgroup: "Acme", icon: "pickup-single", detail: "Neck" },
  { group: "Other", subgroup: "Misc", icon: "pickup-other" },
];

EffectTypeRegistry.register(EffectGuids.kPickupMatch, {
  type: EffectGuids.kPickupMatch,
  displayName: "Pickup Match",
  category: "eq",
  requiresResource: false,
  parameters: [
    { key: "source", name: "Your Pickup", default: 0, min: 0, max: 4, unit: "enum", step: 1, group: "Pickups",
      labels: LABELS, labelInfo: LABEL_INFO, control: "picker" },
    { key: "amount", name: "Amount", default: 1, min: 0, max: 1, unit: "amount", group: "Pickups" },
  ],
});

function node(): GraphNode {
  return { id: "pm1", type: EffectGuids.kPickupMatch, displayName: "Pickup Match", category: "eq", bypassed: false,
    params: { source: 0, amount: 1 }, config: {} } as GraphNode;
}

function presetWith(n: GraphNode): Preset {
  return { id: "user-test", name: "Test", graph: { nodes: [n], edges: [] } } as Preset;
}

const popover = (): HTMLElement | null => document.querySelector<HTMLElement>(".enum-picker-popover");
const optionLabels = (): string[] =>
  Array.from(document.querySelectorAll(".enum-picker-option-label")).map((el) => el.textContent ?? "");

describe("enum picker", () => {
  let current: GraphNode;

  beforeEach(() => {
    picker.closeEnumPicker();
    sent = [];
    current = node();
    showNodeParamsPanel(current, presetWith(current));
  });

  it("draws a picker trigger, not a knob, for a picker parameter", () => {
    const trigger = document.querySelector<HTMLButtonElement>(".enum-picker-trigger[data-param-key='source']");
    expect(trigger).not.toBeNull();
    expect(document.querySelector(".node-param-knob[data-param-key='source']")).toBeNull();
    expect(document.querySelector(".node-param-knob[data-param-key='amount']")).not.toBeNull();
    expect(trigger?.querySelector(".enum-picker-trigger-label")?.textContent).toBe("Single Coil (Neck)");
    expect(trigger?.querySelector("svg")).not.toBeNull();
  });

  it("draws its own conversion curve, not the parametric EQ's band editor", () => {
    expect(document.querySelector(".pickup-response-canvas")).not.toBeNull();
    expect(document.querySelector(".eq-visualizer[data-node-id]")).toBeNull();
  });

  it("opens on the current choice's group, as a body-level popover the Controls sheet ignores", () => {
    document.querySelector<HTMLButtonElement>(".enum-picker-trigger")?.click();
    const el = popover();
    expect(el?.parentElement).toBe(document.body);
    expect(el?.hasAttribute("data-control-bar-popover")).toBe(true);
    expect(el?.querySelector(".enum-picker-group-tile[aria-checked='true']")?.getAttribute("data-group")).toBe("Single Coil");
    expect(optionLabels()).toEqual(["Single Coil (Neck)", "Acme Vintage (neck)"]);
    expect(el?.querySelector(".enum-picker-option.is-selected .enum-picker-option-label")?.textContent).toBe("Single Coil (Neck)");
  });

  it("filters by tile and by search across groups", () => {
    document.querySelector<HTMLButtonElement>(".enum-picker-trigger")?.click();
    document.querySelector<HTMLButtonElement>(".enum-picker-group-tile[data-group='Humbucker']")?.click();
    expect(optionLabels()).toEqual(["Humbucker (Bridge)", "Acme <b>Hot</b> (bridge)"]);
    expect(Array.from(document.querySelectorAll(".enum-picker-subgroup")).map((el) => el.textContent)).toEqual(["Generic", "Acme"]);

    document.querySelector<HTMLButtonElement>(".enum-picker-group-tile[data-group='']")?.click();
    const search = document.querySelector<HTMLInputElement>(".enum-picker-search");
    if (!search) throw new Error("no search box");
    search.value = "acme neck";
    search.dispatchEvent(new Event("input"));
    expect(optionLabels()).toEqual(["Acme Vintage (neck)"]);
  });

  it("sends the chosen label's value, closes first, and updates the trigger in place", () => {
    document.querySelector<HTMLButtonElement>(".enum-picker-trigger")?.click();
    document.querySelector<HTMLButtonElement>(".enum-picker-group-tile[data-group='Humbucker']")?.click();
    const option = Array.from(document.querySelectorAll<HTMLButtonElement>(".enum-picker-option"))
      .find((el) => el.querySelector(".enum-picker-option-label")?.textContent === "Acme <b>Hot</b> (bridge)");
    option?.click();

    expect(popover()).toBeNull();
    expect(picker.isEnumPickerOpen()).toBe(false);
    expect(sent.filter((m) => m.type === "updateSignalPathNodeParam")).toContainEqual(
      expect.objectContaining({ nodeId: "pm1", paramKey: "source", value: 2 }));
    expect(current.params.source).toBe(2);
    expect(document.querySelector(".enum-picker-trigger-label")?.textContent).toBe("Acme <b>Hot</b> (bridge)");
  });

  it("escapes labels and details rather than rendering them as markup", () => {
    document.querySelector<HTMLButtonElement>(".enum-picker-trigger")?.click();
    document.querySelector<HTMLButtonElement>(".enum-picker-group-tile[data-group='Humbucker']")?.click();
    expect(popover()?.querySelector("b, i")).toBeNull();
  });

  it("closes on Escape and on a click outside, and re-clicking the trigger toggles it", () => {
    const trigger = () => document.querySelector<HTMLButtonElement>(".enum-picker-trigger");
    trigger()?.click();
    document.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
    expect(popover()).toBeNull();

    trigger()?.click();
    document.body.dispatchEvent(new PointerEvent("pointerdown", { bubbles: true }));
    expect(popover()).toBeNull();

    trigger()?.click();
    trigger()?.click();
    expect(popover()).toBeNull();
  });

  it("follows the panel when it redraws under an open flyout", () => {
    document.querySelector<HTMLButtonElement>(".enum-picker-trigger")?.click();
    showNodeParamsPanel(current, presetWith(current));
    expect(popover()).not.toBeNull();
    expect(document.querySelector(".enum-picker-trigger")?.getAttribute("aria-expanded")).toBe("true");
  });
});

describe("picker helpers", () => {
  it("parses label info only when it lines up with the labels", () => {
    expect(parseParamLabelInfo([{ group: "A" }, { detail: 3 }], 2)).toEqual([{ group: "A" }, {}]);
    expect(parseParamLabelInfo([{ group: "A" }], 2)).toBeUndefined();
    expect(parseParamLabelInfo("nope", 0)).toBeUndefined();
  });

  it("maps values to choices the way an enum knob does", () => {
    const def = { key: "k", name: "K", default: 0, min: 0, max: 4, unit: "enum", step: 1, labels: LABELS };
    expect(picker.pickerChoiceIndex(def, 2.4)).toBe(2);
    expect(picker.pickerChoiceIndex(def, 99)).toBe(4);
    expect(picker.pickerChoiceIndex(def, -3)).toBe(0);
    expect(picker.isPickerParam(def)).toBe(false);
    expect(picker.isPickerParam({ ...def, control: "picker" })).toBe(true);
  });
});
