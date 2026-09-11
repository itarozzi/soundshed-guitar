/**
 * What ties a preset in the local library back to the community item it came
 * from — and the favourite/rating writes that follow that link back to the API.
 */

import { uiState } from "../state.js";
import type { Preset } from "../types.js";
import { apiFetch } from "./api.js";
import { toneSharingState } from "./state.js";
import type { InstalledToneSharingLookup } from "./types.js";

export function getPresetToneSharingOrigin(preset: Preset | null | undefined): { itemId: string; republishBlocked: boolean } | null {
  if (!preset || !preset.toneSharingOrigin || typeof preset.toneSharingOrigin !== "object") {
    return null;
  }
  const origin = preset.toneSharingOrigin;
  if (origin.source !== "toneSharingApi") {
    return null;
  }
  const itemId = origin.itemId.trim();
  if (!itemId) {
    return null;
  }
  return {
    itemId,
    republishBlocked: origin.republishBlocked !== false,
  };
}

export function getPresetToneSharingPackId(preset: Preset | null | undefined): string | null {
  if (!preset || !preset.toneSharingOrigin || typeof preset.toneSharingOrigin !== "object") {
    return null;
  }
  const origin = preset.toneSharingOrigin;
  if (origin.source !== "toneSharingApi") {
    return null;
  }
  const packId = typeof origin.importedFromPackId === "string"
    ? origin.importedFromPackId.trim()
    : "";
  return packId || null;
}

export function buildInstalledToneSharingLookup(): InstalledToneSharingLookup {
  const itemIds = new Set<string>();
  const packIds = new Set<string>();

  for (const pack of toneSharingState.installedPacks) {
    const packId = typeof pack.packId === "string" ? pack.packId.trim() : "";
    if (packId) {
      packIds.add(packId);
    }

    if (pack.source === "toneSharingApi" && pack.id.startsWith("tone-sharing-api:item:")) {
      const itemId = pack.id.slice("tone-sharing-api:item:".length).trim();
      if (itemId) {
        itemIds.add(itemId);
      }
    }
  }

  for (const preset of uiState.presets) {
    const origin = getPresetToneSharingOrigin(preset);
    if (origin) {
      itemIds.add(origin.itemId);
    }
    const importedFromPackId = getPresetToneSharingPackId(preset);
    if (importedFromPackId) {
      packIds.add(importedFromPackId);
    }
  }

  return { itemIds, packIds };
}

export function isToneSharingSignedIn(): boolean {
  return Boolean(toneSharingState.user);
}

export async function syncToneSharingFavoriteForPreset(preset: Preset | null | undefined, favorite: boolean): Promise<void> {
  if (!toneSharingState.user) {
    return;
  }
  const origin = getPresetToneSharingOrigin(preset);
  if (!origin?.itemId) {
    return;
  }
  await apiFetch(`/items/${origin.itemId}/favorite`, {
    method: favorite ? "PUT" : "DELETE",
  });
}

export async function syncToneSharingRatingForPreset(preset: Preset | null | undefined, rating: number | null): Promise<void> {
  if (!toneSharingState.user) {
    return;
  }
  const origin = getPresetToneSharingOrigin(preset);
  if (!origin?.itemId) {
    return;
  }
  if (rating === null) {
    await apiFetch(`/items/${origin.itemId}/rating`, { method: "DELETE" });
    return;
  }
  await apiFetch(`/items/${origin.itemId}/rating`, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ rating }),
  });
}
