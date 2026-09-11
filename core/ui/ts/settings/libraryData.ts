/**
 * The library list as data: flattening the NAM/IR/FX libraries into one shape,
 * deduplicating it, and deriving the facets the filters offer.
 */

import { getAudioFxLibrary, getIrLibrary } from "../dataLibraries.js";
import { uiState } from "../state.js";
import type { LibraryResource } from "../types.js";
import { escapeHtml } from "../utils.js";

export type LibraryItem = {
  type: string;
  id: string;
  name: string;
  category: string;
  description: string;
  filePath: string;
  tags?: string[];
  metadata?: Record<string, string>;
  fileMissing?: boolean;
};

export function normalizeLibraryFilterValue(value: string): string {
  return value.trim().replace(/\s+/g, " ").toLowerCase();
}

export function splitLibraryTags(raw: string): string[] {
  return raw
    .split(/[,;/|]/g)
    .map((tag) => tag.trim())
    .filter((tag) => Boolean(tag));
}

export function getLibraryItemTags(item: LibraryItem): string[] {
  const tags = new Set<string>();

  (item.tags ?? []).forEach((tag) => {
    const value = tag.trim();
    if (value) {
      tags.add(value);
    }
  });

  const metadataTags = item.metadata?.tags;
  if (typeof metadataTags === "string") {
    splitLibraryTags(metadataTags).forEach((tag) => tags.add(tag));
  }

  return Array.from(tags);
}

export function getLibraryItemCreator(item: LibraryItem): string {
  const metadata = item.metadata ?? {};
  return (
    metadata.creatorName
    ?? metadata.authorUsername
    ?? metadata.modeledBy
    ?? metadata.creator
    ?? metadata.provider
    ?? ""
  ).trim();
}

export function getLibraryItemFacets(items: LibraryItem[]): { tags: string[]; creators: string[] } {
  const tags = new Set<string>();
  const creators = new Set<string>();

  items.forEach((item) => {
    getLibraryItemTags(item).forEach((tag) => tags.add(tag));
    const creator = getLibraryItemCreator(item);
    if (creator) {
      creators.add(creator);
    }
  });

  return {
    tags: Array.from(tags).sort((a, b) => a.localeCompare(b)),
    creators: Array.from(creators).sort((a, b) => a.localeCompare(b)),
  };
}

export function getLibraryItems(): LibraryItem[] {
  const items: LibraryItem[] = [];
  const builtInModels = getAudioFxLibrary();
  const builtInIrs = getIrLibrary();

  builtInModels.forEach((model) => {
    items.push({
      type: "nam",
      id: model.id,
      name: humanizeId(model.id),
      category: "Built-in",
      description: "",
      filePath: model.filePath,
      metadata: { source: "built-in" },
    });
  });

  builtInIrs.forEach((ir) => {
    items.push({
      type: "ir",
      id: ir.id,
      name: humanizeId(ir.id),
      category: "Built-in",
      description: "",
      filePath: ir.filePath,
      metadata: { source: "built-in" },
    });
  });

  const library = uiState.resourceLibrary ?? {};
  Object.entries(library).forEach(([type, resources]) => {
    if (!Array.isArray(resources)) {
      return;
    }
    resources.forEach((res) => {
      if (!res || typeof res !== "object") {
        return;
      }
      const entry = res as LibraryResource;
      const entryFilePath = entry.filePath ?? "";
      items.push({
        type,
        id: entry.id ?? "",
        name: entry.name ?? entry.id ?? "",
        category: entry.category ?? "Imported",
        description: entry.description ?? "",
        filePath: entryFilePath,
        tags: entry.tags ?? undefined,
        metadata: entry.metadata ?? { source: "imported" },
        fileMissing: entry.fileMissing ?? entryFilePath.length === 0,
      });
    });
  });
  return dedupeLibraryItems(items);
}

export type ToneGroup = {
  groupId: string;
  title: string;
  count: number;
  types: Set<string>;
  categories: Set<string>;
  origins: Set<string>;
  modelIds: string[];
  items: LibraryItem[];
  gear?: string;
};

export function groupLibraryItemsByTone(items: LibraryItem[]): ToneGroup[] {
  const groups = new Map<string, ToneGroup>();
  items.forEach((item) => {
    const toneId = item.metadata?.toneId ?? item.metadata?.groupId;
    const toneTitle = item.metadata?.toneTitle ?? item.metadata?.groupName;
    if (!toneId || !toneTitle) {
      return;
    }

    const key = `${toneId}:${toneTitle}`;
    const existing = groups.get(key);
    const origin = inferResourceOrigin(item.filePath, item.metadata);
    const gear = item.metadata?.gear ?? item.category;
    if (existing) {
      existing.count += 1;
      existing.types.add(item.type);
      existing.categories.add(item.category || "Uncategorized");
      existing.origins.add(origin);
      if (item.type === "nam") {
        existing.modelIds.push(item.id);
      }
      existing.items.push(item);
      if (!existing.gear && gear) {
        existing.gear = gear;
      }
    } else {
      groups.set(key, {
        groupId: toneId,
        title: toneTitle,
        count: 1,
        types: new Set([item.type]),
        categories: new Set([item.category || "Uncategorized"]),
        origins: new Set([origin]),
        modelIds: item.type === "nam" ? [item.id] : [],
        items: [item],
        gear: gear,
      });
    }
  });

  return Array.from(groups.values()).sort((a, b) => a.title.localeCompare(b.title));
}

export function inferResourceOrigin(filePath: string, metadata?: Record<string, string>): string {
  const provider = (metadata?.provider ?? metadata?.archiveProvider ?? metadata?.source ?? "").trim().toLowerCase();
  if (provider === "tone3000") {
    return "Tone3000";
  }

  const importedProviders = new Set([
    "presetarchive",
    "blendarchive",
    "factory-archives",
    "remote",
    "generatedpack",
    "zipimport",
    "tonesharingapi",
    "imported",
  ]);
  if (provider && importedProviders.has(provider)) {
    return "Imported";
  }

  if (!filePath) {
    return "Unknown";
  }
  const normalized = filePath.toLowerCase().replace(/\\/g, "/");
  if (normalized.includes("/content/tone3000/")) {
    return "Tone3000";
  }
  if (normalized.includes("/content/presetarchive/")
    || normalized.includes("/content/blendarchive/")
    || normalized.includes("/content/factory-archives/")
    || normalized.includes("/content/remote/")
    || normalized.includes("/imports/")) {
    return "Imported";
  }
  if (normalized.includes("/settings/") || normalized.includes("/appdata/") || normalized.includes("/documents/")) {
    return "Local";
  }
  return "Built-in";
}

export function buildMetadataBadges(metadata?: Record<string, string>): string {
  if (!metadata) {
    return "";
  }

  const badges: string[] = [];
  const provider = metadata.provider;
  const toneTitle = metadata.toneTitle;
  const groupId = metadata.groupId;
  const groupName = metadata.groupName;
  const gear = metadata.gear;
  const modelName = metadata.modelName;
  const entryName = metadata.entryName;
  const sourceUrl = metadata.sourceUrl;
  const authorUsername = metadata.authorUsername;

  if (provider) badges.push(`<span>${escapeHtml(provider)}</span>`);
  if (toneTitle) badges.push(`<span>tone: ${escapeHtml(toneTitle)}</span>`);
  if (groupName) badges.push(`<span>group: ${escapeHtml(groupName)}</span>`);
  if (groupId) badges.push(`<span>groupId: ${escapeHtml(groupId)}</span>`);
  if (gear) badges.push(`<span>gear: ${escapeHtml(gear)}</span>`);
  if (modelName) badges.push(`<span>model: ${escapeHtml(modelName)}</span>`);
  if (entryName) badges.push(`<span>file: ${escapeHtml(entryName)}</span>`);
  if (authorUsername) badges.push(`<span>by: ${escapeHtml(authorUsername)}</span>`);
  const safeTone3000Url = sourceUrl?.startsWith("https://www.tone3000.com/") ? sourceUrl : null;
  if (safeTone3000Url) badges.push(`<a href="${escapeHtml(safeTone3000Url)}" target="_blank" rel="noopener noreferrer">↗ tone3000</a>`);

  return badges.join("");
}

export function dedupeLibraryItems(items: LibraryItem[]): LibraryItem[] {
  const map = new Map<string, LibraryItem>();
  const originRank = (origin: string): number => {
    if (origin === "Imported") return 3;
    if (origin === "Local") return 2;
    if (origin === "Built-in") return 1;
    return 0;
  };

  items.forEach((item) => {
    const key = `${item.type}:${item.id}`;
    const existing = map.get(key);
    if (!existing) {
      map.set(key, item);
      return;
    }
    const existingOrigin = inferResourceOrigin(existing.filePath, existing.metadata);
    const currentOrigin = inferResourceOrigin(item.filePath, item.metadata);
    if (originRank(currentOrigin) > originRank(existingOrigin)) {
      map.set(key, item);
    }
  });
  return Array.from(map.values());
}

export function humanizeId(value: string): string {
  if (!value) {
    return "Resource";
  }
  return value
    .replace(/[_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/\b\w/g, (char) => char.toUpperCase());
}
