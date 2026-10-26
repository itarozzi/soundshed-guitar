/**
 * Dragging a folder in the preset chooser's tree onto another folder nests it
 * there; onto All Presets, it goes back to the top level. Dropped on the indent
 * of a nested folder, its first {@link INDENT_DROP_PX}, it becomes that folder's
 * sibling instead.
 *
 * Pointer-driven, for the reasons given in pointerDrag.ts. It used to be HTML5
 * drag-and-drop, which an iPhone's WKWebView never starts. The tree scrolls, so
 * a touch has to rest on the folder before it picks it up.
 */

import { TOUCH_DRAG_HOLD_MS, beginPointerDrag, getUiZoom, type PointerDragTarget } from "../pointerDrag.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "./sorting.js";

/** How much of a nested folder's left edge counts as its indent. */
export const INDENT_DROP_PX = 16;

const FOLDER_SELECTOR = ".preset-folder-item[data-folder-id]";

/** Read when the pointer reached the folder: a re-render can replace the element. */
interface FolderDropTarget extends PointerDragTarget {
  folderId: string;
  parentId: string;
  depth: number;
  left: number;
}

/**
 * The folder a drop makes the parent: the folder dropped on, or its own parent for
 * a drop on a nested folder's indent. `offsetX` is from the folder's left edge.
 */
export function resolveFolderDropParent(
  target: { folderId: string; parentId: string; depth: number },
  offsetX: number,
): string {
  if (target.folderId === PRESET_FOLDER_ALL_ID) {
    return PRESET_FOLDER_ALL_ID;
  }
  return target.depth > 0 && offsetX < INDENT_DROP_PX ? target.parentId : target.folderId;
}

function resolveFolderDropTarget(event: PointerEvent, tree: HTMLElement, draggedId: string): FolderDropTarget | null {
  const element = document.elementFromPoint(event.clientX, event.clientY)?.closest<HTMLElement>(FOLDER_SELECTOR);
  const folderId = element?.dataset.folderId ?? "";
  if (!element || !tree.contains(element) || folderId === draggedId
    || folderId === PRESET_FOLDER_RECENTS_ID || folderId === PRESET_FOLDER_FAVORITES_ID) {
    return null;
  }
  return {
    element,
    folderId,
    parentId: element.dataset.parentId ?? PRESET_FOLDER_ALL_ID,
    depth: Number(element.dataset.depth ?? "0"),
    left: element.getBoundingClientRect().left,
  };
}

/**
 * Track a press on a folder row of `tree` as a possible drag. All Presets,
 * Favourites and Recents are not folders of their own, and do not move.
 */
export function beginPresetFolderDrag(
  event: PointerEvent,
  item: HTMLElement,
  tree: HTMLElement,
  onMoveFolder: (folderId: string, parentId: string) => void,
): void {
  const folderId = item.dataset.folderId ?? "";
  if (!folderId || folderId === PRESET_FOLDER_ALL_ID || folderId === PRESET_FOLDER_RECENTS_ID || folderId === PRESET_FOLDER_FAVORITES_ID) {
    return;
  }
  beginPointerDrag<FolderDropTarget>(event, {
    source: item,
    sourceSelector: ".preset-folder-item",
    previewClass: "list-drag-preview",
    touchHoldMs: TOUCH_DRAG_HOLD_MS,
    autoScroll: tree,
    resolveTarget: (moveEvent) => resolveFolderDropTarget(moveEvent, tree, folderId),
    setTargetHighlight: (target, highlighted) => target.element.classList.toggle("drag-over", highlighted),
    onDrop: (target, gesture) => {
      if (!target) return;
      const offsetX = (event.clientX + gesture.deltaX - target.left) / getUiZoom();
      const parentId = resolveFolderDropParent(target, offsetX);
      // After the release, as presets/drag.ts explains: the move re-renders the tree.
      window.setTimeout(() => onMoveFolder(folderId, parentId), 0);
    },
  });
}
