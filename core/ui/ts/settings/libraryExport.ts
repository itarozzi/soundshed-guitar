/**
 * Exporting the library as a portable archive, with only the resources the
 * selected presets actually use.
 */

import { arrayBufferToBase64, buildArchiveFileName, requestResourceData, sanitizeFilename } from "../archiveUtils.js";
import { postMessage } from "../bridge.js";
import { showNotification } from "../notifications.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { clonePreset, uiState } from "../state.js";
import type { BlendDefinition, LibraryResource, Preset, ResourceRef } from "../types.js";
import { sha256HexFromBase64 } from "../utils.js";
import { libraryExportButton, libraryExportResourcesSelect } from "./dom.js";
import { collectPresetBlendIds, collectPresetResourceRefs } from "./resourceUsage.js";

export function initLibraryExport(): void {
  if (!libraryExportButton) {
    return;
  }

  if ((libraryExportButton as HTMLButtonElement).dataset.bound === "true") {
    return;
  }

  (libraryExportButton as HTMLButtonElement).dataset.bound = "true";
  libraryExportButton.addEventListener("click", () => void exportLibraryArchive());
}

export type LibraryArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
  tags?: string[];
};

export type LibraryArchive = {
  formatVersion: number;
  createdAt: string;
  resourceMode: "used" | "all";
  presets: Preset[];
  blends: BlendDefinition[];
  resources: LibraryArchiveResource[];
};

export async function exportLibraryArchive(): Promise<void> {
  const presets = uiState.presets.map((preset) => clonePreset(uiState.presetCache.get(preset.id) ?? preset));
  const blends = (uiState.blendLibrary ?? []).map((blend) => JSON.parse(JSON.stringify(blend)) as BlendDefinition);

  if (!presets.length && !blends.length) {
    showNotification("Export failed", "No presets or blends available");
    return;
  }

  const zipLib = window.JSZip;
  if (!zipLib) {
    showNotification("Export failed", "Archive library not available");
    return;
  }

  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    showNotification("Export failed", "Unable to create archive");
    return;
  }
  const namFolder = resourcesFolder.folder("nam");
  const irFolder = resourcesFolder.folder("ir");
  if (!namFolder || !irFolder) {
    showNotification("Export failed", "Unable to create resource folders");
    return;
  }

  const resourceMode = libraryExportResourcesSelect?.value === "all" ? "all" : "used";
  const exportResources: LibraryArchiveResource[] = [];
  const resourceEntries: Array<{ type: string; resource: LibraryResource }> = [];

  if (resourceMode === "all") {
    const library = uiState.resourceLibrary;
    (library.nam ?? []).forEach((resource) => resourceEntries.push({ type: "nam", resource }));
    (library.ir ?? []).forEach((resource) => resourceEntries.push({ type: "ir", resource }));
  } else {
    const blendIds = new Set<string>();
    presets.forEach((preset) => {
      collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
    });
    const referencedBlends = blends.filter((blend) => blendIds.has(blend.id));
    const refs = new Map<string, ResourceRef>();
    presets.forEach((preset) => {
      collectPresetResourceRefs(preset, referencedBlends).forEach((ref) => {
        const resourceType = ref.resourceType ?? ref.type ?? "";
        const resourceId = ref.resourceId ?? ref.id ?? "";
        if (!resourceType || !resourceId) {
          return;
        }
        refs.set(`${resourceType}:${resourceId}`, ref);
      });
    });
    refs.forEach((ref) => {
      const resourceType = ref.resourceType ?? ref.type ?? "";
      const resourceId = ref.resourceId ?? ref.id ?? "";
      if (!resourceType || !resourceId) {
        return;
      }
      const resource = getLibraryResource(resourceType, resourceId);
      if (resource && (resourceType === "nam" || resourceType === "ir")) {
        resourceEntries.push({ type: resourceType, resource });
      }
    });
  }

  let missingCount = 0;
  for (const entry of resourceEntries) {
    const fileName = buildArchiveFileName(entry.resource, entry.type);
    const data = await requestResourceData(entry.type, entry.resource.id);
    if (!data) {
      missingCount += 1;
      continue;
    }
    const hash = await sha256HexFromBase64(data);
    const targetFolder = entry.type === "ir" ? irFolder : namFolder;
    targetFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: entry.resource.id,
      name: entry.resource.name,
      category: entry.resource.category,
      type: entry.type,
      fileName,
      hash,
      tags: entry.resource.tags ?? [],
    });
  }

  const archive: LibraryArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    resourceMode,
    presets,
    blends,
    resources: exportResources,
  };

  zip.file("library.json", JSON.stringify(archive, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  const buffer = await blob.arrayBuffer();
  const data = arrayBufferToBase64(buffer);

  if (missingCount > 0) {
    showNotification("Export warning", `${missingCount} resources could not be read`);
  }

  postMessage({
    type: "saveLibraryArchive",
    fileName: `${sanitizeFilename("Soundshed-Library")}.soundshed-library.zip`,
    data,
  });
}
