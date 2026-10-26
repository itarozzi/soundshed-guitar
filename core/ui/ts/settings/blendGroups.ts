/**
 * The blend controls that live inside the grouped library view: making a blend
 * out of a tone group, deleting one, and dragging models between them.
 */

import {
  BLEND_EDITOR_OPEN_EVENT,
  RESOURCE_GROUP_DRAG_START_EVENT,
  buildBlendModelMappingsFromIds,
  findBlendForToneGroup,
  type BlendEditorOpenDetail,
  type ResourceGroupDragStartDetail,
} from "../blendUtils.js";
import type { BlendDefinition } from "../types.js";
import { postMessage } from "../bridge.js";
import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import { libraryResults } from "./dom.js";
import { inferResourceOrigin } from "./libraryData.js";
import type { ToneGroup } from "./libraryData.js";
import { buildUsedResourceSet } from "./resourceUsage.js";

export function bindBlendCreateButtons(groups: ToneGroup[]): void {
  const buttons = libraryResults?.querySelectorAll(".equipment-library-create-blend");
  if (!buttons) {
    return;
  }

  buttons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const groupId = (btn as HTMLElement).dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group) {
        return;
      }
      createBlendFromGroup(group);
    });
  });
}

export function bindBlendDeleteButtons(groups: ToneGroup[]): void {
  const buttons = libraryResults?.querySelectorAll(".equipment-library-delete-group");
  if (!buttons) {
    return;
  }

  const usedResources = buildUsedResourceSet();

  buttons.forEach((btn) => {
    btn.addEventListener("click", async () => {
      const groupId = (btn as HTMLElement).dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group) {
        return;
      }

      const deletable = group.items.filter((item) =>
        inferResourceOrigin(item.filePath, item.metadata) === "Imported"
        && !usedResources.has(`${item.type}:${item.id}`)
      );
      const usedCount = group.items.filter((item) => usedResources.has(`${item.type}:${item.id}`)).length;
      const skipped = group.items.length - deletable.length;

      if (!deletable.length) {
        const reason = usedCount > 0
          ? "All resources in this group are used by presets or blends."
          : "No deletable imported resources found.";
        showNotification("Delete group", reason);
        return;
      }

      const message = `Delete ${deletable.length} resources from "${group.title}"?`
        + (usedCount ? ` ${usedCount} used resources will be kept.` : "")
        + (skipped ? ` ${skipped} non-deletable resources will be kept.` : "");

      const confirmed = await showConfirm(message, "Delete group");
      if (!confirmed) {
        return;
      }

      postMessage({
        type: "cleanupResourceLibrary",
        scope: "all",
        removeFiles: true,
        resources: deletable.map((item) => ({ type: item.type, id: item.id })),
      });
    });
  });
}

export function bindBlendGroupDragHandlers(groups: ToneGroup[]): void {
  const items = libraryResults?.querySelectorAll(".equipment-library-group") as NodeListOf<HTMLElement> | null;
  if (!items) {
    return;
  }

  // Dragged by pointer (pointerDrag.ts, started by the signal path), which an iPhone can do
  // and HTML5 drag-and-drop it cannot. The buttons and the tone3000 link keep their presses.
  items.forEach((item) => {
    item.addEventListener("pointerdown", (event: PointerEvent) => {
      if (event.target instanceof Element && event.target.closest("button, a")) {
        return;
      }
      const groupId = item.dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group) {
        return;
      }

      const detail: ResourceGroupDragStartDetail = {
        source: item,
        pointerEvent: event,
        payload: {
          groupId: group.groupId,
          title: group.title,
          category: normalizeBlendCategory(group.gear),
          modelIds: group.modelIds,
          modelMappings: buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary),
        },
      };
      document.dispatchEvent(new CustomEvent<ResourceGroupDragStartDetail>(RESOURCE_GROUP_DRAG_START_EVENT, { detail }));
    });
  });
}

/**
 * Opens the blend editor on the group's models: the blend already made from exactly them, or a
 * new one, which the editor places from the models' names and saves only on Save.
 */
export function createBlendFromGroup(group: ToneGroup): void {
  if (!group.modelIds.length) {
    showNotification("Blend creation failed", "No amp models found in this group.");
    return;
  }
  const existing = findBlendForToneGroup(uiState.blendLibrary ?? [], group.groupId, group.modelIds);
  const blend: BlendDefinition = existing ?? {
    id: typeof crypto !== "undefined" && "randomUUID" in crypto
      ? crypto.randomUUID()
      : `${Date.now()}-${Math.random().toString(16).slice(2)}`,
    name: group.title,
    category: normalizeBlendCategory(group.gear),
    models: [...group.modelIds],
    modelMappings: [],
    blendMode: "interpolate",
  };
  document.dispatchEvent(new CustomEvent<BlendEditorOpenDetail>(BLEND_EDITOR_OPEN_EVENT, { detail: { blend } }));
}

export function normalizeBlendCategory(category?: string): string {
  const value = (category ?? "").toLowerCase();
  const allowed = new Set(["pedal", "preamp", "amp", "full-rig", "cab"]);
  if (allowed.has(value)) {
    return value;
  }
  return "amp";
}
