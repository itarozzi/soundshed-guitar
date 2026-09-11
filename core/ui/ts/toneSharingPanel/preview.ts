/**
 * Auditioning a community preset over the user's own rig without committing to
 * it: the preset that was loaded first is remembered so it can be put back.
 */

import { postMessage } from "../bridge.js";
import { importPresetArchive } from "../presets.js";
import { uiState } from "../state.js";
import { syncFeedPreviewButton } from "./actionButtons.js";
import { apiFetch, buildApiUrl } from "./api.js";
import { element } from "./dom.js";
import { resolveCreatorProfileHandle } from "./format.js";
import { previewState, toneSharingState } from "./state.js";
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
 * Walk the preset JSON (as returned by readPresetFromArchive) and replace
 * every resource-id reference that appears in idMap with its mapped value.
 * This is needed so previewed presets resolve to resources imported from
 * the tone-sharing archive.
 */
/**
 * For a single-item archive buffer from the tone-sharing API, extract any
 * embedded resource files (NAM models, IR WAVs, etc.) and import them into
 * the local library via importRemoteResource messages.  Resources already
 * present in the library (matched by content hash or id) are skipped.
 *
 * Returns a Map<oldId, newId> suitable for remapping the preset JSON before
 * it is loaded or previewed.
 */
export async function previewPreset(itemId: string, itemTitle: string): Promise<void> {
  // Save original preset ID so we can restore it later (only on first preview)
  if (!previewState.itemId) {
    previewState.priorPresetId = uiState.activePresetId ?? null;
  }

  const response = await fetch(buildApiUrl(`/items/${itemId}/download`), {
    headers: toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {},
    credentials: "include"
  });
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
  const importedPresets = await importPresetArchive(importFile, {
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
