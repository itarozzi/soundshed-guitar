/**
 * Markup for the Tone3000 tab's rows: a tone's one line of metadata, one of its
 * models, and its Details section.
 *
 * Free functions taking everything they draw from, like folderRows.ts: the tab
 * owns the state (what is expanded, what is previewing) and these only draw it.
 */

import { getPlaySvg, getStopSvg } from "../iconAssets.js";
import type { Tone3000Model, Tone3000Tone } from "../tone3000ApiTypes.js";
import { escapeHtml } from "../utils.js";
import { DOUBLE_CLICK_SELECTS_ATTR } from "./helpers.js";

/**
 * Who made the tone, how many models it has and how popular it is. A search is
 * already narrowed to one kind of gear, so the gear only shows for favourites.
 * Empty parts are left out so the stylesheet's separators never double up.
 */
export function renderToneMetaParts(tone: Tone3000Tone, modelCount: number | null | undefined, showGear: boolean): string {
  const username = tone.user?.username ?? "";
  return [
    showGear ? tone.gear ?? "" : "",
    username ? `by ${username}` : "",
    modelCount != null ? `${modelCount} model${modelCount === 1 ? "" : "s"}` : "",
    tone.downloads_count != null ? `${tone.downloads_count.toLocaleString()} downloads` : "",
    tone.platform ?? "",
  ]
    .filter((part) => part.trim())
    .map((part) => `<span>${escapeHtml(part)}</span>`)
    .join("");
}

export interface ToneModelRowState {
  isPreviewing: boolean;
  isLoadingPreview: boolean;
  isImported: boolean;
  /** A preview plays through the node, so a browser opened without one can only select. */
  canPreview: boolean;
}

export function renderToneModelRow(tone: Tone3000Tone, model: Tone3000Model, state: ToneModelRowState): string {
  const { isPreviewing, isLoadingPreview, isImported, canPreview } = state;
  // A loading preview button stays enabled (aria-disabled only): a disabled one
  // swallows the second click of a double click, and with it the dblclick.
  const rowClass = isPreviewing
    ? "resource-browser-model is-previewing"
    : isLoadingPreview
      ? "resource-browser-model is-preview-loading"
      : "resource-browser-model";
  const previewIcon = isPreviewing
    ? getStopSvg()
    : isLoadingPreview
      ? `<span class="resource-browser-preview-spinner" aria-hidden="true"></span>`
      : getPlaySvg();
  const previewTitle = isPreviewing ? "Stop preview" : isLoadingPreview ? "Loading preview…" : "Preview in the effect";
  const toneId = escapeHtml(String(tone.id));
  const modelId = escapeHtml(String(model.id));
  const modelUrl = escapeHtml(model.model_url);

  return `
    <div class="${rowClass}${isImported ? " is-imported" : ""}" data-model-id="${modelId}">
      <span class="resource-browser-model-name">${escapeHtml(model.name)}</span>
      ${isImported ? `<span class="resource-browser-model-imported" title="Already in your library">In library</span>` : ""}
      <div class="resource-browser-model-actions">
        <button class="resource-browser-model-preview" type="button" ${DOUBLE_CLICK_SELECTS_ATTR}
                data-tone-id="${toneId}" data-model-id="${modelId}" data-model-url="${modelUrl}"
                title="${previewTitle}" aria-label="${previewTitle}"
                ${isLoadingPreview ? `aria-disabled="true"` : ""}
                ${canPreview ? "" : "hidden"}>
          ${previewIcon}
        </button>
        <button class="resource-browser-model-select" type="button" ${DOUBLE_CLICK_SELECTS_ATTR}
                data-tone-id="${toneId}" data-model-id="${modelId}" data-model-url="${modelUrl}"
                data-model-name="${escapeHtml(model.name)}"
                title="Import it and use it in the effect">
          Select
        </button>
      </div>
    </div>
  `;
}

export function renderToneDetails(tone: Tone3000Tone): string {
  const description = tone.description?.trim() || "No description provided.";
  const tags = Array.isArray(tone.tags)
    ? tone.tags.map((tag) => tag?.name?.trim()).filter((name): name is string => Boolean(name))
    : [];
  const infoRows = [
    ["Gear", tone.gear ?? "Unknown"],
    ["Platform", tone.platform ?? "Unknown"],
    ["Models", String(tone.models_count ?? 0)],
    ["Downloads", (tone.downloads_count ?? 0).toLocaleString()],
    ["Author", tone.user?.username ?? "Unknown"],
  ];

  return `
    <div class="resource-browser-tone-details-panel">
      <div class="resource-browser-tone-metadata">
        ${infoRows.map(([label, value]) => `
          <span class="resource-browser-tone-metadata-badge">
            <span class="resource-browser-tone-metadata-label">${escapeHtml(label)}</span>
            <span class="resource-browser-tone-metadata-value">${escapeHtml(value)}</span>
          </span>
        `).join("")}
      </div>
      <div class="resource-browser-tone-details-description">${escapeHtml(description)}</div>
      <div class="resource-browser-tone-details-tags">
        ${tags.length
          ? tags.map((tag) => `<span class="resource-browser-tone-details-tag">${escapeHtml(tag)}</span>`).join("")
          : `<span class="resource-browser-tone-details-tag is-empty">No tags</span>`}
      </div>
    </div>
  `;
}
