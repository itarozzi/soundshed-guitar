/**
 * Dragging a preset — a row of the library popover, or the loaded preset's name
 * in the toolbar — onto something that takes one: a folder, the setlist panel,
 * or a setlist pad (see setlistPadDrop.ts).
 *
 * Pointer-driven, for the reasons given in pointerDrag.ts. The rows used to be
 * HTML5 `draggable`, and the two cannot share an element: once a native drag
 * starts, the browser cancels the pointer stream a pointer drag runs on.
 *
 * Targets register as zones — a selector inside a root that outlives re-renders
 * — so a panel outside the preset library can take a preset without this module
 * knowing its markup.
 */

import { beginPointerDrag, type PointerDragTarget } from "../pointerDrag.js";
import { uiState } from "../state.js";
import { presetChooserLabel, presetFolderTreeElement, presetLibraryPopover, presetListElement, setlistSlotsElement } from "./dom.js";
import { movePresetToFolder } from "./folderControls.js";
import { addPresetToSetlist, setSetlistExpanded } from "./setlists.js";

/**
 * The `data-*` attributes of the element a preset was dropped on, copied when the
 * pointer reached it: a re-render during the drag can replace the element itself.
 */
export type PresetDropZoneData = Readonly<Record<string, string | undefined>>;

type PresetDropHandler = (presetId: string, zone: PresetDropZoneData) => void;

interface PresetDropZone {
  root: HTMLElement;
  selector: string;
  onDrop: PresetDropHandler;
}

interface PresetDropTarget extends PointerDragTarget {
  drop: (presetId: string) => void;
}

const zones: PresetDropZone[] = [];

/**
 * Let any element matching `selector` inside `root` take a dragged preset. `root`
 * has to persist; what is inside it can be re-rendered freely. Where zones
 * overlap, the first registered wins.
 */
export function registerPresetDropZone(root: HTMLElement | null, selector: string, onDrop: PresetDropHandler): void {
  if (root) {
    zones.push({ root, selector, onDrop });
  }
}

/**
 * The library popover covers much of the window, setlist pads included. Once a
 * drag has been outside it, it fades and lets the pointer through for the rest
 * of the drag, so whatever it was covering can take the drop. The class only has
 * an effect under `body.preset-dragging`, so nothing needs to clear it when the
 * drag ends; the next drag clears it on the way in.
 */
function releaseLibraryPopoverOnceLeft(hit: Element | null): void {
  if (presetLibraryPopover?.classList.contains("open") && !presetLibraryPopover.contains(hit)) {
    presetLibraryPopover.classList.add("drag-released");
  }
}

function resolvePresetDropTarget(event: PointerEvent): PresetDropTarget | null {
  const hit = document.elementFromPoint(event.clientX, event.clientY);
  releaseLibraryPopoverOnceLeft(hit);
  for (const zone of zones) {
    const element = hit?.closest<HTMLElement>(zone.selector);
    if (element && zone.root.contains(element)) {
      const data = { ...element.dataset };
      return { element, drop: (presetId) => zone.onDrop(presetId, data) };
    }
  }
  return null;
}

/** Track a press on `source` as a possible drag of `presetId`; the drag preview is cloned from `source`. */
export function beginPresetDrag(event: PointerEvent, source: HTMLElement, sourceSelector: string, presetId: string): void {
  presetLibraryPopover?.classList.remove("drag-released");
  beginPointerDrag<PresetDropTarget>(event, {
    source,
    sourceSelector,
    previewClass: "preset-drag-preview",
    bodyClass: "preset-dragging",
    resolveTarget: resolvePresetDropTarget,
    setTargetHighlight: (target, highlighted) => target.element.classList.toggle("drag-over", highlighted),
    // After the release rather than during it. A drop on a folder re-renders the list the row came
    // from, and Chromium dispatches no click for a press whose target was removed before the button
    // came up — which would leave pointerDrag.ts armed to swallow the next click, keyboard ones too.
    onDrop: (target) => {
      if (target) {
        const { drop } = target;
        window.setTimeout(() => drop(presetId), 0);
      }
    },
  });
}

export function initializePresetDrag(): void {
  // Bound to locals so the null checks narrow inside the listeners below; a
  // narrowing on an imported binding does not survive into a closure.
  const list = presetListElement;
  const label = presetChooserLabel;

  if (list) {
    // Delegated, because the rows are re-rendered every time the list is filtered.
    list.addEventListener("pointerdown", (event) => {
      const pressed = event.target instanceof Element ? event.target : null;
      // The rating stars and the mixer toggle are buttons of their own.
      if (!pressed || pressed.closest("button, input, select, textarea")) {
        return;
      }
      const row = pressed.closest<HTMLElement>("article.preset-item");
      const presetId = row?.dataset.id ?? "";
      if (row && presetId) {
        beginPresetDrag(event, row, "article.preset-item", presetId);
      }
    });
  }

  if (label) {
    label.addEventListener("pointerdown", (event) => {
      const presetId = uiState.activePresetId ?? "";
      if (presetId) {
        beginPresetDrag(event, label, ".preset-chooser-label", presetId);
      }
    });
  }

  registerPresetDropZone(presetFolderTreeElement, ".preset-folder-item[data-folder-id]", (presetId, folder) => {
    if (folder.folderId) {
      movePresetToFolder(presetId, folder.folderId);
    }
  });

  registerPresetDropZone(setlistSlotsElement, "#setlist-slots", (presetId) => {
    addPresetToSetlist(presetId);
    setSetlistExpanded(true);
  });
}

/** Test hook: forget every registered zone. */
export function resetPresetDropZonesForTests(): void {
  zones.length = 0;
}
