/**
 * The preset library popover and the overflow menu in the toolbar.
 *
 * Opening the popover to pick a preset for something else (a mixer slot, a
 * scene) installs an override, so the same UI can return a choice instead of
 * loading it.
 */

import { getCompositePresetList } from "../bridge.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "../featureFlags.js";
import { presetSearchElement } from "../presets/dom.js";
import { setFavoriteToggleState } from "../presets/favorites.js";
import { clonePreset, uiState } from "../state.js";
import { presetChooserLabel, presetControlBar, presetExtraActionsBtn, presetExtraActionsMenu, presetLibraryMultiRigPanel, presetLibraryMultiRigTab, presetLibraryPopover, presetLibraryPresetsPanel, presetLibraryPresetsTab, presetLibraryTabs } from "./dom.js";
import { filterPresets, updatePresetDropdownSelection } from "./filter.js";
import { requestPresetUIRender } from "./refresh.js";
import { updatePresetActionButtons } from "./toolbar.js";
import { setSetlistPanelVisible } from "./setlists.js";

export function syncPresetHeaderPopoverLayer(): void {
  const hasOpenPopover = Boolean(
    presetLibraryPopover?.classList.contains("open") || presetExtraActionsMenu?.classList.contains("open"),
  );
  presetControlBar?.classList.toggle("has-open-popover", hasOpenPopover);
}

let presetChooserOverride: ((presetId: string) => void | Promise<void>) | null = null;

let presetChooserCloseOverride: (() => void) | null = null;

export function openPresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  closePresetExtraActionsMenu();
  syncPresetLibraryFeatureVisibility();
  // Refresh the Multi-Rig list on every open (not just once at app boot,
  // when app settings — and therefore the MultiRig feature flag — may not
  // have finished loading yet) so the tab reliably shows up for anyone with
  // saved Multi-Rig presets, and stays in sync with other sessions/windows.
  if (isFeatureEnabled(Features.MultiRig)) {
    getCompositePresetList();
  }
  const controlBar = presetLibraryPopover.closest<HTMLElement>(".control-bar");
  controlBar?.classList.remove("is-collapsed");
  const collapseButton = document.getElementById("control-bar-collapse-btn");
  collapseButton?.setAttribute("aria-expanded", "true");
  collapseButton?.setAttribute("aria-label", "Collapse controls");
  if (collapseButton instanceof HTMLElement) {
    collapseButton.title = "Collapse controls";
  }
  presetLibraryPopover.classList.add("open");
  presetLibraryPopover.setAttribute("aria-hidden", "false");
  presetChooserLabel?.setAttribute("aria-expanded", "true");
  syncPresetHeaderPopoverLayer();
}

export function closePresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  const onClose = presetChooserCloseOverride;
  presetLibraryPopover.classList.remove("open");
  presetLibraryPopover.setAttribute("aria-hidden", "true");
  presetChooserLabel?.setAttribute("aria-expanded", "false");
  presetChooserOverride = null;
  presetChooserCloseOverride = null;
  onClose?.();
  syncPresetHeaderPopoverLayer();
}

/**
 * Hands back the pending "pick a preset for something else" callback and
 * clears it, so the caller that acts on a row consumes the override rather
 * than leaving it armed for the next click.
 */
export function takePresetChooserOverride(): ((presetId: string) => void | Promise<void>) | null {
  const override = presetChooserOverride;
  presetChooserOverride = null;
  return override;
}

export function openPresetChooserForSelection(
  onSelect: (presetId: string) => void | Promise<void>,
  onClose?: () => void,
): void {
  presetChooserOverride = onSelect;
  presetChooserCloseOverride = onClose ?? null;
  openPresetLibraryPopover();
  presetSearchElement?.focus({ preventScroll: true });
}

export function togglePresetLibraryPopover(): void {
  if (!presetLibraryPopover) {
    return;
  }
  if (presetLibraryPopover.classList.contains("open")) {
    closePresetLibraryPopover();
  } else {
    openPresetLibraryPopover();
  }
}

export function openPresetExtraActionsMenu(): void {
  if (!presetExtraActionsBtn || !presetExtraActionsMenu) {
    return;
  }
  closePresetLibraryPopover();
  presetExtraActionsMenu.classList.add("open");
  presetExtraActionsMenu.setAttribute("aria-hidden", "false");
  presetExtraActionsBtn.setAttribute("aria-expanded", "true");
  syncPresetHeaderPopoverLayer();
}

export function closePresetExtraActionsMenu(): void {
  if (!presetExtraActionsBtn || !presetExtraActionsMenu) {
    return;
  }
  presetExtraActionsMenu.classList.remove("open");
  presetExtraActionsMenu.setAttribute("aria-hidden", "true");
  presetExtraActionsBtn.setAttribute("aria-expanded", "false");
  syncPresetHeaderPopoverLayer();
}

export function togglePresetExtraActionsMenu(): void {
  if (!presetExtraActionsMenu) {
    return;
  }
  if (presetExtraActionsMenu.classList.contains("open")) {
    closePresetExtraActionsMenu();
  } else {
    openPresetExtraActionsMenu();
  }
}

export function syncPresetLibraryFeatureVisibility(): void {
  // The Multi-Rig tab is part of the library whenever the feature is on. Its
  // empty state explains how to build a first mix, so it is worth showing even
  // before anything has been saved — hiding it until then left the feature
  // undiscoverable.
  const showMultiRigTab = isFeatureEnabled(Features.MultiRig);

  presetLibraryPopover?.classList.toggle("preset-library-popover-simple", !showMultiRigTab);

  if (presetLibraryTabs) {
    presetLibraryTabs.hidden = !showMultiRigTab;
    presetLibraryTabs.setAttribute("aria-hidden", String(!showMultiRigTab));
  }

  if (presetLibraryMultiRigTab) {
    presetLibraryMultiRigTab.hidden = !showMultiRigTab;
    presetLibraryMultiRigTab.setAttribute("aria-hidden", String(!showMultiRigTab));
    presetLibraryMultiRigTab.tabIndex = showMultiRigTab ? 0 : -1;
  }

  // Whenever the tab itself isn't shown, force the view back to the normal
  // Presets panel — this is what stops a since-removed/never-fetched
  // Multi-Rig tab from leaving the preset list stuck hidden behind it.
  if (!showMultiRigTab) {
    presetLibraryPresetsTab?.classList.add("active");
    presetLibraryPresetsTab?.setAttribute("aria-selected", "true");
    presetLibraryMultiRigTab?.classList.remove("active");
    presetLibraryMultiRigTab?.setAttribute("aria-selected", "false");
    if (presetLibraryPresetsPanel) {
      presetLibraryPresetsPanel.hidden = false;
    }
    if (presetLibraryMultiRigPanel) {
      presetLibraryMultiRigPanel.hidden = true;
    }
    setSetlistPanelVisible(true);
  }
}

document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => {
  syncPresetLibraryFeatureVisibility();
  filterPresets(presetSearchElement?.value ?? ""); // the "+ Mixer" buttons follow the Multi-Rig flag
});

document.addEventListener("mixerPresetTabSelected", (event) => {
  const customEvent = event as CustomEvent<{ presetId?: string }>;
  const presetId = customEvent.detail?.presetId ?? "";
  if (!presetId) {
    return;
  }

  const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((candidate) => candidate.id === presetId) ?? null;
  if (!preset) {
    return;
  }

  uiState.activePresetId = presetId;
  setFavoriteToggleState(presetId);
  updatePresetDropdownSelection();
  requestPresetUIRender(clonePreset(preset));
  updatePresetActionButtons();
});
