/**
 * The blend controls that live inside the grouped library view: making a blend
 * out of a tone group, deleting one, and dragging models between them.
 */

import { buildBlendModelMappingsFromIds } from "../blendUtils.js";
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
      void createBlendFromGroup(group);
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

  items.forEach((item) => {
    item.addEventListener("dragstart", (event: DragEvent) => {
      const groupId = item.dataset.groupId ?? "";
      const group = groups.find((entry) => entry.groupId === groupId);
      if (!group || !event.dataTransfer) {
        return;
      }

      const payload = {
        groupId: group.groupId,
        title: group.title,
        category: normalizeBlendCategory(group.gear),
        modelIds: group.modelIds,
        modelMappings: buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary),
      };
      event.dataTransfer.setData("application/x-resource-group", JSON.stringify(payload));
      event.dataTransfer.effectAllowed = "copy";
      item.classList.add("dragging");
      document.body.classList.add("fx-dragging");
    });

    item.addEventListener("dragend", () => {
      item.classList.remove("dragging");
      document.body.classList.remove("fx-dragging");
    });
  });
}

export async function createBlendFromGroup(group: ToneGroup): Promise<void> {
  const id = typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const name = prompt("Blend name", group.title) ?? group.title;
  if (!name.trim()) {
    return;
  }

  const snapMode = await showConfirm("Snap between models? Click OK for snap, Cancel for interpolate.", "Blend Mode");

  const category = normalizeBlendCategory(group.gear);
  if (!group.modelIds.length) {
    showNotification("Blend creation failed", "No amp models found in this group.");
    return;
  }

  const modelMappings = buildBlendModelMappingsFromIds(group.modelIds, uiState.resourceLibrary);
  const blend = {
    id,
    name: name.trim(),
    category,
    models: modelMappings.map((mapping) => mapping.id),
    modelMappings,
    blendMode: snapMode ? "snap" : "interpolate",
    toneGroupId: group.groupId,
    toneGroupTitle: group.title,
  };

  postMessage({
    type: "saveBlendDefinition",
    blend,
  });
}

export function normalizeBlendCategory(category?: string): string {
  const value = (category ?? "").toLowerCase();
  const allowed = new Set(["pedal", "preamp", "amp", "full-rig", "cab"]);
  if (allowed.has(value)) {
    return value;
  }
  return "amp";
}
