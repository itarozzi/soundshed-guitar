/**
 * Auditioning a community preset over the user's own rig without committing to
 * it: the preset that was loaded first is remembered so it can be put back.
 */

import { postMessage } from "../bridge.js";
import { uiState } from "../state.js";
import { syncFeedPreviewButton } from "./actionButtons.js";
import { apiFetch, toneSharingFetch } from "./api.js";
import { element } from "./dom.js";
import { resolveCreatorProfileHandle } from "./format.js";
import { getToneSharingHostActions } from "./hostActions.js";
import { previewState } from "./state.js";
import type { ToneSharingItem } from "./types.js";

export function showPreviewIndicator(title: string): void {
  const selector = element<HTMLElement>("preset-selector");
  const indicator = element<HTMLElement>("tone-sharing-preview-indicator");
  const indicatorText = element<HTMLElement>("tone-sharing-preview-text");
  if (selector) selector.style.display = "none";
  if (indicator) indicator.style.display = "flex";
  if (indicatorText) indicatorText.textContent = `Previewing preset: ${title}`;
}

export function hidePreviewIndicator(): void {
  const selector = element<HTMLElement>("preset-selector");
  const indicator = element<HTMLElement>("tone-sharing-preview-indicator");
  if (selector) selector.style.display = "";
  if (indicator) indicator.style.display = "none";
}

export async function clearPreviewPreset(): Promise<void> {
  const feedEl = element<HTMLElement>("tone-sharing-feed");
  if (previewState.itemId) {
    const card = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(previewState.itemId)}"]`) as HTMLElement | null;
    if (card) {
      syncFeedPreviewButton(card, false);
    }
  }
  previewState.itemId = null;
  hidePreviewIndicator();
  if (previewState.priorPresetId) {
    const priorPreset = uiState.presetCache.get(previewState.priorPresetId);
    if (priorPreset) {
      postMessage({ type: "loadPreset", preset: priorPreset, presetId: priorPreset.id });
    }
    previewState.priorPresetId = null;
  }
}

/**
 * Loads a Tone Sharing item into the engine to audition it, without saving it.
 *
 * The archive is read with `previewOnly`, which writes nothing: no preset, no
 * blend, no resource. Its models and IRs are played only when the library
 * already has them (matched by content hash, or by id for Tone3000 models);
 * anything else stays missing until the item is installed. A preview used to
 * import the archive's files and fetch its Tone3000 URLs, which let a shared
 * archive both write to the library and choose where the Tone3000 token went.
 */
export async function previewPreset(itemId: string, itemTitle: string): Promise<void> {
  // Save original preset ID so we can restore it later (only on first preview)
  if (!previewState.itemId) {
    previewState.priorPresetId = uiState.activePresetId ?? null;
  }

  const response = await toneSharingFetch(`/items/${itemId}/download`);
  if (!response.ok) {
    throw new Error(`Preview download failed (${response.status})`);
  }

  const disposition = response.headers.get("content-disposition") || "";
  const fileMatch = disposition.match(/filename="?([^"]+)"?/i);
  const fileName = fileMatch?.[1] || `item-${itemId}.soundshed.preset`;
  const blob = await response.blob();

  let itemMeta: ToneSharingItem | null = null;
  try {
    const itemResponse = await apiFetch<{ item: ToneSharingItem }>(`/items/${itemId}`);
    itemMeta = itemResponse.item;
  } catch {
    itemMeta = null;
  }

  const importFile = new File([blob], fileName, { type: blob.type || "application/octet-stream" });
  const importedPresets = await getToneSharingHostActions().importPresetArchive(importFile, {
    source: "toneSharingApi",
    itemId,
    creatorId: itemMeta?.creatorUserId ?? undefined,
    creatorHandle: resolveCreatorProfileHandle((itemMeta ?? {}) as unknown as Record<string, unknown>) ?? undefined,
    titleHint: fileName.replace(/\.(soundshed\.preset|soundshed\.presets|preset|zip)$/i, ""),
  }, {
    previewOnly: true,
    suppressNotifications: true,
  });

  const preset = importedPresets[importedPresets.length - 1];
  if (!preset) {
    throw new Error("Preview import produced no preset");
  }

  postMessage({ type: "loadPreset", preset, presetId: `preview-${itemId}` });

  // Update DOM to reflect new preview state without a full re-fetch
  const feedEl = element<HTMLElement>("tone-sharing-feed");
  if (previewState.itemId && previewState.itemId !== itemId) {
    const prevCard = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(previewState.itemId)}"]`) as HTMLElement | null;
    if (prevCard) {
      syncFeedPreviewButton(prevCard, false);
    }
  }

  previewState.itemId = itemId;

  const newCard = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(itemId)}"]`) as HTMLElement | null;
  if (newCard) {
    syncFeedPreviewButton(newCard, true);
  }

  showPreviewIndicator(itemTitle);
}
