/**
 * The controls of a model or IR slot (a library picker): Browse, the step arrows, the label that
 * names what is loaded (or asks for it), and Clear, with the file path under them. The params
 * panel draws its slots with it and a custom layout draws the same markup, so a slot placed in
 * a layout looks and works as the panel's does; resourceControls.ts binds both by these classes
 * and data attributes.
 */

import { EffectGuids } from "./effectGuids.js";
import { renderIcon } from "./iconAssets.js";
import { EffectTypeRegistry } from "./presetV2.js";
import { resourceBrowserModal } from "./resourceBrowser.js";
import { getResourcePickerLabelClass, getResourcePickerLabelTitle, renderResourcePickerLabelContent } from "./resourcePickerLabel.js";
import type { ResourcePickerLabelState } from "./resourcePickerLabel.js";
import { escapeHtml } from "./utils.js";

export interface LibraryPickerSlot {
  nodeId: string;
  resourceType: "nam" | "ir";
  resourceIndex: number;
  /** The panel's lone primary slot has always left data-resource-index off (its handlers read 0). */
  includeIndexAttr: boolean;
  exposedResourceId?: string;
  labelState: ResourcePickerLabelState;
  hasCurrentSelection: boolean;
  /** Hidden where the label already opens the library (neural models, the cabinet's IRs). */
  showBrowseButton: boolean;
  /** Whether the arrows can step back and forward; null draws no arrows. */
  navigation: { canPrev: boolean; canNext: boolean } | null;
  currentFilePath?: string;
}

/** True for the resource types drawn as a library picker. */
export function isLibraryPickerType(resourceType: string | undefined): resourceType is "nam" | "ir" {
  return resourceType === "nam" || resourceType === "ir";
}

/**
 * Whether a slot's arrows can step, from the resource browser's navigation list. A Tone3000
 * result set wraps, so both ways are open without fetching the neighbour.
 */
export function libraryPickerNavigation(
  resourceType: "nam" | "ir",
  currentResourceId: string,
  currentFilePath: string,
  navOptions: { categoryHint?: string; contextKey?: string },
): { canPrev: boolean; canNext: boolean } {
  if (resourceBrowserModal.isTone3000NavigationActive(resourceType, navOptions)) {
    return { canPrev: true, canNext: true };
  }
  return {
    canPrev: Boolean(resourceBrowserModal.getAdjacentResourceSelection(resourceType, currentResourceId, currentFilePath, -1, navOptions)),
    canNext: Boolean(resourceBrowserModal.getAdjacentResourceSelection(resourceType, currentResourceId, currentFilePath, 1, navOptions)),
  };
}

/** The slot's `.resource-controls` row, and its file path when one is loaded. */
export function libraryPickerControlsHtml(slot: LibraryPickerSlot): string {
  const nodeId = escapeHtml(slot.nodeId);
  const type = escapeHtml(slot.resourceType);
  const attrs = [
    `data-node-id="${nodeId}"`,
    `data-resource-type="${type}"`,
    slot.includeIndexAttr ? `data-resource-index="${slot.resourceIndex}"` : "",
    slot.exposedResourceId ? `data-exposed-resource-id="${escapeHtml(slot.exposedResourceId)}"` : "",
  ].filter(Boolean).join(" ");
  const navButton = (direction: "prev" | "next", enabled: boolean) => {
    const title = direction === "prev" ? "Previous resource" : "Next resource";
    return `<button type="button" class="preset-action-btn resource-nav-btn resource-nav-${direction}-btn" ${attrs} data-nav-direction="${direction}" title="${title}" aria-label="${title}"${enabled ? "" : " disabled"}>${renderIcon(direction === "prev" ? "arrow-left" : "arrow-right", "resource-nav-icon")}</button>`;
  };
  const filePath = slot.currentFilePath ?? "";
  return `
    <div class="resource-controls">
      ${slot.showBrowseButton ? `<button class="resource-picker-btn" ${attrs}>Browse</button>` : ""}
      ${slot.navigation ? navButton("prev", slot.navigation.canPrev) : ""}
      <button
        type="button"
        class="${escapeHtml(getResourcePickerLabelClass(slot.labelState))}"
        ${attrs}
        ${slot.labelState.isOptional ? `data-optional="true"` : ""}
        title="${escapeHtml(getResourcePickerLabelTitle(slot.labelState))}"
        aria-haspopup="dialog"
      >${renderResourcePickerLabelContent(slot.labelState)}</button>
      ${slot.navigation ? navButton("next", slot.navigation.canNext) : ""}
      <button class="resource-clear-btn" ${attrs} title="Clear selected resource"${slot.hasCurrentSelection ? "" : " disabled"}>${renderIcon("close", "resource-clear-icon")}</button>
    </div>
    ${filePath ? `<div class="resource-path-info" title="${escapeHtml(filePath)}">${escapeHtml(filePath)}</div>` : ""}
  `;
}

/**
 * Whether a model or IR slot on this effect gets a Browse button. Neural models and the cabinet's
 * IRs leave it off: their label opens the library already.
 */
export function librarySlotHasBrowseButton(effectType: string): boolean {
  const resolved = EffectTypeRegistry.resolve(effectType);
  return resolved !== EffectGuids.kAmpNamOptimized && resolved !== EffectGuids.kFxNam && resolved !== EffectGuids.kCabIr;
}
