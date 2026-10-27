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

const INSTALLED_ITEM_PREFIX = "tone-sharing-api:item:";

/**
 * The item an installed preset came from, by the install's record in toneSharing.installedPacks:
 * a single item's entry id names it, and a pack's records each preset's item (`presetItems`). The
 * engine installs presets without a toneSharingOrigin, so this is how they are found.
 */
export function installedToneSharingItemId(presetId: string): string | null {
  const list = (uiState.appSettings as Record<string, unknown> | undefined)?.["toneSharing.installedPacks"];
  if (!presetId || !Array.isArray(list)) {
    return null;
  }
  for (const entry of list) {
    if (!entry || typeof entry !== "object") {
      continue;
    }
    const record = entry as Record<string, unknown>;
    const items = record.presetItems;
    if (items && typeof items === "object") {
      const itemId = (items as Record<string, unknown>)[presetId];
      if (typeof itemId === "string" && itemId) {
        return itemId;
      }
    }
    const id = typeof record.id === "string" ? record.id : "";
    if (id.startsWith(INSTALLED_ITEM_PREFIX) && Array.isArray(record.presetIds) && record.presetIds.includes(presetId)) {
      return id.slice(INSTALLED_ITEM_PREFIX.length) || null;
    }
  }
  return null;
}

/** The item a favourite or rating of this preset belongs to on the service, if any. */
function toneSharingItemIdForPreset(preset: Preset | null | undefined): string | null {
  return getPresetToneSharingOrigin(preset)?.itemId ?? (preset?.id ? installedToneSharingItemId(preset.id) : null);
}

export function isToneSharingSignedIn(): boolean {
  return Boolean(toneSharingState.user);
}

export async function syncToneSharingFavoriteForPreset(preset: Preset | null | undefined, favorite: boolean): Promise<void> {
  if (!toneSharingState.user) {
    return;
  }
  const itemId = toneSharingItemIdForPreset(preset);
  if (!itemId) {
    return;
  }
  await apiFetch(`/items/${itemId}/favorite`, {
    method: favorite ? "PUT" : "DELETE",
  });
}

export async function syncToneSharingRatingForPreset(preset: Preset | null | undefined, rating: number | null): Promise<void> {
  if (!toneSharingState.user) {
    return;
  }
  const itemId = toneSharingItemIdForPreset(preset);
  if (!itemId) {
    return;
  }
  if (rating === null) {
    await apiFetch(`/items/${itemId}/rating`, { method: "DELETE" });
    return;
  }
  await apiFetch(`/items/${itemId}/rating`, {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ rating }),
  });
}
