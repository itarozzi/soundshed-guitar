/**
 * MIDI mapping helpers shared by the MIDI & Automation panel and the right-click
 * "MIDI Learn…" menu: which automation address a control drives, which slot
 * already drives it, and how a mapping reads on screen.
 *
 * Nothing here reaches for app state or builds DOM — the caller hands in the
 * element and the lookups it needs — so it stays a leaf module and tests without
 * the app shell.
 */

import { EffectGuids } from "./effectGuids.js";
import type { EffectTypeInfo } from "./presetV2.js";
import type { AutomationMidiMap, AutomationRegistryEntry, AutomationSlot, GraphNode } from "./types.js";

/** An automation address a control drives, and a label for it. */
export interface MidiLearnTarget {
  address: string;
  label: string;
}

export interface MidiLearnTargetContext {
  /** The engine's `global.*` and `setlist.*` entries. An address missing from it is not offered. */
  registry: readonly AutomationRegistryEntry[];
  /** A node in the graph the engine is playing; undefined for one a `node.*` address cannot reach. */
  findNode: (nodeId: string) => GraphNode | undefined;
  getEffectInfo: (type: string) => EffectTypeInfo | undefined;
}

/** The control-bar knobs, by `data-param`, and the global addresses they set. */
const GLOBAL_KNOB_ADDRESSES = new Map<string, string>([
  ["input", "global.inputTrim"],
  ["output", "global.outputTrim"],
]);

const NODE_PARAM_CONTROL_SELECTOR = [".node-param-knob", ".node-param-toggle", ".node-param-slider", ".node-param-blend-slider"]
  .map((selector) => `${selector}[data-node-id][data-param-key]`)
  .join(", ");

/** Wrappers holding a parameter control together with its label and value readout. */
const NODE_PARAM_GROUP_SELECTOR = ".node-param-group, .node-param-blend-group, .custom-layout-control";

/**
 * Effect types whose parameters belong to what a node has loaded — a plugin, a WASM
 * module — rather than to the type. A `node.*` address names only the type, so it
 * cannot say which of those parameters it means.
 */
const PER_NODE_PARAMETER_TYPES = new Set<string>([EffectGuids.kPluginHost, EffectGuids.kWasmHost]);

const TEXT_ENTRY_SELECTOR = "input:not([type='checkbox']):not([type='range']), textarea, select, [contenteditable=''], [contenteditable='true']";

/**
 * The automation address a right-clicked element drives, or null when it drives
 * nothing a MIDI mapping can reach. A text field is always null: cut, copy and
 * paste are the menu that belongs there.
 */
export function resolveMidiLearnTarget(element: Element | null, context: MidiLearnTargetContext): MidiLearnTarget | null {
  if (!element || element.closest(TEXT_ENTRY_SELECTOR)) {
    return null;
  }
  return resolveGlobalKnob(element, context)
    ?? resolveNodeParam(element, context)
    ?? resolveSetlistControl(element, context);
}

function registryTarget(address: string, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const entry = context.registry.find((candidate) => candidate.address === address);
  return entry ? { address, label: entry.label } : null;
}

function resolveGlobalKnob(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const knob = element.closest<HTMLElement>(".knob[data-param]")
    ?? element.closest(".knob-control")?.querySelector<HTMLElement>(".knob[data-param]");
  const address = GLOBAL_KNOB_ADDRESSES.get(knob?.dataset.param ?? "");
  return address ? registryTarget(address, context) : null;
}

function resolveNodeParam(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const control = findNodeParamControl(element);
  const nodeId = control?.dataset.nodeId;
  const paramKey = control?.dataset.paramKey;
  if (!nodeId || !paramKey) {
    return null;
  }

  const node = context.findNode(nodeId);
  const info = node ? context.getEffectInfo(node.type) : undefined;
  if (!info || PER_NODE_PARAMETER_TYPES.has(info.type)) {
    return null;
  }

  // Only a parameter the type declares has a range the engine can map a MIDI value onto.
  const param = info.parameters.find((candidate) => candidate.key === paramKey);
  if (!param) {
    return null;
  }

  return {
    address: `node.${info.type}.${paramKey}`,
    label: `${info.displayName || info.type}: ${param.name || paramKey}`,
  };
}

function findNodeParamControl(element: Element): HTMLElement | null {
  const direct = element.closest<HTMLElement>(NODE_PARAM_CONTROL_SELECTOR);
  if (direct) {
    return direct;
  }
  // A label, a value readout or a toggle's slider sits beside the control, not inside
  // it. The surrounding group names the control only when it holds just the one.
  const controls = element.closest(NODE_PARAM_GROUP_SELECTOR)?.querySelectorAll<HTMLElement>(NODE_PARAM_CONTROL_SELECTOR);
  return controls?.length === 1 ? controls[0] : null;
}

function resolveSetlistControl(element: Element, context: MidiLearnTargetContext): MidiLearnTarget | null {
  const slot = element.closest<HTMLElement>(".performance-setlist-pad[data-slot-index], .setlist-slot[data-slot-index]");
  if (slot) {
    // setlist.presetN is the Nth slot of whichever setlist is active — the same slot the pad shows.
    const index = Number.parseInt(slot.dataset.slotIndex ?? "", 10);
    return index >= 0 ? registryTarget(`setlist.preset${index + 1}`, context) : null;
  }

  const action = element.closest<HTMLElement>("[data-performance-action]")?.dataset.performanceAction;
  if (action === "bank-up") {
    return registryTarget("setlist.bankUp", context);
  }
  if (action === "bank-down") {
    return registryTarget("setlist.bankDown", context);
  }
  if (element.closest(".performance-bank-current:not(.performance-bank-editor)")) {
    return registryTarget("setlist.bankSelect", context);
  }
  return null;
}

/**
 * The slot that already drives `address`. A default slot wins over a custom slot
 * pointed at the same address: it is the one the DAW exposes under a stable name.
 */
export function findSlotForAddress(slots: readonly AutomationSlot[], address: string): AutomationSlot | undefined {
  return slots.find((slot) => slot.isDefault && slot.address === address)
    ?? slots.find((slot) => slot.address === address);
}

export function countCustomSlots(slots: readonly AutomationSlot[]): number {
  return slots.filter((slot) => !slot.isDefault).length;
}

/** A `custom.N` id no slot uses yet. */
export function nextCustomSlotId(slots: readonly AutomationSlot[]): string {
  let highest = 0;
  for (const slot of slots) {
    const match = /^custom\.(\d+)$/.exec(slot.slotId);
    if (match) {
      highest = Math.max(highest, Number(match[1]));
    }
  }
  return `custom.${highest + 1}`;
}

/**
 * MIDI channels are stored 0-15 on the wire (and -1 for "any"), but musicians
 * and hardware label them 1-16 — always display the 1-based number.
 */
export function formatMidiChannel(channel: number): string {
  return channel < 0 ? "any" : String(channel + 1);
}

const MIDI_EVENT_NAMES = ["CC", "PC", "NoteOn", "NoteOff", "PBend"];
const MIDI_MODE_NAMES = ["Abs", "Rel", "Toggle", "Pickup"];

/** "CC 7 ch1 Abs". The mode is left off when there is none, as in a learn capture. */
export function describeMidiMap(map: Pick<AutomationMidiMap, "eventType" | "channel" | "controller"> & { mode?: number }): string {
  const text = `${MIDI_EVENT_NAMES[map.eventType] || "CC"} ${map.controller} ch${formatMidiChannel(map.channel)}`;
  return map.mode === undefined ? text : `${text} ${MIDI_MODE_NAMES[map.mode] || "Abs"}`;
}
