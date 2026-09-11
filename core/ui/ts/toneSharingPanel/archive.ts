/**
 * Reading a published preset out of its archive, and building the portable
 * .zip a pack download hands to the importer.
 */

import { buildApiUrl, parseApiErrorMessage } from "./api.js";
import { resolveCreatorProfileHandle } from "./format.js";
import { toneSharingState } from "./state.js";
import type { ItemArchive, ItemArchiveResource, ItemCollectionArchive, Tone3000ResourceRef, ToneSharingPackDetails } from "./types.js";

export async function readPresetFromArchive(buffer: ArrayBuffer): Promise<Record<string, unknown>> {
  const bytes = new Uint8Array(buffer);
  const isZip = bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b;

  if (isZip) {
    const zipLib = window.JSZip;
    if (!zipLib) {
      throw new Error("JSZip not loaded");
    }
    const zip = await zipLib.loadAsync(buffer);
    const entries = Object.values(zip.files).filter((entry) => !entry.dir && entry.name.toLowerCase().endsWith(".json"));
    if (!entries.length) {
      throw new Error("Preset archive does not contain a JSON preset file");
    }
    const presetText = await entries[0].async("text");
    const parsed = JSON.parse(presetText) as Record<string, unknown>;
    return (parsed.preset as Record<string, unknown>) ?? parsed;
  }

  const text = new TextDecoder().decode(buffer);
  const parsed = JSON.parse(text) as Record<string, unknown>;
  return (parsed.preset as Record<string, unknown>) ?? parsed;
}

export async function buildPackArchiveFromDetails(details: ToneSharingPackDetails): Promise<{ blob: Blob; fileName: string }> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const mergedPresets: Array<Record<string, unknown>> = [];
  const mergedResources = new Map<string, { entry: ItemArchiveResource; bytes: Uint8Array }>();
  const mergedBlends = new Map<string, Record<string, unknown>>();
  const mergedTone3000Resources = new Map<string, Tone3000ResourceRef>();

  const sortedItems = [...details.items].sort((a, b) => a.sortOrder - b.sortOrder);
  const packCreatorHandle = resolveCreatorProfileHandle(details.pack as unknown as Record<string, unknown>) ?? undefined;
  const packCreatorId = details.pack.creatorUserId ?? undefined;
  for (const item of sortedItems) {
    const response = await fetch(buildApiUrl(`/items/${item.itemId}/download`), {
      headers: toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {},
      credentials: "include"
    });

    if (!response.ok) {
      const message = await parseApiErrorMessage(response);
      throw new Error(`Failed to download pack item ${item.title}: ${message}`);
    }

    const buffer = await response.arrayBuffer();
    const bytes = new Uint8Array(buffer);
    const isZip = bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b;
    if (!isZip) {
      const preset = await readPresetFromArchive(buffer);
      preset.name = item.title || preset.name || "Imported Preset";
      preset.toneSharingOrigin = {
        source: "toneSharingApi",
        itemId: item.itemId,
        originalPresetId: typeof preset.id === "string" ? preset.id : item.itemId,
        importedAt: new Date().toISOString(),
        importedFromPackId: details.pack.id,
        creatorId: packCreatorId,
        creatorHandle: packCreatorHandle,
        republishBlocked: true,
      };
      mergedPresets.push(preset);
      continue;
    }

    const zip = await zipLib.loadAsync(buffer);
    const presetEntry = zip.file("preset.json");
    const presetsEntry = zip.file("presets.json");

    if (presetEntry) {
      const parsed = JSON.parse(await presetEntry.async("text")) as ItemArchive;
      if (parsed.preset) {
        parsed.preset.name = item.title || parsed.preset.name || "Imported Preset";
        parsed.preset.toneSharingOrigin = {
          source: "toneSharingApi",
          itemId: item.itemId,
          originalPresetId: typeof parsed.preset.id === "string" ? parsed.preset.id : item.itemId,
          importedAt: new Date().toISOString(),
          importedFromPackId: details.pack.id,
          creatorId: packCreatorId,
          creatorHandle: packCreatorHandle,
          republishBlocked: true,
        };
        mergedPresets.push(parsed.preset);
      }
      for (const resource of parsed.resources ?? []) {
        const file = zip.file(`resources/${resource.fileName}`) ?? zip.file(resource.fileName);
        if (!file) {
          continue;
        }
        const key = `${resource.type}:${resource.hash ?? resource.fileName}`;
        if (mergedResources.has(key)) {
          continue;
        }
        mergedResources.set(key, {
          entry: resource,
          bytes: new Uint8Array(await file.async("arraybuffer")),
        });
      }
      for (const blend of parsed.blends ?? []) {
        const blendId = typeof blend.id === "string" ? blend.id : "";
        if (blendId && !mergedBlends.has(blendId)) {
          mergedBlends.set(blendId, blend);
        }
      }
      for (const ref of parsed.tone3000Resources ?? []) {
        const key = `${ref.type}:${ref.id}:${ref.toneId ?? ""}:${ref.modelId ?? ""}`;
        if (!mergedTone3000Resources.has(key)) {
          mergedTone3000Resources.set(key, ref);
        }
      }
      continue;
    }

    if (presetsEntry) {
      const parsed = JSON.parse(await presetsEntry.async("text")) as ItemCollectionArchive;
      mergedPresets.push(...(parsed.presets ?? []).map((preset) => ({
        ...preset,
        name: item.title || preset.name || "Imported Preset",
        toneSharingOrigin: {
          source: "toneSharingApi",
          itemId: item.itemId,
          originalPresetId: typeof preset.id === "string" ? preset.id : item.itemId,
          importedAt: new Date().toISOString(),
          importedFromPackId: details.pack.id,
          creatorId: packCreatorId,
          creatorHandle: packCreatorHandle,
          republishBlocked: true,
        },
      })));
      for (const resource of parsed.resources ?? []) {
        const file = zip.file(`resources/${resource.fileName}`) ?? zip.file(resource.fileName);
        if (!file) {
          continue;
        }
        const key = `${resource.type}:${resource.hash ?? resource.fileName}`;
        if (mergedResources.has(key)) {
          continue;
        }
        mergedResources.set(key, {
          entry: resource,
          bytes: new Uint8Array(await file.async("arraybuffer")),
        });
      }
      for (const blend of parsed.blends ?? []) {
        const blendId = typeof blend.id === "string" ? blend.id : "";
        if (blendId && !mergedBlends.has(blendId)) {
          mergedBlends.set(blendId, blend);
        }
      }
      for (const ref of parsed.tone3000Resources ?? []) {
        const key = `${ref.type}:${ref.id}:${ref.toneId ?? ""}:${ref.modelId ?? ""}`;
        if (!mergedTone3000Resources.has(key)) {
          mergedTone3000Resources.set(key, ref);
        }
      }
      continue;
    }

    mergedPresets.push(await readPresetFromArchive(buffer));
  }

  if (!mergedPresets.length) {
    throw new Error("Pack has no importable presets");
  }

  const outZip = new zipLib();
  const resourcesFolder = outZip.folder("resources");
  for (const resource of mergedResources.values()) {
    resourcesFolder?.file(resource.entry.fileName, resource.bytes);
  }

  const archive: ItemCollectionArchive = {
    formatVersion: 1,
    createdAt: new Date().toISOString(),
    presets: mergedPresets,
    resources: Array.from(mergedResources.values()).map((entry) => entry.entry),
    blends: Array.from(mergedBlends.values()),
    ...(mergedTone3000Resources.size > 0 ? { tone3000Resources: Array.from(mergedTone3000Resources.values()) } : {}),
  };
  outZip.file("presets.json", JSON.stringify(archive, null, 2));

  const safeTitle = (details.pack.title || "tone-sharing-pack").trim().replace(/[^a-z0-9\-_ ]/gi, "").replace(/\s+/g, "-");
  const fileName = `${safeTitle || "tone-sharing-pack"}.zip`;
  return {
    blob: await outZip.generateAsync({ type: "blob" }),
    fileName,
  };
}
