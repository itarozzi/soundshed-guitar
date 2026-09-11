/**
 * Markup for one file in the Folder tab, and the tag-filter bar above the list.
 *
 * These are free functions taking everything they draw from, rather than
 * methods on the tab: the virtualised list rebuilds a row on every scroll, and
 * a builder that cannot reach tab state is one that cannot quietly depend on
 * when it is called.
 */

import { uiState } from "../state.js";
import { escapeHtml, findResourceById } from "../utils.js";
import { getPlaySvg } from "../iconAssets.js";
import { formatBytes, getResourceTags, normalizeNamArchitectureBadge, splitTagValues } from "./helpers.js";
import type { FolderListingFile, ResourceType } from "./types.js";

/** What a row needs to know about the tab around it. */
export interface FolderRowState {
  /** The type the modal was opened for; only a matching file gets a select button. */
  wantedResourceType: ResourceType | null;
  previewPath: string | null;
  selectedPath: string;
  expandedPath: string | null;
  isResourceFavorite(resourceId: string): boolean;
}

/** Whether this file is already in the resource library, and under which id. */
export function folderFileLibraryMatch(file: FolderListingFile): { inLibrary: boolean; id: string } {
  const resources = uiState.resourceLibrary[file.resourceType] ?? [];
  const target = file.path.replace(/\\/g, "/").toLowerCase();
  const match = resources.find((res) => (res.filePath ?? "").replace(/\\/g, "/").toLowerCase() === target);
  if (match) return { inLibrary: true, id: match.id };
  if (file.alreadyInLibrary) return { inLibrary: true, id: file.libraryId ?? "" };
  return { inLibrary: false, id: "" };
}

/**
 * The tags to show on a file: the library's, once it has been imported, and
 * otherwise whatever the file's own metadata carries.
 */
export function getFolderFileTags(file: FolderListingFile): string[] {
  const match = folderFileLibraryMatch(file);
  if (match.inLibrary && match.id) {
    const resources = uiState.resourceLibrary[file.resourceType] ?? [];
    const resource = findResourceById(resources, match.id);
    if (resource) {
      return getResourceTags(resource);
    }
  }

  const metadata = file.metadata ?? {};
  const tagSources = [metadata.tags, metadata.tag, metadata.toneTags, metadata.categories]
    .filter((value): value is string => typeof value === "string");
  const tags = new Set<string>();
  tagSources.forEach((raw) => {
    splitTagValues(raw).forEach((tag) => tags.add(tag));
  });
  return Array.from(tags);
}

export function renderFolderTagFilters(bar: HTMLElement | null, availableTags: string[], activeTags: Set<string>): void {
  if (!bar) {
    return;
  }

  if (!availableTags.length) {
    bar.innerHTML = "";
    bar.classList.remove("is-active");
    return;
  }

  const selectedTags = Array.from(activeTags);
  const clearChip = selectedTags.length
    ? `<button class="preset-tag-filter-chip" type="button" data-tag="__clear__">Clear tags</button>`
    : "";

  const chips = availableTags.map((tag) => {
    const active = activeTags.has(tag);
    return `<button class="preset-tag-filter-chip${active ? " is-active" : ""}" type="button" data-tag="${escapeHtml(tag)}">${escapeHtml(tag)}</button>`;
  }).join("");

  bar.innerHTML = clearChip + chips;
  bar.classList.add("is-active");
}

export function renderFolderFileDetails(file: FolderListingFile): string {
  const metadata = file.metadata ?? {};
  const rows: string[] = [];
  rows.push(`<tr><td class="resource-browser-details-label">File Path</td><td class="resource-browser-details-value resource-browser-details-mono">${escapeHtml(file.path)}</td></tr>`);
  if (typeof file.sizeBytes === "number" && file.sizeBytes > 0) {
    rows.push(`<tr><td class="resource-browser-details-label">Size</td><td class="resource-browser-details-value">${escapeHtml(formatBytes(file.sizeBytes))}</td></tr>`);
  }
  for (const [key, value] of Object.entries(metadata)) {
    if (!value) continue;
    const label = key.replace(/_/g, " ").replace(/([A-Z])/g, " $1").trim();
    rows.push(`<tr><td class="resource-browser-details-label">${escapeHtml(label)}</td><td class="resource-browser-details-value">${escapeHtml(value)}</td></tr>`);
  }
  return `
      <div class="resource-browser-item-details-panel">
        <table class="resource-browser-details-table"><tbody>${rows.join("")}</tbody></table>
      </div>
    `;
}

export function renderFolderFileRow(file: FolderListingFile, state: FolderRowState): string {
  const metadata = file.metadata ?? {};
  const typeLabel = file.resourceType === "ir" ? "IR / Cab" : "NAM";
  const match = folderFileLibraryMatch(file);
  const badges: string[] = [`<span>${escapeHtml(typeLabel)}</span>`];
  const tags = getFolderFileTags(file);

  if (file.resourceType === "nam") {
    const architecture = normalizeNamArchitectureBadge(
      metadata.architectureVersion
      || metadata.architecture_version
      || metadata.architecture
      || "",
    );
    if (architecture) badges.push(`<span class="resource-browser-architecture-badge" title="Model architecture">${escapeHtml(architecture)}</span>`);
    const gear = [metadata.gearMake, metadata.gearModel].filter(Boolean).join(" ");
    if (gear) badges.push(`<span class="resource-browser-gear-desc" title="${escapeHtml(gear)}">${escapeHtml(gear)}</span>`);
    const toneType = metadata.toneType ?? "";
    if (toneType) badges.push(`<span class="resource-browser-tone-type">${escapeHtml(toneType.replace(/_/g, " "))}</span>`);
    if (metadata.sampleRate) badges.push(`<span>${escapeHtml(metadata.sampleRate)} Hz</span>`);
  } else {
    if (metadata.sampleRate) badges.push(`<span>${escapeHtml(metadata.sampleRate)} Hz</span>`);
    if (metadata.channels) badges.push(`<span>${escapeHtml(metadata.channels)} ch</span>`);
    if (metadata.durationSec) badges.push(`<span>${escapeHtml(metadata.durationSec)} s</span>`);
  }

  if (file.metadataPending && badges.length <= 1) {
    badges.push(`<span class="resource-browser-meta-pending" title="Reading metadata…">…</span>`);
  }
  if (tags.length) {
    const tagPills = tags
      .map((tag) => `<span class="resource-browser-tag-pill">${escapeHtml(tag)}</span>`)
      .join("");
    badges.push(`<span class="resource-browser-tag-list">${tagPills}</span>`);
  }

  const isFav = Boolean(match.id) && state.isResourceFavorite(match.id);
  const favBtn = `<button class="resource-browser-action-icon-btn resource-browser-item-fav-toggle${isFav ? " is-active" : ""}" type="button" data-path="${escapeHtml(file.path)}" data-resource-type="${escapeHtml(file.resourceType)}" title="${isFav ? "Remove from favourites" : "Add to favourites"}" aria-pressed="${isFav ? "true" : "false"}" aria-label="Toggle favourite">${isFav ? "★" : "☆"}</button>`;

  const typeMatches = state.wantedResourceType === file.resourceType;
  const isPreviewing = state.previewPath === file.path;
  const isSelected = state.selectedPath === file.path;
  const selectPreviewBtn = typeMatches
    ? `<button class="resource-browser-action-icon-btn resource-browser-folder-select-preview${isSelected ? " is-active" : ""}" type="button" data-path="${escapeHtml(file.path)}" title="Select in effect (does not close modal)" aria-label="Select in effect" aria-pressed="${isSelected ? "true" : "false"}">${getPlaySvg()}</button>`
    : "";

  const isExpanded = state.expandedPath === file.path;
  const detailsBtn = `<button class="resource-browser-action-icon-btn resource-browser-item-details-btn" type="button" data-path="${escapeHtml(file.path)}" title="${isExpanded ? "Hide details" : "Show details"}" aria-expanded="${isExpanded ? "true" : "false"}" aria-label="File details">ℹ</button>`;
  const editBtn = `<button class="resource-browser-action-icon-btn resource-browser-item-edit-btn" type="button" data-path="${escapeHtml(file.path)}" data-resource-type="${escapeHtml(file.resourceType)}" title="Edit name, category and tags" aria-label="Edit resource"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 1 1 3 3L7 19l-4 1 1-4Z"/></svg></button>`;

  return `
      <div class="resource-browser-folder-entry${isExpanded ? " is-details-expanded" : ""}${isPreviewing ? " is-previewing" : ""}" data-kind="file" data-path="${escapeHtml(file.path)}">
        <div class="results-item resource-browser-item resource-browser-folder-file-row">
          <div class="results-item-main resource-browser-item-info">
            <div class="results-item-title resource-browser-item-title">${escapeHtml(file.name)}</div>
            <div class="results-item-meta resource-browser-item-meta">${badges.join("")}</div>
          </div>
          <div class="resource-browser-item-actions">
            ${favBtn}
            ${editBtn}
            ${detailsBtn}
            ${selectPreviewBtn}
          </div>
        </div>
        ${isExpanded ? renderFolderFileDetails(file) : ""}
      </div>
    `;
}
