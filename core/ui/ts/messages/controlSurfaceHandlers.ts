/**
 * Automation slots, the MIDI log, and MIDI learn.
 */

import { applyAutomationState, handleMidiLearnCapture, handleMidiLogEntry } from "../automationPanel.js";
import type { AutomationRegistryEntry, AutomationSlot } from "../types.js";
import type { IncomingPayload } from "./types.js";

export function onAutomation(payload: IncomingPayload): void {
  const autoPayload = payload as {
    slots?: AutomationSlot[];
    registry?: AutomationRegistryEntry[];
    maxCustomSlots?: number;
  };
  applyAutomationState({
    slots: autoPayload.slots ?? [],
    registry: autoPayload.registry ?? [],
    maxCustomSlots: autoPayload.maxCustomSlots ?? 16,
  });
}

export function onMidiLog(payload: IncomingPayload): void {
  const logPayload = payload as { midiType?: string; channel?: number; data1?: number; data2?: number };
  handleMidiLogEntry({
    type: logPayload.midiType ?? "Unknown",
    channel: logPayload.channel ?? 0,
    data1: logPayload.data1 ?? 0,
    data2: logPayload.data2 ?? 0,
  });
}

export function onMidiLearnCapture(payload: IncomingPayload): void {
  const capturePayload = payload as { slotId?: string };
  if (typeof capturePayload.slotId === "string") {
    handleMidiLearnCapture(capturePayload.slotId);
  }
}
