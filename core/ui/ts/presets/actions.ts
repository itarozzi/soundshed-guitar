/**
 * What can be done to the preset that is currently loaded — overwrite, edit,
 * delete, publish — and the toolbar state that says which of those are allowed.
 *
 * A factory preset cannot be modified in place, so most of these first check
 * whether the active preset is the user's own.
 */

import { postMessage } from "../bridge.js";
import { buildAttachmentsFromPreset } from "../dataLibraries.js";
import { showConfirm } from "../dialogs.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { switchMainPanel } from "../navigation.js";
import { showNotification } from "../notifications.js";
import { exportCurrentPresetArchive, exportPresetArchiveSession, getToneSharingOriginMetadata, importPackWithConfirmation } from "../presets/archive.js";
import { cachePresetInMemory } from "../presets/cache.js";
import { presetSearchElement } from "../presets/dom.js";
import { loadFavoritePresetIds, saveFavoritePresetIds } from "../presets/favorites.js";
import { PRESET_FOLDER_ALL_ID, getPresetsForFolderId } from "../presets/folderArchive.js";
import { findFolderById, persistPresetFolders, removePresetFromFolders } from "../presets/folders.js";
import { setPresetLibraryRefresher } from "../presets/refresh.js";
import { getPresetArchiveSessionState } from "../presets/sanitize.js";
import { PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "../presets/sorting.js";
import { clonePreset, getActivePresetForRender, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import { isToneSharingSignedIn, openToneSharingPublishPresetModal, openToneSharingSignInModal } from "../toneSharingPanel.js";
import type { Preset } from "../types.js";
import { presetExitSessionButton, presetExportFolderButton, presetExportSessionButton, presetSelector, presetSelectorStatus } from "./dom.js";
import { populatePresetFolderSelect } from "./folderControls.js";
import { initPresetModalAdvancedActions, initPresetModalTabs, setPresetModalActiveTab, updatePresetModalJson, updatePresetModalReport } from "./inspectModal.js";
import { getFilteredPresets, populatePresetDropdown, renderActivePreset, renderPresetUI, updatePresetDropdownSelection } from "./library.js";
import { applyPresetFromLibrary } from "./load.js";
import { closePresetExtraActionsMenu, togglePresetExtraActionsMenu } from "./popover.js";
import { configureSavePresetModalLabels, createDefaultPreset, isActivePresetNewDraft, populateSavePresetModalFields, resolvePresetModalFolderId, updateSavePresetModalPeakInfo } from "./saveModal.js";
import { stripGlobalSignalChainForSave } from "./validate.js";

export function openPublishPresetFlow(): void {
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  if (!activePreset) {
    showNotification("No preset", "Select a preset to publish.");
    return;
  }

  const toneSharingOrigin = getToneSharingOriginMetadata(activePreset);
  if (toneSharingOrigin?.republishBlocked) {
    showNotification("Save As first", "Imported Tone Sharing presets need a local copy before they can be published again.");
    return;
  }

  if (!isToneSharingSignedIn()) {
    switchMainPanel("sharing");
    openToneSharingSignInModal();
    return;
  }

  openToneSharingPublishPresetModal(activePreset.name ?? "", activePreset.description ?? "");
}

// Delete preset via backend storage
export function deletePresetFromBackend(presetId: string): boolean {
  if (!presetId) return false;
  postMessage({ type: "deletePreset", presetId });
  return true;
}

// Check if preset is a user preset (can be edited/deleted)
export function isUserPreset(presetId: string | null): boolean {
  if (!presetId) return false;
  const preset = uiState.presetCache.get(presetId);
  if (!preset) return false;
  // User presets have numeric IDs (timestamps), start with "user-", or use UUIDs
  return /^\d+$/.test(presetId)
    || presetId.startsWith("user-")
    || /^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(presetId);
}

// Check if preset can be modified (user preset OR factory preset editing enabled)
export function canModifyPreset(presetId: string | null): boolean {
  if (getPresetArchiveSessionState()) {
    return Boolean(presetId);
  }
  if (isUserPreset(presetId)) {
    return true;
  }
  // Allow factory preset modification when feature flag is enabled
  return isFeatureEnabled(Features.FactoryPresetArchives);
}

// Delete current preset
export async function deleteCurrentPreset(): Promise<void> {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!canModifyPreset(activePresetId)) {
    showNotification("Error", "Cannot delete factory presets");
    return;
  }

  const preset = uiState.presetCache.get(activePresetId);
  const presetName = preset?.name ?? "Unknown";

  const confirmed = await showConfirm(`Are you sure you want to delete "${presetName}"?`, "Delete preset");
  if (!confirmed) {
    return;
  }

  if (deletePresetFromBackend(activePresetId)) {
    // Remove from UI state
    const index = uiState.presets.findIndex((p) => p.id === activePresetId);
    if (index >= 0) {
      uiState.presets.splice(index, 1);
    }
    removePresetFromFolders(uiState.presetFolders ?? [], activePresetId);
    persistPresetFolders();
    const favorites = loadFavoritePresetIds();
    if (favorites.delete(activePresetId)) {
      saveFavoritePresetIds(favorites);
    }
    uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
    uiState.presetCache.delete(activePresetId);

    // The deleted preset's unsaved changes went with it; loading the next one has nothing to confirm.
    setPresetDirty(false);

    // Select first preset if available
    if (uiState.presets.length > 0) {
      uiState.activePresetId = uiState.presets[0].id;
      void applyPresetFromLibrary(uiState.activePresetId);
    } else {
      uiState.activePresetId = null;
    }

    populatePresetDropdown();
    renderActivePreset();
    showNotification("Preset deleted", presetName);
  } else {
    showNotification("Error", "Failed to delete preset");
  }
}

// Save (overwrite) current preset
export function saveOverwriteCurrentPreset(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (isActivePresetNewDraft()) {
    openEditPresetModal();
    return;
  }

  if (!canModifyPreset(activePresetId)) {
    showNotification("Error", "Cannot overwrite factory presets. Use 'Save As' instead.");
    return;
  }

  const existingPreset = getActivePresetForRender();
  if (!existingPreset) {
    showNotification("Error", "Preset not found");
    return;
  }

  // Build updated preset with current parameters from graph nodes
  const baseAttachments = buildAttachmentsFromPreset(existingPreset);
  const includeGlobalFx = false;

  const updatedPreset: Preset = {
    ...existingPreset,
    attachments: baseAttachments,
  };
  delete (updatedPreset as Record<string, unknown>).globalSignalChain;

  cachePresetInMemory(updatedPreset);
  // Persist to disk via the C++ backend
  const savePayload: Record<string, unknown> = {
    type: "savePreset",
    saveMode: "overwrite",
    presetId: updatedPreset.id,
    name: updatedPreset.name,
    category: updatedPreset.category,
    description: updatedPreset.description,
    includeGlobalSignalChain: includeGlobalFx,
    preset: stripGlobalSignalChainForSave(updatedPreset),
  };
  postMessage(savePayload);

  // Update cache
  uiState.presetCache.set(activePresetId, updatedPreset);
  const index = uiState.presets.findIndex((p) => p.id === activePresetId);
  if (index >= 0) {
    uiState.presets[index] = updatedPreset;
  }

  setActivePresetIsNew(false);
  setActivePresetSnapshot(updatedPreset);
  setActivePresetDraft(updatedPreset);
  setPresetDirty(false);
  showNotification("Preset saved", existingPreset.name);
}

// Open edit preset modal (reuses save modal with pre-filled data)
export function openEditPresetModal(): void {
  const activePresetId = uiState.activePresetId;
  if (!activePresetId) {
    showNotification("Error", "No preset selected");
    return;
  }

  if (!canModifyPreset(activePresetId)) {
    showNotification("Error", "Cannot edit factory presets. Use 'Save As' instead.");
    return;
  }

  const preset = getActivePresetForRender();
  if (!preset) {
    showNotification("Error", "Preset not found");
    return;
  }

  const modal = document.getElementById("save-preset-modal");
  if (!modal) return;
  const isNewPresetDraft = isActivePresetNewDraft();
  configureSavePresetModalLabels(isNewPresetDraft ? "save-new" : "overwrite");
  modal.dataset.saveMode = isNewPresetDraft ? "save-new" : "overwrite";
  delete modal.dataset.sourcePresetId;

  const folderSelect = document.getElementById("preset-folder-select") as HTMLSelectElement | null;
  populatePresetFolderSelect(folderSelect, resolvePresetModalFolderId(activePresetId));
  populateSavePresetModalFields(preset);

  initPresetModalTabs(modal);
  initPresetModalAdvancedActions(modal);
  setPresetModalActiveTab(modal, "details");
  updatePresetModalJson(preset);
  updatePresetModalReport([]);
  delete modal.dataset.cleanedPreset;
  delete modal.dataset.stagedDesignedPeak;
  updateSavePresetModalPeakInfo(modal);

  // Store that we're editing, not creating
  modal.dataset.editingPresetId = activePresetId;

  modal.style.display = "flex";
  const nameInput = document.getElementById("preset-name-input") as HTMLInputElement | null;
  nameInput?.focus();
  nameInput?.select();
}

// Update preset action button states
export function updatePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn") as HTMLButtonElement | null;
  const saveBtn = document.getElementById("preset-save-btn") as HTMLButtonElement | null;
  const saveAsBtn = document.getElementById("preset-save-as-btn") as HTMLButtonElement | null;
  const deleteBtn = document.getElementById("preset-delete-btn") as HTMLButtonElement | null;
  const exportBtn = document.getElementById("preset-export-btn") as HTMLButtonElement | null;
  const importBtn = document.getElementById("preset-import-btn") as HTMLButtonElement | null;
  const publishBtn = document.getElementById("preset-publish-btn") as HTMLButtonElement | null;

  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  const canModify = canModifyPreset(uiState.activePresetId);
  const toneSharingOrigin = getToneSharingOriginMetadata(activePreset);
  const hasActivePreset = Boolean(activePreset);
  const isNewPresetDraft = Boolean(hasActivePreset && isActivePresetNewDraft());
  const archiveSession = getPresetArchiveSessionState();

  if (editBtn) {
    editBtn.hidden = isNewPresetDraft;
    editBtn.disabled = !canModify;
    editBtn.title = canModify
      ? (isNewPresetDraft ? "Set preset details before saving" : "Edit preset details")
      : "Save As to edit factory presets";
  }
  if (saveBtn) {
    saveBtn.disabled = !canModify;
    saveBtn.title = canModify
      ? (isNewPresetDraft ? "Save preset details first" : "Save Preset")
      : "Cannot overwrite factory presets";
    saveBtn.classList.toggle("preset-action-btn-unsaved", Boolean(uiState.presetDirty));
  }
  if (saveAsBtn) {
    const showSaveAs = hasActivePreset && !isNewPresetDraft;
    saveAsBtn.hidden = !showSaveAs;
    saveAsBtn.disabled = !showSaveAs;
    saveAsBtn.title = "Save this preset as a new copy";
  }
  if (deleteBtn) {
    deleteBtn.disabled = !canModify;
    deleteBtn.title = canModify ? "Delete Preset" : "Cannot delete factory presets";
  }
  if (exportBtn) {
    exportBtn.disabled = !hasActivePreset;
    exportBtn.title = hasActivePreset ? "Export preset file" : "No preset to export";
  }
  if (importBtn) {
    importBtn.disabled = false;
    importBtn.title = "Import a preset file";
  }
  if (presetExportSessionButton) {
    const exportTitle = archiveSession
      ? `Export archive session (${uiState.presets.length} presets)`
      : "No preset archive session active";
    presetExportSessionButton.hidden = !archiveSession;
    presetExportSessionButton.disabled = !archiveSession || uiState.presets.length === 0;
    presetExportSessionButton.title = exportTitle;
  }
  if (presetExitSessionButton) {
    presetExitSessionButton.hidden = !archiveSession;
    presetExitSessionButton.disabled = !archiveSession;
    presetExitSessionButton.title = archiveSession
      ? "Exit archive session and restore the normal preset library"
      : "No preset archive session active";
  }
  if (publishBtn) {
    publishBtn.disabled = !hasActivePreset;
    publishBtn.title = !hasActivePreset
      ? "No preset to publish"
      : toneSharingOrigin?.republishBlocked
        ? "Save As before publishing this imported preset"
        : !isToneSharingSignedIn()
          ? "Sign in to Tone Sharing to publish"
          : "Publish to Tone Sharing";
  }

  if (presetSelector) {
    presetSelector.classList.toggle("has-unsaved-changes", Boolean(activePreset && uiState.presetDirty));
  }
  if (presetSelectorStatus) {
    let status = "No preset";
    let statusTitle = "No preset selected";
    presetSelectorStatus.classList.remove("is-warning", "is-dirty");

    if (activePreset) {
      if (archiveSession) {
        status = "Archive";
        statusTitle = `Session-only preset archive: ${archiveSession.archiveName ?? "Preset archive"}. Restart or exit the session to restore the normal preset library.`;
      } else if (toneSharingOrigin?.republishBlocked) {
        status = "Imported";
        statusTitle = "Imported from Tone Sharing. Use Save As to publish.";
        presetSelectorStatus.classList.add("is-warning");
      } else if (isNewPresetDraft) {
        status = "New";
        statusTitle = "New preset draft. Save to set the title and store it.";
      } else if (canModify) {
        status = "User";
        statusTitle = "User preset";
      } else {
        status = "Factory";
        statusTitle = "Factory preset. Use Save As to edit.";
      }

      if (uiState.presetDirty) {
        status += " • Unsaved";
        statusTitle += " Unsaved changes.";
        presetSelectorStatus.classList.add("is-dirty");
      }
    }

    presetSelectorStatus.textContent = status;
    presetSelectorStatus.title = statusTitle;
  }
}

// Supply the real redraw to the modules that can only request one — see
// presets/refresh.ts for why the indirection exists.
setPresetLibraryRefresher((activePreset) => {
  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  populatePresetDropdown();
  if (activePreset) {
    renderPresetUI(clonePreset(activePreset));
  }
  updatePresetDropdownSelection();
  updatePresetActionButtons();
});

export function updatePresetFolderExportButtons(): void {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const folderPresets = getPresetsForFolderId(activeFolderId);
  const folderName = activeFolderId === PRESET_FOLDER_ALL_ID
    ? "All Presets"
    : activeFolderId === PRESET_FOLDER_FAVORITES_ID
      ? "Favourites"
      : activeFolderId === PRESET_FOLDER_RECENTS_ID
        ? "Recents"
      : (findFolderById(uiState.presetFolders ?? [], activeFolderId)?.name ?? "Folder");

  if (presetExportFolderButton) {
    const isAll = activeFolderId === PRESET_FOLDER_ALL_ID;
    const count = isAll ? uiState.presets.length : folderPresets.length;
    const archiveSession = getPresetArchiveSessionState();
    const title = count
      ? (archiveSession && isAll
        ? `Export archive session (${count})`
        : isAll
          ? `Export all presets (${count})`
          : `Export ${folderName} (${count})`)
      : (isAll ? "No presets to export" : `No presets in ${folderName}`);
    presetExportFolderButton.toggleAttribute("disabled", count === 0);
    presetExportFolderButton.title = title;
    presetExportFolderButton.setAttribute("aria-label", title);
  }
}

document.addEventListener("presetDirtyChanged", () => {
  updatePresetActionButtons();
});

// Initialize preset action buttons
export function initializePresetActionButtons(): void {
  const editBtn = document.getElementById("preset-edit-btn");
  const newBtn = document.getElementById("preset-new-btn");
  const saveBtn = document.getElementById("preset-save-btn");
  const saveAsBtn = document.getElementById("preset-save-as-btn");
  const deleteBtn = document.getElementById("preset-delete-btn");
  const publishBtn = document.getElementById("preset-publish-btn");
  const extraActionsBtn = document.getElementById("preset-extra-actions-btn") as HTMLButtonElement | null;
  const extraActionsMenu = document.getElementById("preset-extra-actions-menu");
  const exportBtn = document.getElementById("preset-export-btn");
  const exportSessionBtn = document.getElementById("preset-export-session-btn");
  const exitSessionBtn = document.getElementById("preset-exit-session-btn");
  const importBtn = document.getElementById("preset-import-btn");
  const importInput = document.getElementById("preset-import-input") as HTMLInputElement | null;

  if (editBtn) {
    editBtn.addEventListener("click", openEditPresetModal);
  }

  if (newBtn) {
    newBtn.addEventListener("click", createDefaultPreset);
  }

  if (saveBtn) {
    saveBtn.addEventListener("click", saveOverwriteCurrentPreset);
  }

  if (saveAsBtn) {
    saveAsBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
    });
  }

  if (deleteBtn) {
    deleteBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      void deleteCurrentPreset();
    });
  }

  if (publishBtn) {
    publishBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      if ((publishBtn as HTMLButtonElement).disabled) {
        return;
      }
      openPublishPresetFlow();
    });
  }

  if (extraActionsBtn) {
    extraActionsBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      togglePresetExtraActionsMenu();
    });
  }

  if (extraActionsMenu) {
    extraActionsMenu.addEventListener("click", (event) => {
      event.stopPropagation();
    });
  }

  if (exportBtn) {
    exportBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      if ((exportBtn as HTMLButtonElement).disabled) {
        return;
      }
      closePresetExtraActionsMenu();
      void exportCurrentPresetArchive();
    });
  }

  if (importBtn) {
    importBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      importInput?.click();
    });
  }

  if (exportSessionBtn) {
    exportSessionBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      void exportPresetArchiveSession();
    });
  }

  if (exitSessionBtn) {
    exitSessionBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      closePresetExtraActionsMenu();
      postMessage({ type: "endPresetArchiveSession" });
    });
  }

  if (importInput) {
    importInput.addEventListener("change", () => {
      const file = importInput.files?.[0];
      importInput.value = "";
      if (file) {
        void importPackWithConfirmation(file, { source: "zipImport" });
      }
    });
  }

  document.addEventListener("click", () => {
    closePresetExtraActionsMenu();
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      closePresetExtraActionsMenu();
    }
  });

  updatePresetActionButtons();
}
