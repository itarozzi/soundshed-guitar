/**
 * Presets — the entry point, and the public surface the rest of the UI imports.
 *
 * The work is in ./presets/, one module per part of the feature. This file starts
 * the library up and re-exports what other panels need, so a caller never has to
 * know which module a given preset operation happens to live in.
 */

import { postMessage } from "./bridge.js";
import { REMOTE_BASE_URL, getDefaultPresets, initializeDataLibraries } from "./dataLibraries.js";
import { presetSearchElement } from "./presets/dom.js";
import { setFavoriteToggleState } from "./presets/favorites.js";
import { ensurePresetFolders } from "./presets/folders.js";
import { getFilteredPresets, populatePresetDropdown, renderPresetUI, updatePresetDropdownSelection } from "./presets/library.js";
import { loadPresetIndex } from "./presets/load.js";
import { ensureSetlists, renderSetlistPanel } from "./presets/setlists.js";
import { uiState } from "./state.js";

export { deleteCurrentPreset, deletePresetFromBackend, initializePresetActionButtons, isUserPreset, openEditPresetModal, saveOverwriteCurrentPreset, updatePresetActionButtons } from "./presets/actions.js";
export { initializePresetControls, selectNextPreset, selectPreviousPreset } from "./presets/controls.js";
export { registerPresetDropZone } from "./presets/drag.js";
export { applyPresetFoldersFromBackend } from "./presets/folderControls.js";
export { filterPresets, initializePresetTagFilterBar, populatePresetDropdown, renderActivePreset, updatePresetDropdownSelection } from "./presets/library.js";
export { applyPresetFromLibrary, bindLoadButtons, loadIRFromPath, loadModelFromPath, loadPresetIndex, requestSignalPathTest } from "./presets/load.js";
export { openPresetChooserForSelection, syncPresetLibraryFeatureVisibility } from "./presets/popover.js";
export { applyPresetFavoritesFromBackend, applyPresetRatingsFromBackend, applyPresetRecentsFromAppSettings, recordRecentPreset } from "./presets/recents.js";
export { closeSavePresetModal, createDefaultPreset, initializeSaveAsButton, initializeSavePresetModal, openSavePresetModal, refreshSavePresetModalPeakInfoIfOpen, saveCurrentPreset } from "./presets/saveModal.js";
export { applySetlistCursorFromBackend, applySetlistsFromBackend, assignPresetToActiveSetlistSlot, clearActiveSetlistSlot, createSetlist, deleteActiveSetlist, isOnlyPlayingPreset, setSetlistPanelVisible, updateActiveSetlistDetails } from "./presets/setlists.js";

export { handlePresetDataMessage, refreshPresetCacheEntryFromBackend, rejectPendingPresetRequest } from "./presets/fetch.js";
export { cachePresetInMemory } from "./presets/cache.js";
export { applyPresetArchiveSessionState, sanitizePresetForArchive } from "./presets/sanitize.js";
export { buildPresetArchiveBlob, buildToneSharingPresetArchiveBlobs, handleDroppedPresetPack, importGeneratedPack, importPackWithConfirmation, importPresetArchive, resolveImportedPresetName, startPresetArchiveSessionFromFile } from "./presets/archive.js";

export async function initializePresets(): Promise<void> {
  await initializeDataLibraries();

  if (REMOTE_BASE_URL) {
    await loadPresetIndex();
  } else {
    const basePresets = getDefaultPresets();
    uiState.presets = [...basePresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => uiState.presetCache.set(preset.id, preset));
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }

  // Backend-backed user data
  postMessage({ type: "getPresetList" });
  postMessage({ type: "getPresetFolders" });
  postMessage({ type: "getPresetFavorites" });
  postMessage({ type: "getPresetRatings" });
  postMessage({ type: "getSetlists" });
  postMessage({ type: "getEffectPresets" });

  ensurePresetFolders(false);  // Don't persist — backend response will arrive with saved data
  ensureSetlists();
  uiState.filteredPresets = getFilteredPresets(presetSearchElement?.value ?? "");
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  renderSetlistPanel();
  updatePresetDropdownSelection();
  setFavoriteToggleState(uiState.activePresetId);

  populatePresetDropdown();
  postMessage({ type: "requestState" });
}
