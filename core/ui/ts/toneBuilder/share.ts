/**
 * Sharing what the Tone Builder made on Tone Sharing, under the requester's own account: each
 * preset becomes a community item submitted for moderation, and two or more are gathered into a
 * pack submitted with them.
 *
 * The archives name Tone3000 captures rather than carry them (remoteArchiveResources.ts), so
 * sharing passes on only what each capture's licence allows; whoever installs a preset fetches
 * those captures from their provider.
 */

import { apiFetch } from "../toneSharingPanel/api.js";
import { ensurePublishConsent } from "../toneSharingPanel/consent.js";
import { buildPackImageVariants } from "../toneSharingPanel/images.js";
import { createToneSharingPresetItem, uploadToneSharingAsset } from "../toneSharingPanel/itemUpload.js";
import type { Preset } from "../types.js";

export interface ToneBuilderShareResult {
  itemIds: string[];
  packId: string | null;
  /** Presets that could not be shared, with why. */
  failed: Array<{ name: string; message: string }>;
}

/** Asks for (or confirms) the one-time publishing consent; throws when the user declines. */
export async function confirmSharingConsent(): Promise<void> {
  await ensurePublishConsent();
}

/**
 * Publishes the presets as items and, for two or more, a pack named `packTitle`. A preset that
 * fails is reported and the rest carry on; the pack holds the ones that made it.
 */
export async function sharePresets(
  presets: Preset[],
  packTitle: string,
  packDescription: string,
  onProgress: (message: string) => void,
  /** The pack's artwork, used as its thumbnail. */
  artwork: Blob | null = null,
): Promise<ToneBuilderShareResult> {
  const itemIds: string[] = [];
  const failed: ToneBuilderShareResult["failed"] = [];
  for (const [index, preset] of presets.entries()) {
    onProgress(`Sharing ${index + 1} of ${presets.length}: ${preset.name}`);
    try {
      const item = await createToneSharingPresetItem(preset, { title: preset.name, description: preset.description ?? "" });
      await apiFetch(`/items/${item.id}/publish`, { method: "POST" });
      itemIds.push(item.id);
    } catch (error) {
      failed.push({ name: preset.name, message: (error as Error).message });
    }
  }

  let packId: string | null = null;
  if (itemIds.length >= 2) {
    onProgress("Sharing the pack");
    let thumbnailAssetId: string | undefined;
    if (artwork) {
      try {
        const variants = await buildPackImageVariants(new File([artwork], "artwork.jpg", { type: artwork.type || "image/jpeg" }));
        thumbnailAssetId = await uploadToneSharingAsset("thumbnail", variants.small.blob);
      } catch (error) {
        failed.push({ name: "Pack artwork", message: (error as Error).message });
      }
    }
    try {
      const created = await apiFetch<{ pack: { id: string } }>("/packs", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title: packTitle, description: packDescription, ...(thumbnailAssetId ? { thumbnailAssetId } : {}) }),
      });
      packId = created.pack.id;
      await apiFetch(`/packs/${packId}/items`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ itemIds }),
      });
      await apiFetch(`/packs/${packId}/publish`, { method: "POST" });
    } catch (error) {
      failed.push({ name: `Pack "${packTitle}"`, message: (error as Error).message });
    }
  }
  return { itemIds, packId, failed };
}
