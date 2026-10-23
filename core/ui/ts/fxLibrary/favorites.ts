/**
 * The FX library's favourites and recently used effects, which its Favourites and Recent
 * tabs list. Both are lists of item keys kept in the UI settings ("fxFavorites",
 * "fxRecents"), which the engine stores with the app settings and shares between
 * instances. A key whose blend or custom effect has since been deleted is kept but not
 * shown; it comes back if that id does.
 */

import { EffectTypeRegistry } from "../presetV2.js";
import { adoptEngineUiSettings, getCurrentUiSettings, updateUiSettings } from "../windowSettings.js";
import type { FxItemIdentity } from "./catalog.js";

/** How many recently used effects the Recent tab keeps. */
export const FX_RECENTS_LIMIT = 12;

/** Fired on window when either list changes, so an open library redraws. */
export const FX_MARKS_CHANGED_EVENT = "fxLibraryMarksChanged";

export function fxItemKey(identity: FxItemIdentity): string {
  return [EffectTypeRegistry.resolve(identity.effectType), identity.blendId ?? "", identity.customEffectId ?? ""].join("|");
}

/** Strings only, each once, in order; anything else in a stored list is dropped. */
export function normalizeFxKeys(value: unknown, limit = Number.POSITIVE_INFINITY): string[] {
  if (!Array.isArray(value)) {
    return [];
  }
  const keys = [...new Set(value.filter((entry): entry is string => typeof entry === "string" && entry.length > 0))];
  return keys.slice(0, limit);
}

export function getFxFavoriteKeys(): string[] {
  return normalizeFxKeys(getCurrentUiSettings().fxFavorites);
}

/** Most recent first. */
export function getFxRecentKeys(): string[] {
  return normalizeFxKeys(getCurrentUiSettings().fxRecents, FX_RECENTS_LIMIT);
}

export function isFxFavorite(identity: FxItemIdentity): boolean {
  return getFxFavoriteKeys().includes(fxItemKey(identity));
}

/** The lists as last announced: settings applied for some other reason (zoom) redraw nothing. */
let announced = "";

function marksSignature(): string {
  return JSON.stringify([getFxFavoriteKeys(), getFxRecentKeys()]);
}

function announceChange(): void {
  const signature = marksSignature();
  if (signature === announced) {
    return;
  }
  announced = signature;
  window.dispatchEvent(new CustomEvent(FX_MARKS_CHANGED_EVENT));
}

// The engine's UI settings arrive after the library first renders.
window.addEventListener("uiSettingsApplied", announceChange);

/** Stars or unstars an effect; a new favourite goes last. Returns whether it is now a favourite. */
export function toggleFxFavorite(identity: FxItemIdentity): boolean {
  const key = fxItemKey(identity);
  const favorites = getFxFavoriteKeys();
  const favorite = !favorites.includes(key);
  updateUiSettings({ fxFavorites: favorite ? [...favorites, key] : favorites.filter((entry) => entry !== key) });
  announceChange();
  return favorite;
}

/** Records an effect put in the chain: it goes first, and the list keeps FX_RECENTS_LIMIT. */
export function recordFxRecent(identity: FxItemIdentity): void {
  const key = fxItemKey(identity);
  const recents = getFxRecentKeys();
  if (recents[0] === key) {
    return;
  }
  updateUiSettings({ fxRecents: [key, ...recents.filter((entry) => entry !== key)].slice(0, FX_RECENTS_LIMIT) });
  announceChange();
}

/**
 * Another instance changed the lists (shared sync). They go into the blob this UI sends
 * back, since uiSettingsChanged replaces the arrays whole, and an open library redraws.
 */
export function adoptFxMarksFromEngine(fxFavorites: unknown, fxRecents: unknown): void {
  const favorites = normalizeFxKeys(fxFavorites);
  const recents = normalizeFxKeys(fxRecents, FX_RECENTS_LIMIT);
  // Adopted, not updated: what the engine sent must not be echoed straight back.
  adoptEngineUiSettings({ fxFavorites: favorites, fxRecents: recents });
  announceChange();
}
