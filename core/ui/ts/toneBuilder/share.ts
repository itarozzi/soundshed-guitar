/**
 * Publishing what the Tone Builder made on Tone Sharing. Every result is published, as Soundshed
 * and tagged "tone builder", as soon as its presets are built: there is no choice to make and no
 * consent to ask for, because the presets are Soundshed's, not the requester's.
 *
 * Only the app can build the preset archives, so it uploads them and hands them to the service
 * (POST /tone-builder/jobs/{id}/publish), which publishes them under the Soundshed account with
 * the job's own titles and descriptions: each preset an item, two or more gathered into a pack.
 *
 * The archives name Tone3000 captures rather than carry them (remoteArchiveResources.ts), so
 * publishing passes on only what each capture's licence allows; whoever installs a preset fetches
 * those captures from their provider.
 */

import { apiFetch } from "../toneSharingPanel/api.js";
import { buildPackImageVariants } from "../toneSharingPanel/images.js";
import { uploadPresetArchives, uploadToneSharingAsset } from "../toneSharingPanel/itemUpload.js";
import type { Preset } from "../types.js";

export interface ToneBuilderPublishResult {
  itemIds: string[];
  packId: string | null;
  /** True when the job, or the one whose result it reused, was published before. */
  alreadyPublished: boolean;
}

/**
 * Uploads each built preset's archives and the artwork, then publishes them. `presets` pairs each
 * preset with the proposal key it was built from. Throws when nothing could be published.
 */
export async function publishToneBuilderPresets(
  jobId: string,
  presets: Array<{ key: string; preset: Preset }>,
  onProgress: (message: string) => void,
  /** The pack artwork, used as the pack's (or a lone preset's) thumbnail. */
  artwork: Blob | null = null,
): Promise<ToneBuilderPublishResult> {
  const built: Array<{ key: string; payloadAssetId: string; privatePayloadAssetId: string }> = [];
  for (const [index, { key, preset }] of presets.entries()) {
    onProgress(`Publishing ${index + 1} of ${presets.length}: ${preset.name}`);
    built.push({ key, ...(await uploadPresetArchives(preset)) });
  }

  let thumbnailAssetId: string | null = null;
  if (artwork) {
    try {
      const variants = await buildPackImageVariants(new File([artwork], "artwork.jpg", { type: artwork.type || "image/jpeg" }));
      thumbnailAssetId = await uploadToneSharingAsset("thumbnail", variants.small.blob);
    } catch {
      // Published without a thumbnail rather than not at all.
    }
  }

  onProgress("Publishing on Tone Sharing");
  const result = await apiFetch<{ published: { itemIds: string[]; packId: string | null }; alreadyPublished: boolean }>(
    `/tone-builder/jobs/${encodeURIComponent(jobId)}/publish`,
    {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ presets: built, thumbnailAssetId }),
    },
  );
  return { itemIds: result.published.itemIds, packId: result.published.packId, alreadyPublished: result.alreadyPublished };
}
