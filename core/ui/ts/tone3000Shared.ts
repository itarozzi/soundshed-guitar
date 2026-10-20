import { uiState } from "./state.js";
import { postMessage } from "./bridge.js";
import { tone3000AuthenticatedFetch } from "./tone3000.js";
import { addModels, definitionFromEditState, type BlendEditState } from "./blendEditor/editState.js";
import { arrayBufferToBase64 } from "./utils.js";
import { normaliseLicenceId, remoteResourceMetadata } from "./remoteResources.js";
import type { Tone3000Architecture, Tone3000License, Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import type { BlendDefinition, LibraryResource } from "./types.js";
import { fetchAllTone3000ModelPages, sortTone3000ModelsByName } from "./tone3000Api.js";

interface JSZipObject {
  name: string;
  dir: boolean;
  async(type: "arraybuffer"): Promise<ArrayBuffer>;
}

export interface Tone3000ImportResult {
  importedResourceIds: string[];
  importedNamIds: string[];
  /** Each imported NAM model's own name (the Tone3000 model, or its file in a zip). */
  importedNamNames: Record<string, string>;
}

export function getTone3000ImageUrl(tone: Tone3000Tone): string | null {
  const candidates = [
    Array.isArray(tone.images) ? tone.images[0] : undefined,
    tone.equipment_image_url,
    tone.equipment_image,
    tone.gear_image_url,
    tone.image_url,
    tone.thumbnail_url,
  ];

  for (const candidate of candidates) {
    const value = typeof candidate === "string" ? candidate.trim() : "";
    if (!value) {
      continue;
    }
    if (value.startsWith("http://") || value.startsWith("https://") || value.startsWith("data:")) {
      return value;
    }
  }

  return null;
}

/// Artwork is not part of the model download, so anything imported before the
/// image URL was recorded has no `imageUrl` in its metadata. Every tone listing
/// the API returns already carries the image, so fill it in for matching library
/// resources as the user browses rather than re-fetching tones later.
export function backfillTone3000ResourceImages(tones: Tone3000Tone[]): void {
  const imageUrlByToneId = new Map<string, string>();
  tones.forEach((tone) => {
    const imageUrl = getTone3000ImageUrl(tone);
    if (imageUrl) {
      imageUrlByToneId.set(String(tone.id), imageUrl);
    }
  });
  if (!imageUrlByToneId.size) {
    return;
  }

  for (const resourceType of ["nam", "ir"] as const) {
    (uiState.resourceLibrary[resourceType] ?? []).forEach((resource) => {
      const metadata = resource.metadata;
      if (!metadata || metadata.provider !== "tone3000" || metadata.imageUrl) {
        return;
      }

      const imageUrl = imageUrlByToneId.get(metadata.toneId ?? "");
      if (!imageUrl) {
        return;
      }

      const updated = { ...metadata, imageUrl };
      resource.metadata = updated;
      postMessage({
        type: "updateLibraryResource",
        resourceType,
        resourceId: resource.id,
        name: resource.name,
        category: resource.category,
        description: resource.description,
        metadata: updated,
      });
    });
  }
}

/** The saved pack identity and attribution, usable even through proxies without a tone-detail endpoint. */
export function tone3000ToneFromResource(resource: LibraryResource | undefined): Tone3000Tone | null {
  const metadata = resource?.metadata;
  if (metadata?.provider !== "tone3000") {
    return null;
  }
  const toneId = metadata.toneId?.trim() || metadata.groupId?.trim();
  if (!toneId) {
    return null;
  }
  const licence = normaliseLicenceId(metadata.licence ?? metadata.license);
  const licenses: Tone3000License[] = ["t3k", "cc-by", "cc-by-sa", "cc-by-nc", "cc-by-nc-sa", "cc-by-nd", "cc-by-nc-nd", "cco"];
  const sourcePrefix = "https://www.tone3000.com/tones/";
  return {
    id: toneId,
    title: metadata.toneTitle?.trim() || metadata.groupName?.trim() || `Tone3000 pack ${toneId}`,
    description: resource?.description,
    gear: metadata.gear || metadata.tone3000Category,
    platform: metadata.platform,
    license: licenses.find((value) => value === (licence === "cc0" ? "cco" : licence)),
    slug: metadata.sourceUrl?.startsWith(sourcePrefix) ? metadata.sourceUrl.slice(sourcePrefix.length).split(/[?#]/)[0] : undefined,
    images: metadata.imageUrl ? [metadata.imageUrl] : undefined,
    user: {
      id: metadata.creatorId ?? "",
      username: metadata.authorUsername ?? "",
      display_name: metadata.creatorName,
      url: metadata.creatorUrl,
    },
  };
}

/// Sorted by name here, so the resource browser, its next/prev stepping and the
/// Settings browser all list a tone's models in the same order.
export async function fetchTone3000Models(
  tone: Tone3000Tone,
  architecture?: Tone3000Architecture,
): Promise<Tone3000Model[]> {
  const models = await fetchAllTone3000ModelPages(async (url) => {
    const response = await tone3000AuthenticatedFetch(url);
    if (!response.ok) {
      throw new Error(`Model fetch failed: ${response.status}`);
    }
    return response.json();
  }, tone.id, architecture);
  return sortTone3000ModelsByName(models);
}

export async function importTone3000Models(
  tone: Tone3000Tone,
  models: Tone3000Model[],
  onProgress?: (completed: number, total: number, currentName?: string) => void,
): Promise<Tone3000ImportResult> {
  const resourceType = (tone.platform ?? "nam").toLowerCase() === "ir" ? "ir" : "nam";
  const gearFolder = sanitizeFilename(tone.gear ?? "other");
  const toneLabel = tone.title ?? tone.name ?? "tone";
  const toneFolder = sanitizeFilename(toneLabel);
  const subfolder = `${gearFolder}/${toneFolder}`;

  const importedResourceIds: string[] = [];
  const importedNamIds: string[] = [];
  const importedNamNames: Record<string, string> = {};

  let completed = 0;
  const total = models.length;

  for (const model of models) {
    const modelResponse = await tone3000AuthenticatedFetch(model.model_url);

    if (!modelResponse.ok) {
      throw new Error(`Model download failed: ${modelResponse.status}`);
    }

    const buffer = await modelResponse.arrayBuffer();
    const contentType = modelResponse.headers.get("content-type") ?? "";
    const fileNameHint = sanitizeFilename(model.name ?? toneLabel ?? "model");

    if (contentType.includes("zip") || model.model_url.toLowerCase().endsWith(".zip")) {
      const zipped = await importZipBuffer(buffer, {
        tone,
        model,
        subfolder,
        fallbackNameHint: fileNameHint,
        resourceType,
      });
      importedResourceIds.push(...zipped.importedResourceIds);
      importedNamIds.push(...zipped.importedNamIds);
      Object.assign(importedNamNames, zipped.importedNamNames);
    } else {
      const data = arrayBufferToBase64(buffer);
      const extension = resourceType === "ir" ? ".wav" : ".nam";
      const fileName = `${fileNameHint}${extension}`;
      const resourceId = `tone3000:${model.id}`;

      importRemoteResource({
        tone,
        model,
        resourceType,
        resourceId,
        fileName,
        subfolder,
        data,
      });

      importedResourceIds.push(resourceId);
      if (resourceType === "nam") {
        importedNamIds.push(resourceId);
        importedNamNames[resourceId] = model.name ?? "";
      }
    }

    completed += 1;
    onProgress?.(completed, total, model.name ?? model.id);
  }

  return {
    importedResourceIds,
    importedNamIds,
    importedNamNames,
  };
}

/**
 * A blend of a tone's models, placed the way the blend editor places added models: by what
 * the names say they were captured at ("G5" for gain 5), on the parameters that differ, or on
 * the Sweep when the names say nothing. The names come from the import itself: the resource
 * library has not heard about these models yet when the blend is made.
 */
export function createTone3000BlendDefinition(tone: Tone3000Tone, modelIds: string[], modelNames: Record<string, string> = {}): BlendDefinition {
  const id = typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const known = (uiState.resourceLibrary.nam ?? []).filter((resource) => !modelIds.includes(resource.id) || !modelNames[resource.id]);
  const fresh = modelIds
    .filter((modelId) => modelNames[modelId] || !known.some((resource) => resource.id === modelId))
    .map((modelId) => ({
      id: modelId,
      name: modelNames[modelId] || modelId,
      category: tone.gear ?? "",
      metadata: { gear: tone.gear ?? "", toneId: String(tone.id), toneTitle: tone.title ?? tone.name ?? "" },
    }) as unknown as LibraryResource);
  const library = { ...uiState.resourceLibrary, nam: [...known, ...fresh] };
  const state: BlendEditState = {
    id,
    name: `${tone.title ?? tone.name ?? "Tone3000"} Blend`,
    category: normalizeBlendCategory(tone.gear),
    mode: "interpolate",
    axis: "sweep",
    params: [],
    rows: [],
  };
  addModels(state, modelIds, library);
  return definitionFromEditState(state, library);
}

/** What a capture filed under this gear covers, in the provider-neutral words (remoteResources.ts). */
export function tone3000CaptureScope(gear: string | undefined): string | null {
  switch (gear) {
    case "amp": return "amp";
    case "amp-cab":
    case "full-rig": return "amp+cab";
    case "pedal": return "pedal";
    case "outboard": return "outboard";
    case "cab": return "cab";
    case "space": return "room";
    default: return null;
  }
}

/**
 * The library metadata for a model imported from a tone: the provider-neutral keys
 * (remoteResources.ts) plus the tone and model fields this provider needs to find it again.
 */
export function tone3000ResourceMetadata(
  tone: Tone3000Tone,
  model: { id: string | number; name?: string; modelUrl?: string; architectureVersion?: string },
  entryName?: string,
): Record<string, string> {
  const sourceUrl = `https://www.tone3000.com/tones/${tone.slug ?? tone.id}`;
  const creatorName = tone.user?.display_name ?? tone.user?.name ?? tone.user?.username ?? "";
  return {
    ...remoteResourceMetadata({
      provider: "tone3000",
      providerKey: entryName ? `${model.id}:${sanitizeFilename(entryName)}` : String(model.id),
      licence: tone.license ?? null,
      creatorName,
      creatorUrl: tone.user?.url ?? null,
      sourceUrl,
      captureScope: tone3000CaptureScope(tone.gear),
    }),
    tone3000Category: tone.gear ?? "",
    toneId: String(tone.id),
    toneTitle: tone.title ?? "",
    groupId: String(tone.id),
    groupName: tone.title ?? tone.name ?? "",
    gear: tone.gear ?? "",
    platform: tone.platform ?? "",
    modelId: String(model.id),
    modelName: model.name ?? "",
    ...(model.modelUrl ? { modelUrl: model.modelUrl } : {}),
    imageUrl: getTone3000ImageUrl(tone) ?? "",
    architectureVersion: model.architectureVersion ?? "",
    ...(entryName ? { entryName } : {}),
    sourceUrl,
    creatorId: tone.user?.id != null ? String(tone.user.id) : "",
    creatorName,
    authorUsername: tone.user?.username ?? "",
  };
}

function importRemoteResource(options: {
  tone: Tone3000Tone;
  model: Tone3000Model;
  resourceType: "nam" | "ir";
  resourceId: string;
  fileName: string;
  subfolder: string;
  data: string;
  entryName?: string;
}): void {
  const { tone, model, resourceType, resourceId, fileName, subfolder, data, entryName } = options;

  postMessage({
    type: "importRemoteResource",
    provider: "tone3000",
    resourceType,
    resourceId,
    name: `${tone.title} - ${entryName ?? model.name}`,
    description: tone.description ?? "",
    category: tone.gear ?? "",
    subfolder,
    fileName,
    metadata: tone3000ResourceMetadata(tone, {
      id: model.id,
      name: model.name,
      modelUrl: model.model_url,
      architectureVersion: model.architecture_version ?? undefined,
    }, entryName),
    data,
  });
}

async function importZipBuffer(
  buffer: ArrayBuffer,
  options: {
    tone: Tone3000Tone;
    model: Tone3000Model;
    subfolder: string;
    fallbackNameHint: string;
    resourceType: "nam" | "ir";
  },
): Promise<Tone3000ImportResult> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("JSZip not loaded");
  }

  const zip = await zipLib.loadAsync(buffer);
  const entries = Object.values(zip.files) as JSZipObject[];
  const importedResourceIds: string[] = [];
  const importedNamIds: string[] = [];
  const importedNamNames: Record<string, string> = {};

  for (const entry of entries) {
    if (entry.dir) {
      continue;
    }

    const lowerName = entry.name.toLowerCase();
    const isNam = lowerName.endsWith(".nam") || lowerName.endsWith(".json");
    const isIr = lowerName.endsWith(".wav") || lowerName.endsWith(".ir");

    if ((options.resourceType === "nam" && !isNam) || (options.resourceType === "ir" && !isIr)) {
      continue;
    }

    const fileBuffer = await entry.async("arraybuffer");
    const data = arrayBufferToBase64(fileBuffer);
    const fileName = sanitizeFilename(entry.name.split("/").pop() ?? options.fallbackNameHint);
    const resourceId = `tone3000:${options.model.id}:${sanitizeFilename(entry.name)}`;

    importRemoteResource({
      tone: options.tone,
      model: options.model,
      resourceType: options.resourceType,
      resourceId,
      fileName,
      subfolder: options.subfolder,
      data,
      entryName: entry.name,
    });

    importedResourceIds.push(resourceId);
    if (options.resourceType === "nam") {
      importedNamIds.push(resourceId);
      importedNamNames[resourceId] = entry.name.split("/").pop() ?? "";
    }
  }

  if (!importedResourceIds.length) {
    throw new Error("No supported files found in archive");
  }

  return {
    importedResourceIds,
    importedNamIds,
    importedNamNames,
  };
}

function normalizeBlendCategory(category?: string): string {
  const value = (category ?? "").toLowerCase();
  const allowed = new Set(["pedal", "preamp", "amp", "full-rig", "cab"]);
  if (allowed.has(value)) {
    return value;
  }
  return "amp";
}

function sanitizeFilename(raw: string): string {
  const trimmed = raw.trim() || "resource";
  return trimmed.replace(/[^a-z0-9-_.]+/gi, "-");
}
