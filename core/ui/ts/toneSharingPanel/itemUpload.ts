/**
 * Uploading a preset as a community item: the asset upload both its archives
 * go through, and the item record that ties them together.
 *
 * Creating an item does not submit it. Until `/items/{id}/publish` is called it
 * stays a draft only its creator sees, so a pack can gather drafts and submit
 * them with itself.
 */

import { clonePreset } from "../state.js";
import type { Preset } from "../types.js";
import { apiFetch, toneSharingFetch } from "./api.js";
import { getToneSharingHostActions } from "./hostActions.js";
import type { ToneSharingItem } from "./types.js";

/** Uploads one blob as an asset of the given kind and returns its id. */
export async function uploadToneSharingAsset(kind: "item_payload" | "thumbnail", blob: Blob): Promise<string> {
  const mimeType = blob.type || "application/octet-stream";
  const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ kind, mimeType, byteSize: blob.size })
  });

  const uploadResponse = await toneSharingFetch(`/uploads/${init.uploadId}`, {
    method: "PUT",
    headers: { "content-type": mimeType },
    body: blob,
  });
  const uploadPayload = await uploadResponse.json().catch(() => null);
  if (!uploadResponse.ok || uploadPayload?.ok === false) {
    throw new Error(uploadPayload?.error?.message || `Upload failed (${uploadResponse.status})`);
  }

  const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ uploadId: init.uploadId })
  });
  return complete.assetId;
}

/** True when the preset has the chain a community item needs: input, output and an effect between. */
export function hasPublishableSignalChain(preset: Preset): boolean {
  const graphNodes = preset.graph?.nodes ?? [];
  const hasInput = graphNodes.some((n) => n.type === "input");
  const hasOutput = graphNodes.some((n) => n.type === "output");
  const hasEffect = graphNodes.some((n) => n.type !== "input" && n.type !== "output");
  return hasInput && hasOutput && hasEffect;
}

/**
 * Uploads the preset's public and private archives and creates an item for them.
 * The item is left a draft; the caller decides whether to submit it.
 */
export async function createToneSharingPresetItem(
  preset: Preset,
  meta: { title: string; description: string; tags?: string[] },
): Promise<ToneSharingItem> {
  const archiveBlobs = await getToneSharingHostActions().buildToneSharingPresetArchiveBlobs(clonePreset(preset));
  const payloadAssetId = await uploadToneSharingAsset("item_payload", archiveBlobs.publicBlob);
  const privatePayloadAssetId = await uploadToneSharingAsset("item_payload", archiveBlobs.privateBlob);

  const result = await apiFetch<{ item: ToneSharingItem }>("/items", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      type: "preset",
      title: meta.title,
      description: meta.description,
      tags: meta.tags && meta.tags.length > 0 ? meta.tags : undefined,
      payloadAssetId,
      privatePayloadAssetId
    })
  });
  return result.item;
}
