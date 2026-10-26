/**
 * Moving a preset folder by pointer: nesting it in another, bringing it back to the top
 * from All Presets, and making it a sibling from a nested folder's indent.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { resetPointerDragForTests } from "../ts/pointerDrag";
import { INDENT_DROP_PX, beginPresetFolderDrag, resolveFolderDropParent } from "../ts/presets/folderDrag";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID } from "../ts/presets/sorting";

function pointer(type: string, x: number, y: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  Object.defineProperty(event, "isPrimary", { value: true });
  return event as PointerEvent;
}

let tree: HTMLElement;
let moves: [string, string][];
/** The element `document.elementFromPoint` reports, standing in for layout. */
let under: Element | null;

function row(folderId: string): HTMLElement {
  return tree.querySelector<HTMLElement>(`[data-folder-id="${folderId}"]`) as HTMLElement;
}

function drag(sourceId: string, onto: HTMLElement | null, releaseX = 100): void {
  const press = pointer("pointerdown", 100, 10);
  row(sourceId).dispatchEvent(press);
  beginPresetFolderDrag(press, row(sourceId), tree, (folderId, parentId) => moves.push([folderId, parentId]));
  under = onto;
  document.dispatchEvent(pointer("pointermove", releaseX, 60));
  document.dispatchEvent(pointer("pointerup", releaseX, 60));
  vi.runAllTimers();
}

beforeEach(() => {
  vi.useFakeTimers();
  document.body.innerHTML = `
    <div id="preset-folder-tree">
      <div class="preset-folder-item" data-folder-id="${PRESET_FOLDER_FAVORITES_ID}"></div>
      <div class="preset-folder-item" data-folder-id="${PRESET_FOLDER_ALL_ID}"></div>
      <div class="preset-folder-item" data-folder-id="rock" data-parent-id="${PRESET_FOLDER_ALL_ID}" data-depth="0"></div>
      <div class="preset-folder-item" data-folder-id="metal" data-parent-id="rock" data-depth="1"></div>
      <div class="preset-folder-item" data-folder-id="clean" data-parent-id="${PRESET_FOLDER_ALL_ID}" data-depth="0"></div>
    </div>
  `;
  tree = document.getElementById("preset-folder-tree") as HTMLElement;
  moves = [];
  under = null;
  document.elementFromPoint = () => under;
});

afterEach(() => {
  resetPointerDragForTests();
  vi.useRealTimers();
  document.body.innerHTML = "";
});

describe("resolveFolderDropParent", () => {
  it("nests in the folder dropped on, except from a nested folder's indent", () => {
    expect(resolveFolderDropParent({ folderId: "rock", parentId: PRESET_FOLDER_ALL_ID, depth: 0 }, 0)).toBe("rock");
    expect(resolveFolderDropParent({ folderId: "metal", parentId: "rock", depth: 1 }, INDENT_DROP_PX)).toBe("metal");
    expect(resolveFolderDropParent({ folderId: "metal", parentId: "rock", depth: 1 }, INDENT_DROP_PX - 1)).toBe("rock");
    expect(resolveFolderDropParent({ folderId: PRESET_FOLDER_ALL_ID, parentId: "", depth: 0 }, 0)).toBe(PRESET_FOLDER_ALL_ID);
  });
});

describe("beginPresetFolderDrag", () => {
  it("moves a folder into the one it is dropped on", () => {
    drag("clean", row("rock"));
    expect(moves).toEqual([["clean", "rock"]]);
    expect(row("rock").classList.contains("drag-over")).toBe(false);
  });

  it("makes it a sibling when dropped on a nested folder's indent", () => {
    // jsdom lays nothing out, so every row's left edge is 0: the release x is the offset.
    drag("clean", row("metal"), INDENT_DROP_PX - 4);
    expect(moves).toEqual([["clean", "rock"]]);
  });

  it("brings it back to the top level from All Presets", () => {
    drag("metal", row(PRESET_FOLDER_ALL_ID));
    expect(moves).toEqual([["metal", PRESET_FOLDER_ALL_ID]]);
  });

  it("moves nothing onto itself, Favourites, or outside the tree", () => {
    drag("rock", row("rock"));
    drag("rock", row(PRESET_FOLDER_FAVORITES_ID));
    const outside = document.createElement("div");
    outside.className = "preset-folder-item";
    outside.dataset.folderId = "clean";
    document.body.appendChild(outside);
    drag("rock", outside);
    expect(moves).toEqual([]);
  });

  it("does not drag the virtual folders", () => {
    drag(PRESET_FOLDER_ALL_ID, row("rock"));
    drag(PRESET_FOLDER_FAVORITES_ID, row("rock"));
    expect(moves).toEqual([]);
  });
});
