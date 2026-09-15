import { afterEach, describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import {
  buildMidiLearnMenuItems,
  countCustomSlots,
  describeMidiMap,
  findSlotForAddress,
  formatMidiChannel,
  nextCustomSlotId,
  resolveMidiLearnTarget,
} from "../ts/midiMapping.js";
import type { MidiLearnTargetContext } from "../ts/midiMapping.js";
import type { EffectTypeInfo } from "../ts/presetV2.js";
import type { AutomationRegistryEntry, AutomationSlot, GraphNode } from "../ts/types.js";

const AMP_TYPE = "0f3c3c1e-amp";

const REGISTRY_LABELS: Record<string, string> = {
  "global.inputTrim": "Input Trim",
  "global.outputTrim": "Output Trim",
  "setlist.preset1": "Setlist Preset 1",
  "setlist.preset8": "Setlist Preset 8",
  "setlist.bankUp": "Bank Up",
  "setlist.bankDown": "Bank Down",
  "setlist.bankSelect": "Select Bank",
};

const registry: AutomationRegistryEntry[] = Object.entries(REGISTRY_LABELS).map(([address, label]) => ({
  address,
  label,
  unit: "",
  min: 0,
  max: 1,
  isStepped: false,
  isTrigger: false,
}));

const effects = new Map<string, EffectTypeInfo>([
  [AMP_TYPE, {
    type: AMP_TYPE,
    displayName: "Amp",
    category: "amp",
    requiresResource: false,
    parameters: [
      { key: "gain", name: "Gain", default: 5, min: 0, max: 10, unit: "amount" },
      { key: "bright", name: "Bright", default: 0, min: 0, max: 1, unit: "toggle" },
    ],
  } as EffectTypeInfo],
  [EffectGuids.kPluginHost, {
    type: EffectGuids.kPluginHost,
    displayName: "Plugin",
    category: "utility",
    requiresResource: false,
    parameters: [{ key: "mix", name: "Mix", default: 1, min: 0, max: 1, unit: "amount" }],
  } as EffectTypeInfo],
]);

const aliases = new Map([["amp_legacy", AMP_TYPE]]);

const nodes = new Map<string, GraphNode>([
  ["amp1", { id: "amp1", type: "amp_legacy", params: {} } as GraphNode],
  ["plugin1", { id: "plugin1", type: EffectGuids.kPluginHost, params: {} } as GraphNode],
]);

const context: MidiLearnTargetContext = {
  registry,
  findNode: (nodeId) => nodes.get(nodeId),
  getEffectInfo: (type) => effects.get(aliases.get(type) ?? type),
};

function resolveAt(selector: string) {
  return resolveMidiLearnTarget(document.querySelector(selector), context);
}

function slot(slotId: string, address: string, isDefault: boolean): AutomationSlot {
  return { slotId, label: slotId, address, isDefault, value: 0 };
}

afterEach(() => {
  document.body.innerHTML = "";
});

describe("resolveMidiLearnTarget", () => {
  it("maps the input and output knobs to the global trims, from the knob or its readout", () => {
    document.body.innerHTML = `
      <div class="knob-control"><div class="knob" data-param="input"><div class="knob-indicator" id="in-indicator"></div></div><span class="knob-value" id="in-value">+0.0 dB</span></div>
      <div class="knob-control"><div class="knob" data-param="output" id="out-knob"></div></div>
      <div class="knob-control"><div class="knob" data-param="transpose" id="transpose-knob"></div></div>
    `;
    expect(resolveAt("#in-indicator")).toEqual({ address: "global.inputTrim", label: "Input Trim" });
    expect(resolveAt("#in-value")?.address).toBe("global.inputTrim");
    expect(resolveAt("#out-knob")?.address).toBe("global.outputTrim");
    // Transpose is not an automation address.
    expect(resolveAt("#transpose-knob")).toBeNull();
  });

  it("maps an effect parameter to its canonical type address, from the control, its label or a toggle's slider", () => {
    document.body.innerHTML = `
      <div class="node-param-group">
        <span class="node-param-label" id="gain-label">Gain</span>
        <div class="knob node-param-knob" id="gain-knob" data-node-id="amp1" data-param-key="gain"></div>
      </div>
      <div class="node-param-group">
        <label class="toggle-switch">
          <input class="node-param-toggle" type="checkbox" data-node-id="amp1" data-param-key="bright">
          <span class="toggle-slider" id="bright-slider"></span>
        </label>
      </div>
    `;
    expect(resolveAt("#gain-knob")).toEqual({ address: `node.${AMP_TYPE}.gain`, label: "Amp: Gain" });
    expect(resolveAt("#gain-label")?.address).toBe(`node.${AMP_TYPE}.gain`);
    expect(resolveAt("#bright-slider")).toEqual({ address: `node.${AMP_TYPE}.bright`, label: "Amp: Bright" });
  });

  it("does not guess which control a shared group's label belongs to", () => {
    document.body.innerHTML = `
      <div class="node-param-group">
        <span id="shared-label">Channel</span>
        <div class="knob node-param-knob" id="first" data-node-id="amp1" data-param-key="gain"></div>
        <div class="knob node-param-knob" data-node-id="amp1" data-param-key="bright"></div>
      </div>
    `;
    expect(resolveAt("#shared-label")).toBeNull();
    expect(resolveAt("#first")?.address).toBe(`node.${AMP_TYPE}.gain`);
  });

  it("offers nothing a node address cannot drive", () => {
    document.body.innerHTML = `
      <div class="knob node-param-knob" id="missing-node" data-node-id="gone" data-param-key="gain"></div>
      <div class="knob node-param-knob" id="undeclared" data-node-id="amp1" data-param-key="presence"></div>
      <div class="knob node-param-knob" id="plugin" data-node-id="plugin1" data-param-key="mix"></div>
      <div class="node-param-group">
        <div class="knob node-param-knob" data-node-id="amp1" data-param-key="gain"></div>
        <span class="node-param-value"><input type="number" class="knob-inline-editor" id="inline-editor"></span>
      </div>
    `;
    expect(resolveAt("#missing-node")).toBeNull();
    expect(resolveAt("#undeclared")).toBeNull();
    expect(resolveAt("#plugin")).toBeNull();
    expect(resolveAt("#inline-editor")).toBeNull();
  });

  it("maps setlist pads, setlist slots and the bank controls", () => {
    document.body.innerHTML = `
      <button class="performance-setlist-pad" data-performance-action="setlist-slot" data-slot-index="0"><span id="pad-name">Clean</span></button>
      <button class="performance-setlist-pad" data-performance-action="setlist-slot" data-slot-index="3" id="unregistered-pad"></button>
      <div class="setlist-slot" data-slot-index="7" id="library-slot"></div>
      <button data-performance-action="bank-up" id="bank-up"><svg></svg></button>
      <button data-performance-action="bank-down" id="bank-down"></button>
      <div class="performance-bank-current"><strong id="bank-name">Bank 1</strong></div>
      <div class="performance-bank-current performance-bank-editor"><span id="bank-editor">Name</span></div>
    `;
    expect(resolveAt("#pad-name")).toEqual({ address: "setlist.preset1", label: "Setlist Preset 1" });
    expect(resolveAt("#library-slot")?.address).toBe("setlist.preset8");
    // Only addresses the engine registers are offered.
    expect(resolveAt("#unregistered-pad")).toBeNull();
    expect(resolveAt("#bank-up")?.address).toBe("setlist.bankUp");
    expect(resolveAt("#bank-down")?.address).toBe("setlist.bankDown");
    expect(resolveAt("#bank-name")?.address).toBe("setlist.bankSelect");
    expect(resolveAt("#bank-editor")).toBeNull();
  });

  it("offers nothing for an element that drives nothing", () => {
    document.body.innerHTML = `<div id="plain">Hello</div>`;
    expect(resolveAt("#plain")).toBeNull();
    expect(resolveMidiLearnTarget(null, context)).toBeNull();
  });
});

describe("slot lookup", () => {
  it("prefers the default slot for an address over a custom slot pointed at it", () => {
    const slots = [
      slot("custom.1", "global.inputTrim", false),
      slot("default.inputLevel", "global.inputTrim", true),
      slot("custom.2", "node.amp.gain", false),
    ];
    expect(findSlotForAddress(slots, "global.inputTrim")?.slotId).toBe("default.inputLevel");
    expect(findSlotForAddress(slots, "node.amp.gain")?.slotId).toBe("custom.2");
    expect(findSlotForAddress(slots, "node.amp.bright")).toBeUndefined();
    expect(countCustomSlots(slots)).toBe(2);
  });

  it("picks a custom id past every numbered one in use", () => {
    expect(nextCustomSlotId([])).toBe("custom.1");
    expect(nextCustomSlotId([
      slot("default.bankUp", "setlist.bankUp", true),
      slot("custom.4", "", false),
      slot("custom.54321", "", false),
      slot("custom.named", "", false),
    ])).toBe("custom.54322");
  });
});

describe("buildMidiLearnMenuItems", () => {
  const mapped: AutomationSlot = {
    ...slot("custom.3", "node.amp.gain", false),
    midiMap: { eventType: 0, channel: 0, controller: 7, mode: 0, sensitivity: 0.1, pickupRange: 0.1 },
  };
  const menu = (state: Parameters<typeof buildMidiLearnMenuItems>[0]) => buildMidiLearnMenuItems(state)
    .map((item) => `${item.action}${item.disabled ? " (disabled)" : ""}${item.hint ? `: ${item.hint}` : ""}`);

  it("offers only learn when the control has no MIDI mapping", () => {
    expect(menu({ slot: undefined, armedSlotId: null, customSlotCount: 2, maxCustomSlots: 16 })).toEqual(["learn"]);
    // A slot that already drives the control needs no free custom slot.
    expect(menu({ slot: slot("default.bankUp", "setlist.bankUp", true), armedSlotId: null, customSlotCount: 16, maxCustomSlots: 16 })).toEqual(["learn"]);
  });

  it("offers Clear Mapping, naming the mapping, when the slot has one", () => {
    expect(menu({ slot: mapped, armedSlotId: null, customSlotCount: 16, maxCustomSlots: 16 })).toEqual(["learn", "clear: CC 7 ch1 Abs"]);
  });

  it("swaps learn for cancel while that slot listens, and keeps Clear Mapping", () => {
    expect(menu({ slot: mapped, armedSlotId: "custom.3", customSlotCount: 3, maxCustomSlots: 16 })).toEqual(["cancel: Listening…", "clear: CC 7 ch1 Abs"]);
    // Another slot listening does not change this control's menu.
    expect(menu({ slot: mapped, armedSlotId: "default.bankUp", customSlotCount: 3, maxCustomSlots: 16 })).toEqual(["learn", "clear: CC 7 ch1 Abs"]);
  });

  it("disables learn on an undriven control when every custom slot is taken", () => {
    expect(menu({ slot: undefined, armedSlotId: null, customSlotCount: 16, maxCustomSlots: 16 })).toEqual(["learn (disabled): All 16 custom slots in use"]);
  });
});

describe("describeMidiMap", () => {
  it("shows 1-based channels, and the mode only when there is one", () => {
    expect(describeMidiMap({ eventType: 0, channel: 0, controller: 7, mode: 0 })).toBe("CC 7 ch1 Abs");
    expect(describeMidiMap({ eventType: 2, channel: 9, controller: 60 })).toBe("NoteOn 60 ch10");
    expect(formatMidiChannel(-1)).toBe("any");
  });
});
