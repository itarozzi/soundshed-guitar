/**
 * The tone builder's inventory, read from the running app: the effect catalogue the engine sent
 * (EffectTypeRegistry) and the resource library. Kept apart from ./build.ts so the builder itself
 * stays free of app state and can run in scripts and the API worker.
 */

import { EffectTypeRegistry } from "../presetV2.js";
import { uiState } from "../state.js";
import type { LibraryResource } from "../types.js";
import type { BuildInventory, InventoryEffect, InventoryResource } from "./types.js";

function metadataText(metadata: Record<string, string> | undefined, ...keys: string[]): string | null {
  for (const key of keys) {
    const value = metadata?.[key];
    if (typeof value === "string" && value.trim()) {
      return value.trim();
    }
  }
  return null;
}

/** A library resource in the builder's provider-neutral shape. */
export function toInventoryResource(type: string, resource: LibraryResource): InventoryResource {
  const metadata = resource.metadata;
  return {
    id: resource.id,
    type,
    name: resource.name,
    category: resource.category,
    fileMissing: resource.fileMissing === true,
    filePath: resource.filePath ?? null,
    provider: metadataText(metadata, "provider"),
    creator: metadataText(metadata, "creatorName", "authorUsername", "creator"),
    hash: resource.hash ?? null,
    licence: metadataText(metadata, "licence", "license"),
    captureScope: metadataText(metadata, "captureScope"),
    metadata: metadata ? { ...metadata } : undefined,
  };
}

export function readAppBuildInventory(): BuildInventory {
  const effects: InventoryEffect[] = EffectTypeRegistry.getAll().map((effect) => ({
    type: effect.type,
    displayName: effect.displayName,
    category: effect.category,
    catalogHidden: effect.catalogHidden ?? false,
    requiresResource: effect.requiresResource ?? false,
    resourceType: effect.resourceType ?? null,
    parameters: (effect.parameters ?? []).map((p) => ({
      key: p.key,
      name: p.name,
      min: p.min,
      max: p.max,
      default: p.default,
      unit: p.unit ?? null,
      step: p.step ?? null,
      labels: p.labels ?? null,
    })),
    presets: (effect.presets ?? []).map((p) => ({ id: p.id, name: p.name, source: p.source ?? null, parameters: p.parameters ?? {} })),
  }));
  const library = uiState.resourceLibrary ?? {};
  const resources = Object.entries(library).flatMap(([type, list]) =>
    Array.isArray(list) ? list.map((resource) => toInventoryResource(type, resource)) : [],
  );
  return { effects, resources };
}
