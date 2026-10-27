/**
 * Installing a downloaded tone into the library, and taking one out again. The engine does
 * both ("installPresetArchives", "deleteInstalledPresetArchive"; PluginControllerArchiveInstall.cpp),
 * as it does for Nano Q, and records the install in toneSharing.installedPacks.
 *
 * What stays here is what only this UI can do: reading each download for the Tone3000 models it
 * names but does not carry and fetching those with the user's key or the proxy, asking before a
 * pack goes in, loading the preset installed, and saying how it went.
 */

import { postMessage } from "../bridge.js";
import { showAlert, showConfirm } from "../dialogs.js";
import type { IncomingPayload } from "../messages/types.js";
import { showNotification } from "../notifications.js";
import { setActivePresetId } from "../presetLibraryStore.js";
import type { RemoteResourceRef } from "../remoteResources.js";
import { getLibraryResource } from "../resourceLibrary.js";
import { arrayBufferToBase64 } from "../utils.js";
import { getToneSharingHostActions } from "./hostActions.js";
import type { ToneSharingHostActions } from "./hostActions.js";

/** One download: a preset archive (zip), or a bare preset JSON as some items are. */
export type DownloadedArchive = { title: string; data: ArrayBuffer };

/** What `installPresetArchives` records the install under (the installedPacks schema). */
export type InstallEntry = { id: string; title: string; source: "toneSharingApi"; packId?: string; archiveFileName?: string };

const INSTALL_TIMEOUT_MS = 300_000;
const DELETE_TIMEOUT_MS = 60_000;

type Pending = { resolve: (payload: Record<string, unknown>) => void; reject: (error: Error) => void; timer: ReturnType<typeof setTimeout> };
const pending = new Map<string, Pending>();
let nextRequestId = 0;

function awaitReply(key: string, timeoutMs: number, send: () => void): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(key);
      reject(new Error("Timed out waiting for the engine"));
    }, timeoutMs);
    pending.set(key, { resolve, reject, timer });
    try {
      send();
    } catch (error) {
      clearTimeout(timer);
      pending.delete(key);
      reject(error instanceof Error ? error : new Error(String(error)));
    }
  });
}

function settle(key: string, payload: Record<string, unknown>, error?: Error): void {
  const entry = pending.get(key);
  if (!entry) {
    return;
  }
  clearTimeout(entry.timer);
  pending.delete(key);
  if (error) {
    entry.reject(error);
  } else {
    entry.resolve(payload);
  }
}

export function onPresetArchivesInstalled(payload: IncomingPayload): void {
  settle(`install:${typeof payload.requestId === "string" ? payload.requestId : ""}`, payload);
}

export function onPresetArchivesInstallFailed(payload: IncomingPayload): void {
  const reply = payload as { requestId?: string; message?: string; detail?: string };
  settle(`install:${reply.requestId ?? ""}`, {}, new Error(reply.detail || reply.message || "Install failed"));
}

export function onInstalledPresetArchiveDeleted(payload: IncomingPayload): void {
  settle(`delete:${typeof payload.id === "string" ? payload.id : ""}`, payload);
}

function stringList(value: unknown): string[] {
  return Array.isArray(value) ? value.filter((entry): entry is string => typeof entry === "string") : [];
}

type ReadArchive = { title: string; base64: string; presets: number; resources: number; blends: number; remoteRefs: RemoteResourceRef[] };

async function readDownload(download: DownloadedArchive): Promise<ReadArchive> {
  const zipLib = window.JSZip;
  if (!zipLib) {
    throw new Error("Archive library not available");
  }

  const bytes = new Uint8Array(download.data);
  let buffer = download.data;
  let root: Record<string, unknown>;
  if (bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b) {
    const zip = await zipLib.loadAsync(buffer);
    const entry = zip.file("preset.json") ?? zip.file("presets.json");
    if (!entry) {
      throw new Error(`"${download.title}" is not a preset archive`);
    }
    root = JSON.parse(await entry.async("text")) as Record<string, unknown>;
  } else {
    // A bare preset: wrapped as the one-preset archive the engine reads.
    const parsed = JSON.parse(new TextDecoder().decode(bytes)) as Record<string, unknown>;
    root = { formatVersion: 1, preset: (parsed.preset as Record<string, unknown> | undefined) ?? parsed, resources: [], blends: [] };
    const zip = new zipLib();
    zip.file("preset.json", JSON.stringify(root));
    buffer = await (await zip.generateAsync({ type: "blob" })).arrayBuffer();
  }

  const count = (key: string): number => (Array.isArray(root[key]) ? (root[key] as unknown[]).length : 0);
  return {
    title: download.title,
    base64: arrayBufferToBase64(buffer),
    presets: root.preset ? 1 : count("presets"),
    resources: count("resources"),
    blends: count("blends"),
    remoteRefs: getToneSharingHostActions().readArchiveRemoteRefs(root as Parameters<ToneSharingHostActions["readArchiveRemoteRefs"]>[0]),
  };
}

function isInLibrary(ref: RemoteResourceRef): boolean {
  const existing = getLibraryResource(ref.type, ref.id);
  return Boolean(existing && !existing.fileMissing);
}

/**
 * Installs downloaded archives as one entry (a pack with `folder`, its presets in a folder of
 * that name). With `confirm`, the user is asked first, as for a pack. Resolves with the new
 * presets' ids, or null when the user cancelled; rejects with the engine's reason on failure.
 */
export async function installDownloadedArchives(request: {
  entry: InstallEntry;
  folder?: string;
  downloads: DownloadedArchive[];
  confirm?: boolean;
}): Promise<string[] | null> {
  const archives = await Promise.all(request.downloads.map(readDownload));
  const refs = new Map<string, RemoteResourceRef>();
  archives.forEach((archive) => archive.remoteRefs.forEach((ref) => refs.set(ref.id, ref)));

  if (request.confirm) {
    const message = getToneSharingHostActions().buildImportSummaryMessage({
      format: "presetArchive",
      title: request.entry.title,
      presetCount: archives.reduce((sum, archive) => sum + archive.presets, 0),
      resourceCount: archives.reduce((sum, archive) => sum + archive.resources, 0),
      blendCount: archives.reduce((sum, archive) => sum + archive.blends, 0),
      tone3000ResourceCount: [...refs.values()].filter((ref) => ref.provider === "tone3000").length,
      packId: request.entry.packId,
    });
    if (!(await showConfirm(message, "Import Pack"))) {
      return null;
    }
  }

  // The engine installs only what the library holds, so the models the archives name but do not
  // carry are fetched first. One that cannot be had leaves its slot empty (allowMissingRemote).
  const remoteRefs = [...refs.values()];
  const missingBefore = new Set(remoteRefs.filter((ref) => !isInLibrary(ref)).map((ref) => `${ref.type}:${ref.id}`));
  const brought: Array<{ type: string; id: string }> = [];
  const unavailable = remoteRefs.length > 0
    ? await getToneSharingHostActions().importArchiveRemoteResources(remoteRefs, new Map(), brought)
    : [];
  const fetchedResources = brought.filter((resource) => missingBefore.has(`${resource.type}:${resource.id}`));

  const requestId = `tone-install-${++nextRequestId}`;
  const reply = await awaitReply(`install:${requestId}`, INSTALL_TIMEOUT_MS, () => postMessage({
    type: "installPresetArchives",
    requestId,
    entry: request.entry,
    ...(request.folder ? { folder: request.folder } : {}),
    archives: archives.map((archive) => ({ title: archive.title, data: archive.base64 })),
    allowMissingRemote: true,
    fetchedResources,
  }));

  const presetIds = stringList(reply.presetIds);
  const latestId = presetIds[presetIds.length - 1];
  if (latestId) {
    // The engine leaves the running preset alone; the last one installed is the one shown.
    setActivePresetId(latestId);
    postMessage({ type: "loadPreset", presetId: latestId });
  }

  const name = request.downloads.length === 1 ? request.downloads[0].title : request.entry.title;
  const summary = presetIds.length === 1 ? `"${name}" imported` : `${presetIds.length} presets imported`;
  const missing = [...new Set([...unavailable, ...stringList(reply.missingRemote)])];
  if (missing.length > 0) {
    // A toast is gone before a list of names can be read.
    void showAlert(
      `${summary}, but ${missing.length === 1 ? "this model" : "these models"} could not be downloaded `
        + `and ${missing.length === 1 ? "its slot is" : "their slots are"} left empty: ${missing.join(", ")}. `
        + "A model goes missing when its creator removes, replaces or hides it.",
      "Some models are missing",
    );
  } else {
    showNotification(presetIds.length === 1 ? "Preset imported" : "Presets imported", presetIds.length === 1 ? name : summary);
  }
  return presetIds;
}

/** Has the engine take an install out of the library. Resolves with the presets it removed. */
export async function requestInstalledPackRemoval(id: string): Promise<string[]> {
  const reply = await awaitReply(`delete:${id}`, DELETE_TIMEOUT_MS, () => postMessage({ type: "deleteInstalledPresetArchive", id }));
  return stringList(reply.presetIds);
}
