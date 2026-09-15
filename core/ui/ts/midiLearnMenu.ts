/**
 * Right-click → "MIDI Learn…" on the controls a MIDI mapping can drive: the input
 * and output level knobs, effect parameters, and the setlist pads and bank controls.
 *
 * It is the MIDI panel's Learn button, reached from the control. The learn is armed
 * on the slot that already drives the control's address — a default slot, or a
 * custom one — and an effect parameter nothing drives yet gets a custom slot made
 * for it first. Either way the captured mapping lands in the slot table the panel
 * edits. `midiMapping.ts` decides which address an element drives.
 */

import { armMidiLearn, cancelMidiLearn, getArmedMidiLearnSlotId, onMidiLearnChange } from "./automationPanel.js";
import type { MidiLearnCapture } from "./automationPanel.js";
import { postMessage } from "./bridge.js";
import { countCustomSlots, describeMidiMap, findSlotForAddress, nextCustomSlotId, resolveMidiLearnTarget } from "./midiMapping.js";
import type { MidiLearnTarget } from "./midiMapping.js";
import { showNotification } from "./notifications.js";
import { BUILTIN_EFFECTS, EffectTypeRegistry } from "./presetV2.js";
import type { EffectTypeInfo } from "./presetV2.js";
import { getActivePresetForRender, uiState } from "./state.js";
import type { GraphNode } from "./types.js";
import { escapeHtml } from "./utils.js";

/** How far inside the window the menu's edges are kept. */
const VIEWPORT_MARGIN_PX = 4;

let initialized = false;
let menuElement: HTMLElement | null = null;
let statusElement: HTMLElement | null = null;
/** The learn this menu armed, while it waits for a MIDI event. */
let listening: { slotId: string; label: string } | null = null;

export function initializeMidiLearnMenu(): void {
  if (initialized) return;
  initialized = true;

  document.addEventListener("contextmenu", handleContextMenu);
  // Capture phase, so a control that stops the event still closes the menu.
  document.addEventListener("pointerdown", (event) => {
    if (menuElement && !menuElement.contains(event.target as Node | null)) {
      closeMenu();
    }
  }, true);
  document.addEventListener("keydown", handleKeydown);
  document.addEventListener("scroll", closeMenu, true);
  window.addEventListener("resize", closeMenu);
  window.addEventListener("blur", closeMenu);
  onMidiLearnChange(handleMidiLearnChange);
}

function handleContextMenu(event: MouseEvent): void {
  // A right-click on the menu itself keeps it, rather than swapping in the browser's menu.
  if (menuElement && event.target instanceof Node && menuElement.contains(event.target)) {
    event.preventDefault();
    return;
  }
  closeMenu();
  const automation = uiState.automation;
  // Until the engine has sent its slots there is nothing to arm, and no way to tell
  // which slot already drives a control.
  if (!automation?.registry.length) {
    return;
  }

  const target = resolveMidiLearnTarget(event.target instanceof Element ? event.target : null, {
    registry: automation.registry,
    findNode: findPlayingNode,
    getEffectInfo: findEngineEffectInfo,
  });
  if (!target) {
    return;
  }

  event.preventDefault();
  openMenu(target, event.clientX, event.clientY);
}

/**
 * An effect type the engine registers. The chain's Input and Output routing nodes
 * (BUILTIN_EFFECTS) are defined only in the UI: the engine has no range for their
 * gain, so a learned MIDI value would reach it as a raw 0..1.
 */
function findEngineEffectInfo(type: string): EffectTypeInfo | undefined {
  const info = EffectTypeRegistry.get(type);
  return info && !BUILTIN_EFFECTS.some((routing) => routing.type === info.type) ? info : undefined;
}

/**
 * The node with this id in the preset the engine is playing. A composite being edited
 * shows its inner graph instead, and a `node.*` address cannot reach those nodes.
 */
function findPlayingNode(nodeId: string): GraphNode | undefined {
  if (uiState.compositeEditMode) {
    return undefined;
  }
  const preset = getActivePresetForRender();
  const graphs = [preset?.graph, ...(preset?.scenes ?? []).map((scene) => scene.graph)];
  for (const graph of graphs) {
    const node = graph?.nodes.find((candidate) => candidate.id === nodeId);
    if (node) {
      return node;
    }
  }
  return undefined;
}

function openMenu(target: MidiLearnTarget, x: number, y: number): void {
  const slots = uiState.automation?.slots ?? [];
  const maxCustomSlots = uiState.automation?.maxCustomSlots ?? 16;
  const slot = findSlotForAddress(slots, target.address);
  const armed = slot !== undefined && slot.slotId === getArmedMidiLearnSlotId();
  const noFreeSlot = !slot && countCustomSlots(slots) >= maxCustomSlots;

  let hint = "";
  if (armed) {
    hint = "Listening…";
  } else if (noFreeSlot) {
    hint = `All ${maxCustomSlots} custom slots in use`;
  } else if (slot?.midiMap) {
    hint = describeMidiMap(slot.midiMap);
  }

  const menu = document.createElement("div");
  menu.className = "midi-learn-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", "MIDI Learn");
  menu.innerHTML = `
    <div class="midi-learn-menu-title">${escapeHtml(slot?.label || target.label)}</div>
    <button class="midi-learn-menu-item" type="button" role="menuitem"${noFreeSlot ? " disabled" : ""}>
      <span>${armed ? "Cancel MIDI Learn" : "MIDI Learn…"}</span>
      ${hint ? `<span class="midi-learn-menu-hint">${escapeHtml(hint)}</span>` : ""}
    </button>
  `;

  const item = menu.querySelector<HTMLButtonElement>(".midi-learn-menu-item");
  item?.addEventListener("click", () => {
    closeMenu();
    if (armed) {
      cancelMidiLearn();
    } else {
      startMidiLearn(target);
    }
  });

  // Measured in place, then pulled back inside the window if it would overhang.
  document.body.appendChild(menu);
  const { width, height } = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(x, window.innerWidth - width - VIEWPORT_MARGIN_PX))}px`;
  menu.style.top = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(y, window.innerHeight - height - VIEWPORT_MARGIN_PX))}px`;
  menuElement = menu;
  item?.focus();
}

function closeMenu(): void {
  menuElement?.remove();
  menuElement = null;
}

function startMidiLearn(target: MidiLearnTarget): void {
  const slots = uiState.automation?.slots ?? [];
  const existing = findSlotForAddress(slots, target.address);
  let slotId = existing?.slotId;

  if (!slotId) {
    const maxCustomSlots = uiState.automation?.maxCustomSlots ?? 16;
    if (countCustomSlots(slots) >= maxCustomSlots) {
      showNotification("MIDI Learn unavailable", `all ${maxCustomSlots} custom slots are in use`);
      return;
    }
    slotId = nextCustomSlotId(slots);
    // The engine handles messages in order, so the slot exists by the time learn is armed on it.
    postMessage({ type: "setAutomationSlot", slotId, label: target.label, address: target.address });
  }

  // Set before arming: arming notifies handleMidiLearnChange, which drops a learn it did not start.
  listening = { slotId, label: existing?.label || target.label };
  armMidiLearn(slotId);
  showListeningStatus(listening.label);
}

function handleMidiLearnChange(armedSlotId: string | null, capture: MidiLearnCapture | null): void {
  if (!listening) {
    return;
  }
  if (capture?.slotId === listening.slotId) {
    showNotification(`Mapped ${describeMidiMap(capture)} to ${listening.label}`);
  }
  if (armedSlotId !== listening.slotId) {
    // Captured, cancelled, or replaced by a learn armed from the MIDI panel.
    listening = null;
    hideListeningStatus();
  }
}

function handleKeydown(event: KeyboardEvent): void {
  if (event.key !== "Escape") {
    return;
  }
  if (menuElement) {
    closeMenu();
    event.preventDefault();
  } else if (listening) {
    cancelMidiLearn();
  }
}

function showListeningStatus(label: string): void {
  if (!statusElement) {
    statusElement = document.createElement("div");
    statusElement.className = "midi-learn-status";
    statusElement.setAttribute("role", "status");
    // One listener for the element's lifetime; its contents are replaced for each learn.
    statusElement.addEventListener("click", (event) => {
      if (event.target instanceof Element && event.target.closest(".midi-learn-status-cancel")) {
        cancelMidiLearn();
      }
    });
    document.body.appendChild(statusElement);
  }
  statusElement.innerHTML = `
    <span class="midi-learn-status-dot" aria-hidden="true"></span>
    <span class="midi-learn-status-text">Move a MIDI control to map <strong>${escapeHtml(label)}</strong></span>
    <button class="midi-learn-status-cancel" type="button">Cancel</button>
  `;
  statusElement.hidden = false;
}

function hideListeningStatus(): void {
  if (statusElement) {
    statusElement.hidden = true;
  }
}
