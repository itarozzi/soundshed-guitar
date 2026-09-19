/**
 * Tone sharing panel — the community browse/publish surface.
 *
 * This file is the entry point and the public face of the feature: it owns
 * initialisation, the app-settings hook, and the small set of symbols the rest
 * of the UI imports. Everything else lives in ./toneSharingPanel/, grouped by
 * what it does rather than by when it runs.
 *
 * The modules under that directory call each other in both directions — the
 * browse dispatcher renders every sub-view, and several sub-views ask for a
 * browse refresh once they have changed something. That is the shape of the
 * feature, not an accident of the split, so the imports are left direct rather
 * than routed through a registry. It is safe because every participant is a
 * hoisted `function` declaration and nothing in the cycle runs at import time;
 * scripts/check-cycles.js collapses a feature's own modules for this reason.
 */

import { FEATURE_FLAGS_CHANGED_EVENT, Features } from "./featureFlags.js";
import { loadAuthSession } from "./toneSharingPanel/account.js";
import { normalizeSettingString } from "./toneSharingPanel/api.js";
import { bindBrowseActions, bindBrowseModeButtons } from "./toneSharingPanel/bindings.js";
import { loadBrowse, loadMine } from "./toneSharingPanel/browse.js";
import { element } from "./toneSharingPanel/dom.js";
import { clearPackThumbnailObjectUrls } from "./toneSharingPanel/images.js";
import { normalizeInstalledPackMetadata, reconcileInstalledPacksWithPresetLibrary } from "./toneSharingPanel/installedPacks.js";
import { restoreLocalState } from "./toneSharingPanel/localState.js";
import { openSharedTargetFromLocation } from "./toneSharingPanel/shareLinks.js";
import { browseState, storageKeys, toneSharingState } from "./toneSharingPanel/state.js";
import { bindTopControls } from "./toneSharingPanel/topBindings.js";
import type { InstalledPackMetadata } from "./toneSharingPanel/types.js";

export { openToneSharingSignInModal } from "./toneSharingPanel/account.js";
export { registerInstalledToneSharingPack } from "./toneSharingPanel/installedPacks.js";
export { isToneSharingSignedIn, syncToneSharingFavoriteForPreset, syncToneSharingRatingForPreset } from "./toneSharingPanel/presetLinks.js";
export { openToneSharingPublishPresetModal } from "./toneSharingPanel/publish.js";
export { handleToneSharingDeepLink } from "./toneSharingPanel/shareLinks.js";
export type { InstalledPackMetadata } from "./toneSharingPanel/types.js";

export function applyToneSharingAppSettings(settings?: Record<string, unknown>): void {
  if (!settings || !element("panel-sharing")) {
    return;
  }

  let changed = false;
  const persistedSession = normalizeSettingString(settings[storageKeys.sessionId]);
  const persistedInstalled = settings[storageKeys.installedPacks];

  if (persistedSession !== toneSharingState.sessionId) {
    toneSharingState.sessionId = persistedSession;
    changed = true;
  }

  if (Array.isArray(persistedInstalled)) {
    const normalizedInstalled = persistedInstalled
      .map((entry) => normalizeInstalledPackMetadata(entry))
      .filter((entry): entry is InstalledPackMetadata => entry !== null);
    const currentSerialized = JSON.stringify(toneSharingState.installedPacks);
    const nextSerialized = JSON.stringify(normalizedInstalled);
    if (currentSerialized !== nextSerialized) {
      toneSharingState.installedPacks = normalizedInstalled;
      changed = true;
    }
  }

  if (reconcileInstalledPacksWithPresetLibrary()) {
    changed = true;
  }

  if (!changed) {
    return;
  }

  // Applying persisted settings must not hit the Tone Sharing API while the Tones tab
  // is inactive. If the panel has never been opened, just keep the applied state
  // (sessionId/installedPacks); the network loads run when the tab is first activated
  // via initializeToneSharingPanel(). Only refresh here when the panel is already live.
  if (!toneSharingPanelInitialized) {
    return;
  }

  clearPackThumbnailObjectUrls();
  void (async () => {
    await loadAuthSession();
    await Promise.all([loadBrowse(), loadMine()]);
    await openSharedTargetFromLocation();
  })();
}

let toneSharingPanelInitialized = false;

export function initializeToneSharingPanel(): void {
  if (!element("panel-sharing")) {
    return;
  }

  if (toneSharingPanelInitialized) {
    return;
  }
  toneSharingPanelInitialized = true;

  restoreLocalState();
  clearPackThumbnailObjectUrls();
  bindTopControls();
  bindBrowseModeButtons();
  bindBrowseActions();
  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, (event) => {
    const detail = (event as CustomEvent<{ featureId?: string }>).detail;
    if (detail?.featureId !== Features.AiToneSearch) {
      return;
    }
    if (browseState.mode === "mine") {
      void loadMine();
      return;
    }
    void loadBrowse();
  });

  void (async () => {
    await loadAuthSession();
    await Promise.all([loadBrowse(), loadMine()]);
    await openSharedTargetFromLocation();
  })();
}
