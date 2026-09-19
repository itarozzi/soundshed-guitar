/**
 * The per-user marks on a preset that the backend owns — favourites, ratings and
 * the recently-played list — applied back into UI state.
 */

import { setAppSetting } from "../bridge.js";
import { presetSearchElement } from "../presets/dom.js";
import { normalizeRecentPresetIds, setFavoriteToggleState, trackRecentPreset } from "../presets/favorites.js";
import { PRESET_FOLDER_RECENTS_ID } from "../presets/sorting.js";
import { uiState } from "../state.js";
import type { AppSettingValue } from "../types.js";
import { updateUiSettings } from "../windowSettings.js";
import { filterPresets } from "./filter.js";
import { renderPresetUI } from "./library.js";

export const PRESET_RECENTS_SETTING = "presets.recents";

export function applyPresetFavoritesFromBackend(favorites: string[]): void {
  uiState.presetFavorites = new Set(favorites);
  setFavoriteToggleState(uiState.activePresetId);
}

export function applyPresetRecentsFromAppSettings(): void {
  const uiRecents = normalizeRecentPresetIds(uiState.uiSettings?.presetRecents);
  const legacyRecents = normalizeRecentPresetIds(uiState.appSettings?.[PRESET_RECENTS_SETTING]);
  const normalized = uiRecents.length ? uiRecents : legacyRecents;
  uiState.uiSettings = {
    ...(uiState.uiSettings ?? { zoom: 1 }),
    presetRecents: normalized,
  };
  if (!uiRecents.length && legacyRecents.length) {
    updateUiSettings({ presetRecents: normalized });
    uiState.appSettings[PRESET_RECENTS_SETTING] = normalized as unknown as AppSettingValue;
    setAppSetting(PRESET_RECENTS_SETTING, null);
  }
  if (uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID) {
    filterPresets(presetSearchElement?.value ?? "");
    return;
  }
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}

export function recordRecentPreset(presetId: string | null | undefined): void {
  trackRecentPreset(presetId);
  if (uiState.activePresetFolderId === PRESET_FOLDER_RECENTS_ID) {
    filterPresets(presetSearchElement?.value ?? "");
  }
}

export function applyPresetRatingsFromBackend(ratings: Record<string, number>): void {
  uiState.presetRatings = { ...ratings };
  renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
}
