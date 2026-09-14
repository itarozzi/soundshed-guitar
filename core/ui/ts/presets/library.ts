/**
 * The preset list as the user sees it: the tag filter, the filtered set, the
 * dropdown, and the library rows.
 */

import { bindDemoAudioControls } from "../demoAudio.js";
import { switchMainPanel } from "../navigation.js";
import { getPresetRating, getRecentPresets, loadFavoritePresetIds, setPresetRating } from "../presets/favorites.js";
import { PRESET_FOLDER_ALL_ID } from "../presets/folderArchive.js";
import { collectPresetIds, findFolderById, getPresetFolderPath, isVirtualPresetFolderId, sortPresetFoldersAlphabetically } from "../presets/folders.js";
import { PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID, sortPresetsAlphabetically } from "../presets/sorting.js";
import { renderSignalPathBar } from "../signalPath.js";
import { getActivePresetForRender, uiState } from "../state.js";
import type { Preset } from "../types.js";
import { renderMixerPanel, renderPresetDetails, renderPresetList } from "../views.js";
import { updatePresetFolderExportButtons } from "./actions.js";
import { presetChooserLabel } from "./dom.js";
import { movePresetFolder, setActivePresetFolder, syncPresetFolderToolbarState } from "./folderControls.js";
import { applyPresetFromLibrary, bindLoadButtons, requestSignalPathTest } from "./load.js";
import { closePresetLibraryPopover, takePresetChooserOverride } from "./popover.js";

export const activeTagFilters = new Set<string>();

export function getFilteredPresets(query: string): Preset[] {
  const normalized = query.trim().toLowerCase();
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  const preserveOrder = activeFolderId === PRESET_FOLDER_RECENTS_ID;
  let basePresets = uiState.presets.slice();

  if (activeFolderId === PRESET_FOLDER_FAVORITES_ID) {
    const favorites = loadFavoritePresetIds();
    basePresets = basePresets.filter((preset) => favorites.has(preset.id));
  }

  if (activeFolderId === PRESET_FOLDER_RECENTS_ID) {
    basePresets = getRecentPresets();
  }

  if (!isVirtualPresetFolderId(activeFolderId)) {
    const folder = findFolderById(uiState.presetFolders ?? [], activeFolderId);
    if (folder) {
      const allowedIds = collectPresetIds(folder);
      basePresets = basePresets.filter((preset) => allowedIds.has(preset.id));
    }
  }

  if (activeTagFilters.size > 0) {
    basePresets = basePresets.filter((preset) => {
      const presetTags = preset.tags ?? [];
      return Array.from(activeTagFilters).every((tag) => presetTags.includes(tag));
    });
  }

  if (!normalized) {
    return preserveOrder ? basePresets : sortPresetsAlphabetically(basePresets);
  }

  const filteredPresets = basePresets.filter((preset) => {
    const tokens = [preset.name, preset.category, preset.description];
    return tokens.some((token) => token && token.toLowerCase().includes(normalized));
  });

  return preserveOrder ? filteredPresets : sortPresetsAlphabetically(filteredPresets);
}

export function renderPresetUI(preset: Preset | null): void {
  syncPresetFolderToolbarState();

  const visiblePresets = uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID
    ? uiState.filteredPresets
    : sortPresetsAlphabetically(uiState.filteredPresets);

  renderPresetList(visiblePresets, uiState.activePresetId, async (presetId) => {
    const override = takePresetChooserOverride();
    if (override) {
      await override(presetId);
      closePresetLibraryPopover();
      return;
    }
    await applyPresetFromLibrary(presetId);
  }, {
    folders: sortPresetFoldersAlphabetically(uiState.presetFolders ?? []),
    activeFolderId: uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID,
    onSelectFolder: setActivePresetFolder,
    onMoveFolder: movePresetFolder,
    getRating: getPresetRating,
    onRate: setPresetRating,
    getFolderPath: getPresetFolderPath,
    recentsCount: getRecentPresets().length,
    recentsActive: uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID,
    onSelectRecents: () => setActivePresetFolder(PRESET_FOLDER_RECENTS_ID),
    favoritesCount: loadFavoritePresetIds().size,
    favoritesActive: uiState.activePresetFolderId === PRESET_FOLDER_FAVORITES_ID,
    onSelectFavorites: () => setActivePresetFolder(PRESET_FOLDER_FAVORITES_ID),
    hasAnyPresets: uiState.presets.length > 0,
    onOpenToneSharing: () => {
      closePresetLibraryPopover();
      switchMainPanel("sharing");
    },
  });

  renderPresetDetails(preset, {
    onPresetSelected: async (presetId) => {
      await applyPresetFromLibrary(presetId);
    },
    onApplyPreset: async (presetId) => {
      await applyPresetFromLibrary(presetId);
    },
    onRequestSignalTest: requestSignalPathTest,
    onBindLoadButtons: bindLoadButtons,
  });
  bindDemoAudioControls();
  renderSignalPathBar();
  updatePresetFolderExportButtons();
}

export function renderActivePreset(): void {
  const active = getActivePresetForRender();
  renderPresetUI(active);
  renderMixerPanel();
}

export function filterPresets(query: string): void {
  uiState.filteredPresets = getFilteredPresets(query);
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

export function initializePresetTagFilterBar(): void {
  const bar = document.getElementById("preset-tag-filter-bar");
  if (!bar) return;
  bar.querySelectorAll<HTMLButtonElement>(".preset-tag-filter-chip").forEach((btn) => {
    btn.addEventListener("click", () => {
      const tag = btn.dataset.tag ?? "";
      if (!tag) return;
      if (activeTagFilters.has(tag)) {
        activeTagFilters.delete(tag);
        btn.classList.remove("active");
      } else {
        activeTagFilters.add(tag);
        btn.classList.add("active");
      }
      const searchInput = document.getElementById("preset-search") as HTMLInputElement | null;
      filterPresets(searchInput?.value ?? "");
    });
  });
}

export function populatePresetDropdown(): void {
  updatePresetDropdownSelection();
}

export function updatePresetDropdownSelection(): void {
  if (!presetChooserLabel) return;
  const preset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  presetChooserLabel.textContent = preset?.name ?? "Select Preset";
}
