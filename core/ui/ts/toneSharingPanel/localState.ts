/**
 * Restoring what the panel persisted locally — the session id and the installed
 * pack list — before anything renders.
 */

import { uiState } from "../state.js";
import { normalizeSettingString } from "./api.js";
import { normalizeInstalledPackMetadata, persistInstalledPacks, reconcileInstalledPacksWithPresetLibrary } from "./installedPacks.js";
import { storageKeys, toneSharingState } from "./state.js";
import type { InstalledPackMetadata } from "./types.js";

export function restoreLocalState(): void {
  const appSettings = uiState.appSettings ?? {};
  const persistedSession = normalizeSettingString(appSettings[storageKeys.sessionId]);
  const persistedInstalled = appSettings[storageKeys.installedPacks];
  if (persistedSession) {
    toneSharingState.sessionId = persistedSession;
  }

  const parseInstalled = (value: unknown): InstalledPackMetadata[] => {
    if (!Array.isArray(value)) {
      return [];
    }
    return value
      .map((entry) => normalizeInstalledPackMetadata(entry))
      .filter((entry): entry is InstalledPackMetadata => entry !== null);
  };

  if (Array.isArray(persistedInstalled)) {
    toneSharingState.installedPacks = parseInstalled(persistedInstalled);
  } else {
    toneSharingState.installedPacks = [];
  }
  if (reconcileInstalledPacksWithPresetLibrary()) {
    persistInstalledPacks();
  }

}
