/**
 * Rendering the resource library: the filter bar, and the flat and
 * grouped-by-tone views behind it.
 */

import { postMessage } from "../bridge.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { renderIcon } from "../iconAssets.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";
import { bindBlendCreateButtons, bindBlendDeleteButtons, bindBlendGroupDragHandlers } from "./blendGroups.js";
import { libraryActiveTagFilters, libraryCategorySelect, libraryCreatorSelect, libraryResults, librarySearchInput, librarySourceSelect, librarySummary, libraryTagFilterBar, libraryTypeSelect, libraryViewSelect } from "./dom.js";
import { bindLibraryActions } from "./libraryActions.js";
import { buildMetadataBadges, getLibraryItemCreator, getLibraryItemFacets, getLibraryItemTags, getLibraryItems, groupLibraryItemsByTone, inferResourceOrigin, normalizeLibraryFilterValue } from "./libraryData.js";
import type { LibraryItem } from "./libraryData.js";
import { buildUsedResourceSet } from "./resourceUsage.js";

let libraryFiltersInitialized = false;

let libraryCreatorFilter = "all";

let libraryStateRequestedAt = 0;

export function initLibraryFilters(): void {
  if (libraryFiltersInitialized) {
    return;
  }
  libraryFiltersInitialized = true;

  librarySearchInput?.addEventListener("input", () => renderLibraryView());
  libraryTypeSelect?.addEventListener("change", () => renderLibraryView());
  librarySourceSelect?.addEventListener("change", () => renderLibraryView());
  libraryViewSelect?.addEventListener("change", () => renderLibraryView());
  libraryCategorySelect?.addEventListener("change", () => renderLibraryView());
  libraryCreatorSelect?.addEventListener("change", () => {
    libraryCreatorFilter = libraryCreatorSelect?.value ?? "all";
    renderLibraryView();
  });
  libraryTagFilterBar?.addEventListener("click", (event) => {
    const target = event.target as HTMLElement | null;
    const chip = target?.closest(".preset-tag-filter-chip") as HTMLButtonElement | null;
    if (!chip) {
      return;
    }
    const tag = chip.dataset.tag ?? "";
    if (!tag) {
      libraryActiveTagFilters.clear();
      renderLibraryView();
      return;
    }
    if (tag === "__clear__") {
      libraryActiveTagFilters.clear();
      renderLibraryView();
      return;
    }
    if (libraryActiveTagFilters.has(tag)) {
      libraryActiveTagFilters.delete(tag);
    } else {
      libraryActiveTagFilters.add(tag);
    }
    renderLibraryView();
  });
  bindLibraryActions();
}

export function renderLibraryFilterFacets(items: LibraryItem[]): void {
  const { tags, creators } = getLibraryItemFacets(items);

  if (libraryCreatorSelect) {
    const current = libraryCreatorFilter || "all";
    libraryCreatorSelect.innerHTML = [
      `<option value="all">All Creators</option>`,
      ...creators.map((creator) => `<option value="${escapeHtml(creator)}">${escapeHtml(creator)}</option>`),
    ].join("");
    libraryCreatorSelect.value = creators.includes(current) ? current : "all";
    libraryCreatorFilter = libraryCreatorSelect.value;
  }

  if (libraryTagFilterBar) {
    if (!tags.length) {
      libraryTagFilterBar.innerHTML = `<span class="results-empty">No tags available for these resources.</span>`;
      return;
    }

    const activeTags = libraryActiveTagFilters;
    libraryTagFilterBar.innerHTML = [
      `<button class="preset-tag-filter-chip${activeTags.size === 0 ? " active" : ""}" type="button" data-tag="">All Tags</button>`,
      ...tags.map((tag) => `<button class="preset-tag-filter-chip${activeTags.has(tag) ? " active" : ""}" type="button" data-tag="${escapeHtml(tag)}">${escapeHtml(tag)}</button>`),
      activeTags.size > 0 ? `<button class="preset-tag-filter-chip" type="button" data-tag="__clear__">Clear</button>` : "",
    ].join("");
  }
}

export function renderLibraryView(): void {
  if (!libraryResults) {
    return;
  }

  const resourcesHaveMissingFlags = Object.values(uiState.resourceLibrary ?? {}).some((entries) =>
    (entries ?? []).some((entry) => typeof entry?.fileMissing === "boolean"),
  );
  if (!resourcesHaveMissingFlags) {
    const now = Date.now();
    if (now - libraryStateRequestedAt > 1000) {
      libraryStateRequestedAt = now;
      postMessage({ type: "requestState" });
    }
  }

  const allItems = getLibraryItems();
  const query = (librarySearchInput?.value ?? "").trim().toLowerCase();
  const typeFilter = libraryTypeSelect?.value ?? "all";
  const sourceFilter = librarySourceSelect?.value ?? "all";
  const viewMode = libraryViewSelect?.value ?? "list";

  const typeFilteredItems = typeFilter === "all"
    ? allItems
    : allItems.filter((item) => item.type === typeFilter);

  updateCategoryOptions(typeFilteredItems);
  const categoryFilter = libraryCategorySelect?.value ?? "all";
  renderLibraryFilterFacets(typeFilteredItems);
  const creatorFilter = libraryCreatorSelect?.value ?? libraryCreatorFilter ?? "all";

  let filtered = typeFilteredItems;
  if (categoryFilter !== "all") {
    filtered = filtered.filter((item) => item.category === categoryFilter);
  }

  if (sourceFilter !== "all") {
    filtered = filtered.filter((item) => {
      const origin = inferResourceOrigin(item.filePath, item.metadata).toLowerCase();
      return origin === sourceFilter;
    });
  }

  if (creatorFilter !== "all") {
    const normalizedCreator = normalizeLibraryFilterValue(creatorFilter);
    filtered = filtered.filter((item) => normalizeLibraryFilterValue(getLibraryItemCreator(item)) === normalizedCreator);
  }

  if (libraryActiveTagFilters.size > 0) {
    filtered = filtered.filter((item) => {
      const resourceTags = getLibraryItemTags(item).map(normalizeLibraryFilterValue);
      return Array.from(libraryActiveTagFilters).every((tag) => resourceTags.includes(normalizeLibraryFilterValue(tag)));
    });
  }

  if (query) {
    filtered = filtered.filter((item) => {
      const haystack = [
        item.name,
        item.id,
        item.category,
        item.description,
        item.filePath,
        getLibraryItemCreator(item),
        ...getLibraryItemTags(item),
      ].join(" ").toLowerCase();
      return haystack.includes(query);
    });
  }

  if (viewMode === "grouped") {
    renderGroupedLibraryView(filtered, allItems.length);
    return;
  }

  renderListLibraryView(filtered, allItems.length);
}

export function renderListLibraryView(filtered: LibraryItem[], totalCount: number): void {
  if (!libraryResults) {
    return;
  }

  if (librarySummary) {
    librarySummary.textContent = `Showing ${filtered.length} of ${totalCount} resources`;
  }

  if (!filtered.length) {
    libraryResults.innerHTML = `<div class="equipment-library-empty">No resources match the current filters.</div>`;
    return;
  }

  const usedResources = buildUsedResourceSet();

  libraryResults.innerHTML = filtered
    .map((item) => renderLibraryItemRow(item, usedResources))
    .join("");
}

export function renderGroupedLibraryView(filtered: LibraryItem[], totalCount: number): void {
  if (!libraryResults) {
    return;
  }

  const grouped = groupLibraryItemsByTone(filtered);
  const usedResources = buildUsedResourceSet();
  if (librarySummary) {
    librarySummary.textContent = `Showing ${grouped.length} of ${totalCount} resources (grouped)`;
  }

  if (!grouped.length) {
    libraryResults.innerHTML = `<div class="equipment-library-empty">No grouped resources match the current filters.</div>`;
    return;
  }

  libraryResults.innerHTML = grouped
    .map((group) => {
      const typeLabel = group.types.size === 1
        ? Array.from(group.types)[0]
        : "Mixed";
      const categoryLabel = group.categories.size === 1
        ? Array.from(group.categories)[0]
        : "Multiple";
      const originLabel = group.origins.size === 1
        ? Array.from(group.origins)[0]
        : "Mixed";
      const modelCountLabel = `${group.count} models`;
      const usedCount = group.items.filter((item) => usedResources.has(`${item.type}:${item.id}`)).length;
      const usedBadge = usedCount > 0
        ? `<span class="equipment-library-used" title="${usedCount} used by presets or blends">${renderIcon("link", "equipment-library-used-icon")}</span>`
        : "";
      return `
        <div class="results-item equipment-library-item equipment-library-group" draggable="true" data-group-id="${escapeHtml(group.groupId)}">
          <div class="results-item-main equipment-library-item-main">
            <div class="results-item-title equipment-library-item-title">${escapeHtml(group.title)}${usedBadge}</div>
            <div class="results-item-meta equipment-library-item-meta">
              <span>${escapeHtml(typeLabel)}</span>
              <span>${escapeHtml(categoryLabel)}</span>
              <span>${escapeHtml(originLabel)}</span>
              <span>${escapeHtml(group.groupId)}</span>
              <span>${escapeHtml(modelCountLabel)}</span>
              ${group.items[0]?.metadata?.authorUsername ? `<span>by: ${escapeHtml(group.items[0].metadata.authorUsername)}</span>` : ""}
              ${(group.items[0]?.metadata?.sourceUrl ?? "").startsWith("https://www.tone3000.com/") ? `<a href="${escapeHtml(group.items[0].metadata!.sourceUrl!)}" target="_blank" rel="noopener noreferrer">↗ tone3000</a>` : ""}
            </div>
          </div>
          <div class="results-item-actions equipment-library-item-actions">
            ${isFeatureEnabled(Features.BlendTools) ? `<button class="equipment-library-create-blend" data-group-id="${escapeHtml(group.groupId)}">Create Blend</button>` : ""}
            <button class="equipment-library-delete-group icon-btn danger" data-group-id="${escapeHtml(group.groupId)}" title="Delete Group"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg></button>
          </div>
        </div>
      `;
    })
    .join("");

  bindBlendCreateButtons(grouped);
  bindBlendDeleteButtons(grouped);
  bindBlendGroupDragHandlers(grouped);
}

export function renderLibraryItemRow(item: LibraryItem, usedResources: Set<string>): string {
  const typeLabel = item.type === "ir" ? "Cab IR" : item.type === "nam" ? "Amp Model" : item.type.toUpperCase();
  const categoryLabel = item.category ? item.category : "Uncategorized";
  const originLabel = inferResourceOrigin(item.filePath, item.metadata);
  const metadataBadges = buildMetadataBadges(item.metadata);
  const creator = getLibraryItemCreator(item);
  const creatorBadge = creator ? `<span>by: ${escapeHtml(creator)}</span>` : "";
  const tags = getLibraryItemTags(item);
  const tagsBadge = tags.length
    ? `<span class="equipment-library-item-tags">${tags.map((tag) => `<span class="equipment-library-item-tag">${escapeHtml(tag)}</span>`).join("")}</span>`
    : "";
  const missingBadge = item.fileMissing ? "<span class=\"equipment-library-missing\">Missing File</span>" : "";
  const browseLabel = originLabel === "Local"
    ? (item.fileMissing ? "Set Path" : "Change Path")
    : (item.fileMissing ? "Browse File" : "Replace File");
  const browseAction = `<button class="equipment-library-browse" data-resource-type="${escapeHtml(item.type)}" data-resource-id="${escapeHtml(item.id)}">${browseLabel}</button>`;
  const editAction = originLabel !== "Built-in"
    ? `<button class="equipment-library-edit" data-resource-type="${escapeHtml(item.type)}" data-resource-id="${escapeHtml(item.id)}">Edit</button>`
    : "";
  const copyPathAction = originLabel === "Local" && Boolean(item.filePath.trim())
    ? `<button class="equipment-library-copy-path" data-resource-type="${escapeHtml(item.type)}" data-resource-id="${escapeHtml(item.id)}" title="Copy local file path" aria-label="Copy local file path"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg></button>`
    : "";
  const usedKey = `${item.type}:${item.id}`;
  const usedBadge = usedResources.has(usedKey)
    ? `<span class="equipment-library-used" title="Used by preset">${renderIcon("link", "equipment-library-used-icon")}</span>`
    : "";
  return `
    <div class="results-item equipment-library-item">
      <div class="results-item-main equipment-library-item-main">
        <div class="results-item-title equipment-library-item-title">${escapeHtml(item.name || item.id)}${usedBadge}</div>
        <div class="results-item-meta equipment-library-item-meta">
          <span>${escapeHtml(typeLabel)}</span>
          <span>${escapeHtml(categoryLabel)}</span>
          <span>${escapeHtml(originLabel)}</span>
          ${creatorBadge}
          ${missingBadge}
          <span>${escapeHtml(item.id)}</span>
          ${metadataBadges}
          ${tagsBadge}
        </div>
        <div class="results-item-path equipment-library-item-path" title="${escapeHtml(item.filePath)}">${escapeHtml(item.filePath || "(no file path)")}</div>
      </div>
      <div class="results-item-actions equipment-library-item-actions">
        ${editAction}
        ${copyPathAction}
        ${browseAction}
      </div>
    </div>
  `;
}

export function updateCategoryOptions(items: LibraryItem[]): void {
  if (!libraryCategorySelect) {
    return;
  }

  const categories = Array.from(
    new Set(items.map((item) => item.category).filter((value) => value && value.trim().length > 0)),
  ).sort((a, b) => a.localeCompare(b));

  const previousSelection = libraryCategorySelect.value;
  const options = ["<option value=\"all\">All Categories</option>"]
    .concat(categories.map((category) => `
      <option value="${escapeHtml(category)}">${escapeHtml(category)}</option>
    `.trim()))
    .join("");

  libraryCategorySelect.innerHTML = options;
  if (previousSelection !== "all" && categories.includes(previousSelection)) {
    libraryCategorySelect.value = previousSelection;
  } else {
    libraryCategorySelect.value = "all";
  }
}
