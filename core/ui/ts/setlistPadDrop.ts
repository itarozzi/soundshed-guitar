/**
 * Setlist pads as somewhere to drop a preset.
 *
 * A preset dragged from the library popover or the toolbar's preset name
 * (presets/drag.ts) and dropped on a pad is assigned to that pad's slot — the
 * same as holding the pad's save button with that preset loaded, including the
 * confirmation before it replaces a different preset.
 */

import { assignSetlistSlot } from "./performancePads.js";
import { registerPresetDropZone } from "./presets.js";

export function initializeSetlistPadPresetDrop(): void {
  // Only the setlist view renders these wraps, so effect pads never take a preset.
  // The wrap rather than the pad itself, so a drop on the pad's save button counts.
  registerPresetDropZone(
    document.getElementById("panel-performance"),
    ".performance-setlist-pad-wrap[data-slot-index]",
    (presetId, pad) => void assignSetlistSlot(Number(pad.slotIndex), presetId),
  );
}
