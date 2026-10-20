/**
 * Blend archives (.namz): a blend definition with the model files it plays, for sharing a
 * blend with someone who does not have the models. Exported and imported from Settings →
 * Blends; an import adds its models to the library (reusing any already there, by hash) and
 * saves the blend under a new id.
 */

import { arrayBufferToBase64, buildArchiveFileName, generateResourceId, requestResourceData, sanitizeFilename } from "./archiveUtils.js";
import { collectBlendModelIds } from "./blendUtils.js";
import { postMessage } from "./bridge.js";
import type { BlendDefinition, LibraryResource, ResourceLibrary } from "./types.js";
import { sha256HexFromBase64 } from "./utils.js";

function findByHash(library: ResourceLibrary, hash?: string): LibraryResource | undefined {
  if (!hash) {
    return undefined;
  }
  const normalized = hash.toLowerCase();
  return (library.nam ?? []).find((resource) => resource.hash?.toLowerCase() === normalized);
}

/** Writes the blend and its models to a .namz the engine saves where the user chooses. */
export async function exportBlendArchive(blend: BlendDefinition, library: ResourceLibrary): Promise<boolean> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    return false;
  }
  const zip = new zipLib();
  const resourcesFolder = zip.folder("resources");
  if (!resourcesFolder) {
    return false;
  }

  const resources = library.nam ?? [];
  const exportResources: Array<{ id: string; name: string; category: string; type: string; fileName: string; hash?: string }> = [];
  for (const modelId of collectBlendModelIds(blend)) {
    const resource = resources.find((res) => res.id === modelId);
    const data = resource ? await requestResourceData("nam", resource.id) : undefined;
    if (!resource || !data) {
      continue;
    }
    const fileName = buildArchiveFileName(resource, "nam");
    resourcesFolder.file(fileName, data, { base64: true });
    exportResources.push({
      id: resource.id,
      name: resource.name,
      category: resource.category,
      type: "nam",
      fileName,
      hash: await sha256HexFromBase64(data),
    });
  }

  zip.file("blend.json", JSON.stringify({ formatVersion: 1, blend: { ...blend }, resources: exportResources }, null, 2));
  const blob = await zip.generateAsync({ type: "blob" });
  postMessage({
    type: "saveBlendArchive",
    fileName: `${sanitizeFilename(blend.name || blend.id || "blend")}.namz`,
    data: arrayBufferToBase64(await blob.arrayBuffer()),
  });
  return true;
}

/** Imports a .namz: its models into the library, its blend under a new id. Returns the blend's name. */
export async function importBlendArchive(file: File, library: ResourceLibrary): Promise<string | null> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    return null;
  }
  const zip = await zipLib.loadAsync(await file.arrayBuffer());
  const blendEntry = zip.file("blend.json");
  if (!blendEntry) {
    return null;
  }
  const archive = JSON.parse(await blendEntry.async("text")) as {
    blend?: BlendDefinition;
    resources?: Array<{ id?: string; fileName?: string; name?: string; category?: string; hash?: string }>;
  };

  const fileMap = new Map<string, JSZipObject>();
  (Object.values(zip.files) as JSZipObject[]).forEach((entry) => {
    if (!entry.dir) {
      fileMap.set(entry.name.replace(/^resources\//, ""), entry);
    }
  });

  const idMap = new Map<string, string>();
  for (const resource of archive.resources ?? []) {
    const fileName = resource.fileName ?? "";
    const existing = findByHash(library, resource.hash);
    if (existing) {
      idMap.set(resource.id ?? fileName, existing.id);
      continue;
    }
    const entry = fileMap.get(fileName);
    if (!entry) {
      continue;
    }
    const newId = generateResourceId(fileName);
    idMap.set(resource.id ?? fileName, newId);
    postMessage({
      type: "importRemoteResource",
      provider: "blendArchive",
      resourceType: "nam",
      resourceId: newId,
      name: resource.name ?? fileName,
      description: "",
      category: resource.category ?? "",
      subfolder: "blend-imports",
      fileName,
      hash: resource.hash ?? "",
      metadata: { provider: "blendArchive", sourceFile: fileName },
      data: arrayBufferToBase64(await entry.async("arraybuffer")),
    });
  }

  const blend = archive.blend;
  if (!blend) {
    return null;
  }
  const remapId = (id: string): string => idMap.get(id) ?? id;
  postMessage({
    type: "saveBlendDefinition",
    blend: {
      ...blend,
      id: generateResourceId(blend.id || blend.name || "blend"),
      models: (blend.models ?? []).map(remapId),
      modelMappings: (blend.modelMappings ?? []).map((mapping) => ({ ...mapping, id: remapId(mapping.id) })),
    },
  });
  return blend.name || "Blend";
}
