/**
 * Auditioning a community preset over the user's own rig without committing to
 * it: the preset that was loaded first is remembered so it can be put back.
 */

import { postMessage } from "../bridge.js";
import type { IncomingPayload } from "../messages/types.js";
import { showNotification } from "../notifications.js";
import type { ArchivePreview } from "../presets/archiveTypes.js";
import { uiState } from "../state.js";
import { syncFeedPreviewButton } from "./actionButtons.js";
import { toneSharingFetch } from "./api.js";
import { element } from "./dom.js";
import { getToneSharingHostActions } from "./hostActions.js";
import { readDownload } from "./install.js";
import { previewState } from "./state.js";

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

/**
 * Bumped by every preview and clear: a preview still loading when the next request comes
 * drops out at its next await, so it cannot apply itself over a clear or a later preview.
 */
let previewRequestId = 0;

export async function clearPreviewPreset(): Promise<void> {
  previewRequestId += 1;
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
 * The engine reads the download and plays it ("previewPresetArchive"), writing nothing: no preset,
 * blend or resource. Its own models and IRs play from the library where it holds the same bytes,
 * else from a temp folder of the preview's own. The Tone3000 models it names but does not carry
 * are fetched here first, when the library lacks them, by their Tone3000 tone and model ids: the
 * archive's own model URL is never followed, so it cannot choose where the Tone3000 token goes.
 * One that cannot be had leaves its slot empty for the preview.
 */
export async function previewPreset(itemId: string, itemTitle: string): Promise<void> {
  const requestId = ++previewRequestId;
  // Save original preset ID so we can restore it later (only on first preview)
  if (!previewState.itemId) {
    previewState.priorPresetId = uiState.activePresetId ?? null;
  }

  const response = await toneSharingFetch(`/items/${itemId}/download`);
  if (requestId !== previewRequestId) {
    return;
  }
  if (!response.ok) {
    throw new Error(`Preview download failed (${response.status})`);
  }

  const archive = await readDownload({ title: itemTitle, data: await response.arrayBuffer(), itemId });
  if (requestId !== previewRequestId) {
    return;
  }

  const preview: ArchivePreview = { resources: [], unavailable: [] };
  await getToneSharingHostActions().prepareArchiveRemoteResourcesForPreview(archive.remoteRefs, new Map(), preview);
  if (requestId !== previewRequestId) {
    return;
  }

  postMessage({
    type: "previewPresetArchive",
    requestId: previewMessageId(requestId),
    itemId,
    presetId: `preview-${itemId}`,
    title: itemTitle,
    data: archive.base64,
    fetchedResources: preview.resources,
  });
  if (preview.unavailable.length > 0) {
    showNotification(
      "Preview is missing models",
      `${preview.unavailable.join(", ")} could not be loaded, so the preview plays without ${preview.unavailable.length === 1 ? "it" : "them"}. Installing the tone fetches them.`,
    );
  }

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

function previewMessageId(requestId: number): string {
  return `tone-preview-${requestId}`;
}

/** The engine played the preview ("presetArchivePreviewed"): its presetLoaded has shown it already. */
export function onPresetArchivePreviewed(_payload: IncomingPayload): void {
  // Nothing more to do: the indicator went up with the request, and the engine's presetLoaded
  // brought the preset in. The models it plays without were named when they could not be fetched.
}

/**
 * The engine could not read the download ("presetArchivePreviewFailed"; its error is shown as a
 * notification already). The indicator comes down; nothing was loaded, so nothing is put back.
 */
export function onPresetArchivePreviewFailed(payload: IncomingPayload): void {
  if (payload.requestId !== previewMessageId(previewRequestId)) {
    return;
  }
  const feedEl = element<HTMLElement>("tone-sharing-feed");
  if (previewState.itemId) {
    const card = feedEl?.querySelector(`.tone-sharing-card-item[data-id="${CSS.escape(previewState.itemId)}"]`) as HTMLElement | null;
    if (card) {
      syncFeedPreviewButton(card, false);
    }
  }
  previewState.itemId = null;
  previewState.priorPresetId = null;
  hidePreviewIndicator();
}
