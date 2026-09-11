/**
 * Everything the host says about presets: loads, saves, the library index,
 * folders, favourites, ratings, setlists and archive sessions.
 */

import { syncControlsFromState } from "../controls.js";
import { appendLog } from "../logging.js";
import { reconcileActiveCompositePreset } from "../multiPresetMixer.js";
import { showNotification } from "../notifications.js";
import { refreshPerformancePads } from "../performancePads.js";
import { applyPresetArchiveSessionState, applyPresetFavoritesFromBackend, applyPresetFoldersFromBackend, applyPresetRatingsFromBackend, applySetlistCursorFromBackend, applySetlistsFromBackend, cachePresetInMemory, handlePresetDataMessage, populatePresetDropdown, recordRecentPreset, refreshPresetCacheEntryFromBackend, renderActivePreset, updatePresetActionButtons, updatePresetDropdownSelection } from "../presets.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { migratePresetNodeTypes } from "../presetV2.js";
import { refreshEffectPresetsFlyout, refreshSelectedNodeParams } from "../signalPath.js";
import { clonePreset, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import type { Preset, PresetFolder, Setlist, StoredEffectPreset } from "../types.js";
import { markIgnoreNextStatePreset } from "./echoGuard.js";
import { normalizePresetResources } from "./normalize.js";
import { takePendingSharedPresetHydration } from "./sharedSync.js";
import type { IncomingPayload } from "./types.js";

export function onPresetLoaded(payload: IncomingPayload): void {
  const preset = (payload as { preset?: Preset }).preset;
  if (preset) {
    // Clear loading state before re-rendering — the re-render below removes
    // all loading classes and overlays baked into the DOM by the render functions.
    uiState.presetLoadingId = null;
    migratePresetNodeTypes(preset);
    normalizePresetResources(preset);
    const preserveNewDraft = Boolean(uiState.activePresetIsNew && uiState.activePresetId === preset.id);
    uiState.activePresetSceneId = normalizePresetScenes(preset, (payload as { sceneId?: string }).sceneId ?? uiState.activePresetSceneId ?? undefined);
    recordRecentPreset(preset.id);
    uiState.activePresetId = preset.id;
    setActivePresetIsNew(preserveNewDraft);
    uiState.presetCache.set(preset.id, clonePreset(preset));
    setActivePresetSnapshot(preset);
    setActivePresetDraft(preset);
    setPresetDirty(false);
    updatePresetDropdownSelection();
  }
  const activePresetIds = (payload as { activePresetIds?: string[] }).activePresetIds;
  if (Array.isArray(activePresetIds)) {
    uiState.mixer = uiState.mixer ?? { activePresetIds: [], presets: {}, masterGain: 1.0, mixGainDb: 0 };
    uiState.mixer.activePresetIds = activePresetIds.slice();
    activePresetIds.forEach((id) => {
      if (!uiState.mixer!.presets[id]) {
        uiState.mixer!.presets[id] = { id, mix: 1.0, pan: 0.0, mute: false, solo: false };
      }
    });
    reconcileActiveCompositePreset(); // a setlist step or program change reports the new mixer here, not via "state"
  }
  const parameters = (payload as { parameters?: Record<string, unknown> }).parameters;
  if (parameters) {
    uiState.parameters = {
      values: Array.isArray((parameters as { parameters?: unknown }).parameters)
        ? ((parameters as { parameters: [] }).parameters as [])
        : uiState.parameters.values,
    };
  }
  if (preset) {
    uiState.presetCache.set(preset.id, clonePreset(preset));
    setActivePresetSnapshot(preset);
    setActivePresetDraft(preset);
    setPresetDirty(false);
  }
  renderActivePreset();
  refreshPerformancePads();
  syncControlsFromState();
  updatePresetActionButtons();
}

export function onPresetExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Preset exported", info.path ?? "");
}

export function onPresetExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Preset export failed", info.message ?? "");
}

export function onPresetSaved(payload: IncomingPayload): void {
  const savedPreset = (payload as { preset?: Preset }).preset;
  appendLog(
    `preset saved ← ${savedPreset?.name ?? "unknown"} `
    + `(graphNodes=${savedPreset?.graph?.nodes?.length ?? 0}, scenes=${savedPreset?.scenes?.length ?? 0})`,
  );
  if (savedPreset) {
    normalizePresetResources(savedPreset);
    uiState.activePresetSceneId = normalizePresetScenes(savedPreset, (payload as { sceneId?: string }).sceneId ?? uiState.activePresetSceneId ?? undefined);
    cachePresetInMemory(savedPreset);
    uiState.activePresetId = savedPreset.id;
    setActivePresetIsNew(false);
    uiState.presetCache.set(savedPreset.id, clonePreset(savedPreset));
    setActivePresetSnapshot(savedPreset);
    setActivePresetDraft(savedPreset);
    setPresetDirty(false);
    if (!uiState.presets.some((p) => p.id === savedPreset.id)) {
      uiState.presets.unshift(clonePreset(savedPreset));
      uiState.filteredPresets = uiState.presets.slice();
      populatePresetDropdown();
    }
    renderActivePreset();
    updatePresetDropdownSelection();
    markIgnoreNextStatePreset(savedPreset.id);
  }
  showNotification("Preset saved", (payload as { path?: string }).path ?? savedPreset?.name ?? "");
}

export function onPresetArchiveSessionStarted(payload: IncomingPayload): void {
  const sessionPayload = payload as { active?: boolean; archiveName?: string; archiveKey?: string; presetCount?: number };
  applyPresetArchiveSessionState({
    active: Boolean(sessionPayload.active),
    archiveName: sessionPayload.archiveName,
    archiveKey: sessionPayload.archiveKey,
    presetCount: sessionPayload.presetCount,
  });
  renderActivePreset();
  updatePresetActionButtons();
  showNotification("Preset archive session started", sessionPayload.archiveName ?? "");
}

export function onPresetArchiveSessionEnded(): void {
  applyPresetArchiveSessionState({ active: false });
  renderActivePreset();
  updatePresetActionButtons();
  showNotification("Preset archive session ended");
}

export function onPresetArchiveSessionFailed(payload: IncomingPayload): void {
  showNotification("Preset archive session failed", (payload as { message?: string }).message ?? "");
}

export function onPresetList(payload: IncomingPayload): void {
  const presetListPayload = payload as { presets?: Array<{ id: string; name: string; category?: string; source?: string }> };
  if (Array.isArray(presetListPayload.presets)) {
    appendLog(`preset list received ← ${presetListPayload.presets.length} presets`);
    const cachedBeforeUpdate = new Set(uiState.presetCache.keys());
    const nextPresets: Preset[] = [];
    for (const p of presetListPayload.presets) {
      const incomingCategory = p.category ?? "Factory";
      const existingCached = uiState.presetCache.get(p.id);
      const nextPreset: Preset = existingCached
        ? {
            ...existingCached,
            name: p.name,
            category: incomingCategory,
          }
        : { id: p.id, name: p.name, category: incomingCategory } as Preset;
      uiState.presetCache.set(p.id, nextPreset);
      nextPresets.push(nextPreset);
    }
    uiState.presets = nextPresets;
    uiState.filteredPresets = nextPresets.slice();
    populatePresetDropdown();
    renderActivePreset();

    if (takePendingSharedPresetHydration()) {
      presetListPayload.presets.forEach((presetSummary) => {
        if (cachedBeforeUpdate.has(presetSummary.id)) {
          refreshPresetCacheEntryFromBackend(presetSummary.id);
        }
      });
    }
  }
}

export function onPresetData(payload: IncomingPayload): void {
  const presetPayload = payload as { preset?: Preset };
  if (presetPayload.preset) {
    migratePresetNodeTypes(presetPayload.preset);
    normalizePresetResources(presetPayload.preset);
    normalizePresetScenes(presetPayload.preset);
    handlePresetDataMessage(presetPayload.preset, (payload as { requestId?: string }).requestId);
  }
}

export function onPresetFolders(payload: IncomingPayload): void {
  const foldersPayload = payload as { folders?: PresetFolder[]; activeFolderId?: string | null };
  applyPresetFoldersFromBackend(foldersPayload.folders ?? [], foldersPayload.activeFolderId ?? null);
}

export function onPresetFavorites(payload: IncomingPayload): void {
  const favoritesPayload = payload as { favorites?: string[] };
  applyPresetFavoritesFromBackend(Array.isArray(favoritesPayload.favorites) ? favoritesPayload.favorites : []);
}

export function onPresetRatings(payload: IncomingPayload): void {
  const ratingsPayload = payload as { ratings?: Record<string, number> };
  applyPresetRatingsFromBackend(ratingsPayload.ratings ?? {});
}

export function onSetlists(payload: IncomingPayload): void {
  const setlistsPayload = payload as { setlists?: Setlist[]; activeSetlistId?: string | null };
  applySetlistsFromBackend(setlistsPayload.setlists ?? [], setlistsPayload.activeSetlistId ?? null);
  refreshPerformancePads();
}

export function onEffectPresets(payload: IncomingPayload): void {
  const effectPresetsPayload = payload as { byEffectType?: Record<string, StoredEffectPreset[]> };
  uiState.effectPresets = effectPresetsPayload.byEffectType ?? {};
  // The backend re-broadcasts after each save/delete, so both the params panel
  // and an open presets flyout need to pick up the new list.
  refreshSelectedNodeParams();
  refreshEffectPresetsFlyout();
}

export function onSetlistCursorChanged(payload: IncomingPayload): void {
  const cursorPayload = payload as { cursorIndex?: number; presetId?: string; activeSetlistId?: string };
  if (typeof cursorPayload.cursorIndex === "number") {
    applySetlistCursorFromBackend(cursorPayload.cursorIndex, cursorPayload.presetId, cursorPayload.activeSetlistId);
    refreshPerformancePads();
  }
}
