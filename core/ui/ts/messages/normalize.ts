/**
 * Reading the host's loosely-typed resource and preset payloads into the shapes
 * UI state expects.
 */

import { getPresetSceneGraphs } from "../presetScenes.js";
import { uiState } from "../state.js";
import type { GlobalSignalChainConfig, LibraryResource, Preset, ResourceRef } from "../types.js";

export function normalizeResourceRef(ref?: ResourceRef | null): void {
  if (!ref) return;
  const resourceType = ref.resourceType ?? "";
  const resourceId = ref.resourceId ?? "";
  if (!ref.type && resourceType) {
    ref.type = resourceType;
  }

  if (!ref.id && resourceId) {
    ref.id = resourceId;
  }
  if (ref.type && !ref.resourceType) {
    ref.resourceType = ref.type;
  }
  if (ref.id && !ref.resourceId) {
    ref.resourceId = ref.id;
  }
}

export function normalizePresetResources(preset?: Preset | null): void {
  for (const graph of getPresetSceneGraphs(preset)) {
    graph.nodes.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((ref) => normalizeResourceRef(ref));
      }
    });
  }
}

export function normalizeResourcePath(path: string): string {
  return path.trim().replace(/\\/g, "/").toLowerCase();
}

export function upsertImportedResourceInUiState(info: { id?: string; name?: string; resourceType?: string; filePath?: string }): void {
  const resourceType = (info.resourceType ?? "").trim();
  const resourceId = (info.id ?? "").trim();
  if (!resourceType || !resourceId) return;

  const currentList = uiState.resourceLibrary[resourceType] ?? [];
  const importedPath = normalizeResourcePath(info.filePath ?? "");
  const existingIndex = currentList.findIndex((resource) => {
    if (resource.id === resourceId) return true;
    return importedPath.length > 0 && normalizeResourcePath(resource.filePath ?? "") === importedPath;
  });

  const existing = existingIndex >= 0 ? currentList[existingIndex] : null;
  const name = (info.name ?? "").trim() || existing?.name || resourceId;
  const filePath = (info.filePath ?? "").trim() || existing?.filePath || "";

  const nextResource: LibraryResource = {
    id: resourceId,
    name,
    category: existing?.category ?? "Uncategorized",
    description: existing?.description ?? "",
    tags: existing?.tags ?? [],
    filePath,
    hash: existing?.hash,
    metadata: existing?.metadata,
    fileMissing: existing?.fileMissing,
  };

  const nextList = [...currentList];
  if (existingIndex >= 0) {
    nextList[existingIndex] = nextResource;
  } else {
    nextList.push(nextResource);
  }

  uiState.resourceLibrary = {
    ...uiState.resourceLibrary,
    [resourceType]: nextList,
  };
}

export function removeResourceFromUiState(info: { id?: string; resourceType?: string }): void {
  const resourceType = (info.resourceType ?? "").trim();
  const resourceId = (info.id ?? "").trim();
  if (!resourceType || !resourceId) return;

  const currentList = uiState.resourceLibrary[resourceType] ?? [];
  const nextList = currentList.filter((resource) => resource.id !== resourceId);
  if (nextList.length === currentList.length) return;

  uiState.resourceLibrary = {
    ...uiState.resourceLibrary,
    [resourceType]: nextList,
  };
}

export function presetSignature(preset?: Preset | null): string {
  if (!preset) return "";

  const normalize = (value: unknown): unknown => {
    if (Array.isArray(value)) {
      return value.map(normalize);
    }
    if (value && typeof value === "object") {
      const obj = value as Record<string, unknown>;
      const cleaned: Record<string, unknown> = { ...obj };
      if (typeof cleaned.resourceType === "string" || typeof cleaned.type === "string") {
        cleaned.resourceType = typeof cleaned.resourceType === "string" ? cleaned.resourceType : cleaned.type;
        delete cleaned.type;
      }
      if (typeof cleaned.resourceId === "string" || typeof cleaned.id === "string") {
        cleaned.resourceId = typeof cleaned.resourceId === "string" ? cleaned.resourceId : cleaned.id;
        delete cleaned.id;
      }
      if (cleaned.filePath === "") {
        delete cleaned.filePath;
      }
      if (cleaned.embeddedId === "") {
        delete cleaned.embeddedId;
      }
      if (cleaned.parameterId === "") {
        delete cleaned.parameterId;
      }
      if (cleaned.parameters && typeof cleaned.parameters === "object" && cleaned.parameters !== null) {
        if (Object.keys(cleaned.parameters as Record<string, unknown>).length === 0) {
          delete cleaned.parameters;
        }
      }
      if (cleaned.params && typeof cleaned.params === "object" && cleaned.params !== null) {
        const params = cleaned.params as Record<string, unknown>;
        const cleanedParams: Record<string, unknown> = { ...params };
        delete cleanedParams.calibrationInputLevel;
        delete cleanedParams.calibrationOutputLevel;
        cleaned.params = cleanedParams;
      }
      const sorted: Record<string, unknown> = {};
      Object.keys(cleaned).sort().forEach((key) => {
        sorted[key] = normalize(cleaned[key]);
      });
      return sorted;
    }
    return value;
  };

  return JSON.stringify(normalize(preset));
}

export function normalizeGlobalSignalChain(chain?: GlobalSignalChainConfig | null): GlobalSignalChainConfig | null {
  if (!chain) {
    return null;
  }
  const normalizeGraph = (graph?: { nodes?: Array<Record<string, unknown>> } | null) => {
    if (!graph?.nodes) {
      return;
    }
    graph.nodes.forEach((node) => {
      const anyNode = node as { enabled?: boolean; bypassed?: boolean };
      if (typeof anyNode.bypassed !== "boolean") {
        if (typeof anyNode.enabled === "boolean") {
          anyNode.bypassed = !anyNode.enabled;
        } else {
          anyNode.bypassed = false;
        }
      }
      if (typeof anyNode.enabled !== "boolean") {
        anyNode.enabled = !anyNode.bypassed;
      }
    });
  };
  normalizeGraph(chain.preChainGraph as unknown as { nodes?: Array<Record<string, unknown>> });
  normalizeGraph(chain.postChainGraph as unknown as { nodes?: Array<Record<string, unknown>> });
  return chain;
}
