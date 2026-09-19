/**
 * The record of which community packs are installed locally: registering one,
 * reconciling it against the preset library, rendering the Installed tab, and
 * working out what deleting a pack would actually remove.
 *
 * Deletion is planned before it is performed — presets shared with another pack,
 * or resources still referenced elsewhere, have to survive it.
 */

import { postMessage, setAppSetting } from "../bridge.js";
import { clonePreset, uiState } from "../state.js";
import type { Preset, PresetFolder } from "../types.js";
import { escapeHtml } from "../utils.js";
import { renderToneIconButton } from "./actionButtons.js";
import { element } from "./dom.js";
import { getToneSharingHostActions } from "./hostActions.js";
import { clearPackDetail } from "./packDetail.js";
import { browseState, storageKeys, toneSharingState } from "./state.js";
import type { InstalledPackDeletionPlan, InstalledPackMetadata, InstalledPackSource } from "./types.js";

export function normalizeInstalledPackMetadata(raw: unknown): InstalledPackMetadata | null {
  if (!raw || typeof raw !== "object") {
    return null;
  }

  const value = raw as Record<string, unknown>;
  const id = typeof value.id === "string" ? value.id.trim() : "";
  const title = typeof value.title === "string" ? value.title.trim() : "";
  const source = value.source === "zipImport" || value.source === "toneSharingApi" || value.source === "generatedPack"
    ? value.source
    : "zipImport";
  const importedAt = typeof value.importedAt === "string" && value.importedAt
    ? value.importedAt
    : new Date().toISOString();
  const presetIds = Array.isArray(value.presetIds)
    ? value.presetIds.filter((entry): entry is string => typeof entry === "string" && entry.trim().length > 0)
    : [];
  const resources = Array.isArray(value.resources)
    ? value.resources
        .filter((entry) => entry && typeof entry === "object")
        .map((entry) => {
          const resource = entry as Record<string, unknown>;
          return {
            type: typeof resource.type === "string" ? resource.type.trim() : "",
            id: typeof resource.id === "string" ? resource.id.trim() : "",
          };
        })
        .filter((entry) => entry.type.length > 0 && entry.id.length > 0)
    : [];
  const presetSignatures = value.presetSignatures && typeof value.presetSignatures === "object"
    ? Object.fromEntries(
        Object.entries(value.presetSignatures as Record<string, unknown>)
          .filter(([presetId, signature]) => typeof presetId === "string" && presetId.trim().length > 0 && typeof signature === "string" && signature.trim().length > 0)
          .map(([presetId, signature]) => [presetId.trim(), String(signature).trim()])
      )
    : {};

  if (!id || !title) {
    return null;
  }

  return {
    id,
    title,
    source,
    importedAt,
    packId: typeof value.packId === "string" && value.packId ? value.packId : undefined,
    archivePath: typeof value.archivePath === "string" && value.archivePath ? value.archivePath : undefined,
    archiveFileName: typeof value.archiveFileName === "string" && value.archiveFileName ? value.archiveFileName : undefined,
    presetIds: Array.from(new Set(presetIds)),
    presetSignatures,
    resources,
  };
}

export function persistInstalledPacks(): void {
  setAppSetting(storageKeys.installedPacks, toneSharingState.installedPacks);
}

export function reconcileInstalledPacksWithPresetLibrary(persist = false): boolean {
  if (!Array.isArray(toneSharingState.installedPacks) || toneSharingState.installedPacks.length === 0) {
    return false;
  }

  const availablePresetIds = new Set(uiState.presets.map((preset) => preset.id));
  const nextInstalled = toneSharingState.installedPacks
    .map((pack) => {
      const nextPresetIds = pack.presetIds.filter((presetId) => availablePresetIds.has(presetId));
      if (nextPresetIds.length === 0) {
        return null;
      }

      const nextPresetSignatures = Object.fromEntries(
        Object.entries(pack.presetSignatures ?? {}).filter(([presetId]) => nextPresetIds.includes(presetId))
      );

      return {
        ...pack,
        presetIds: nextPresetIds,
        presetSignatures: nextPresetSignatures,
      } as InstalledPackMetadata;
    })
    .filter((pack): pack is InstalledPackMetadata => pack !== null);

  const currentSerialized = JSON.stringify(toneSharingState.installedPacks);
  const nextSerialized = JSON.stringify(nextInstalled);
  if (currentSerialized === nextSerialized) {
    return false;
  }

  toneSharingState.installedPacks = nextInstalled;
  if (persist) {
    persistInstalledPacks();
  }
  return true;
}

export function mergeInstalledPackMetadata(entry: InstalledPackMetadata): void {
  const existingIndex = toneSharingState.installedPacks.findIndex((pack) => pack.id === entry.id);
  if (existingIndex >= 0) {
    toneSharingState.installedPacks[existingIndex] = entry;
  } else {
    toneSharingState.installedPacks.unshift(entry);
  }
  persistInstalledPacks();
}

export function registerInstalledToneSharingPack(entry: InstalledPackMetadata): void {
  mergeInstalledPackMetadata(entry);
  if (browseState.mode === "installed") {
    void renderInstalledPacks();
  }
}

export function registerInstalledToneSharingPackFromImport(info: {
  packId?: string;
  fileName?: string;
  path?: string;
}): void {
  const packId = info.packId?.trim() ?? "";
  const fileName = info.fileName?.trim() || (packId ? `tone-sharing-pack-${packId}.zip` : "tone-sharing-pack.zip");
  const generatedId = packId
    ? `tone-sharing-api:${packId}`
    : `tone-sharing-api:${fileName.toLowerCase()}`;

  registerInstalledToneSharingPack({
    id: generatedId,
    title: fileName.replace(/\.zip$/i, ""),
    source: "toneSharingApi",
    importedAt: new Date().toISOString(),
    packId: packId || undefined,
    archivePath: info.path?.trim() || undefined,
    archiveFileName: fileName,
    presetIds: [],
    presetSignatures: {},
    resources: [],
  });
}

export function formatInstalledSource(source: InstalledPackSource): string {
  if (source === "toneSharingApi") {
    return "Tone Sharing";
  }
  if (source === "generatedPack") {
    return "Generated Pack";
  }
  return "Imported Zip";
}

export function formatInstalledTimestamp(value: string): string {
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return "unknown";
  }
  return parsed.toLocaleString();
}

export async function renderInstalledPacks(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  reconcileInstalledPacksWithPresetLibrary(true);

  clearPackDetail();
  if (!toneSharingState.installedPacks.length) {
    feed.innerHTML = `<div class="tone-sharing-status">No installed packs yet. Import a zip or Tone Sharing pack first.</div>`;
    return;
  }

  const cards = toneSharingState.installedPacks.map((pack) => {
    const subtitle = `${formatInstalledSource(pack.source)} · ${formatInstalledTimestamp(pack.importedAt)}`;
    const details = `${pack.presetIds.length} preset${pack.presetIds.length === 1 ? "" : "s"} · ${pack.resources.length} resource${pack.resources.length === 1 ? "" : "s"}`;
    const archiveDetail = pack.archiveFileName
      ? `<div class="tone-sharing-card-item-description">Archive: ${escapeHtml(pack.archiveFileName)}</div>`
      : "";

    return `
      <div class="tone-sharing-card-item tone-sharing-pack-hero" data-kind="installed" data-id="${escapeHtml(pack.id)}">
        <div class="tone-sharing-card-item-content">
          <div class="tone-sharing-card-item-title">${escapeHtml(pack.title)}</div>
          <div class="tone-sharing-card-item-meta">${escapeHtml(subtitle)}</div>
          <div class="tone-sharing-card-item-description">${escapeHtml(details)}</div>
          ${archiveDetail}
        </div>
        <div class="tone-sharing-card-item-actions">
          ${renderToneIconButton({ kind: "action", value: "delete-installed", icon: "delete", label: "Delete installed pack" })}
        </div>
      </div>
    `;
  });

  feed.innerHTML = `
    <div class="tone-sharing-row">
      <div class="tone-sharing-row-title">Installed Packs</div>
      <div class="tone-sharing-row-track">
        ${cards.join("")}
      </div>
    </div>
  `;
}

export function removePresetIdsFromFolders(folder: PresetFolder, toRemove: Set<string>): void {
  if (Array.isArray(folder.presetIds)) {
    folder.presetIds = folder.presetIds.filter((presetId) => !toRemove.has(presetId));
  }
  if (Array.isArray(folder.children)) {
    folder.children.forEach((child) => removePresetIdsFromFolders(child, toRemove));
  }
}

export function pruneEmptyPresetFolders(folders: PresetFolder[]): PresetFolder[] {
  return folders
    .map((folder) => {
      const nextChildren = pruneEmptyPresetFolders(folder.children ?? []);
      const nextPresetIds = folder.presetIds ?? [];
      return {
        ...folder,
        children: nextChildren,
        presetIds: nextPresetIds,
      };
    })
    .filter((folder) => (folder.presetIds?.length ?? 0) > 0 || (folder.children?.length ?? 0) > 0);
}

export function presetFolderExistsById(folders: PresetFolder[], folderId: string): boolean {
  for (const folder of folders) {
    if (folder.id === folderId) {
      return true;
    }
    if (presetFolderExistsById(folder.children ?? [], folderId)) {
      return true;
    }
  }
  return false;
}

export function collectPresetResourceKeys(preset: Preset): Set<string> {
  const keys = new Set<string>();
  const addKey = (type: unknown, id: unknown): void => {
    if (typeof type !== "string" || typeof id !== "string") {
      return;
    }
    const resolvedType = type.trim();
    const resolvedId = id.trim();
    if (!resolvedType || !resolvedId) {
      return;
    }
    keys.add(`${resolvedType}:${resolvedId}`);
  };

  const graphs = [preset.graph, ...(Array.isArray(preset.scenes) ? preset.scenes.map((scene) => scene.graph) : [])]
    .filter((graph): graph is NonNullable<typeof preset.graph> => Boolean(graph));

  for (const graph of graphs) {
    for (const node of graph.nodes ?? []) {
      for (const res of node.resources ?? []) {
        addKey(res.resourceType ?? res.type, res.resourceId ?? res.id);
      }
    }
  }

  addKey("nam", preset.audioFxModelId);
  addKey("ir", preset.irId);

  for (const attachment of preset.attachments ?? []) {
    const attachmentType = attachment.type === "audiofx"
      ? "nam"
      : attachment.type === "ir"
        ? "ir"
        : attachment.type;
    addKey(attachmentType, attachment.id);
  }

  return keys;
}

export function buildInstalledPackDeletionPlan(pack: InstalledPackMetadata): InstalledPackDeletionPlan {
  const removablePresetIds: string[] = [];
  const preservedPresetIds: string[] = [];
  const missingPresetIds: string[] = [];
  const removablePresetResourceKeys = new Set<string>();

  const uniquePackPresetIds = Array.from(new Set(pack.presetIds));
  for (const presetId of uniquePackPresetIds) {
    const preset = uiState.presetCache.get(presetId) ?? uiState.presets.find((entry) => entry.id === presetId) ?? null;
    if (!preset) {
      // Preset not in UI state — still queue for disk deletion.
      missingPresetIds.push(presetId);
      removablePresetIds.push(presetId);
      continue;
    }

    // All pack presets are removable; the user confirms deletion explicitly.
    removablePresetIds.push(presetId);
    const resourceKeys = collectPresetResourceKeys(preset);
    resourceKeys.forEach((key) => removablePresetResourceKeys.add(key));
  }

  const toRemove = new Set(removablePresetIds);
  const remainingResourceKeys = new Set<string>();
  uiState.presets.forEach((preset) => {
    if (toRemove.has(preset.id)) {
      return;
    }
    collectPresetResourceKeys(preset).forEach((key) => remainingResourceKeys.add(key));
  });

  const uniquePackResources = Array.from(new Map(pack.resources.map((entry) => [`${entry.type}:${entry.id}`, entry])).values());
  const removableResourceEntries = uniquePackResources.filter((entry) => {
    const key = `${entry.type}:${entry.id}`;
    return removablePresetResourceKeys.has(key) && !remainingResourceKeys.has(key);
  });
  const removableResourceKeys = new Set(removableResourceEntries.map((entry) => `${entry.type}:${entry.id}`));
  const preservedResourceEntries = uniquePackResources.filter((entry) => !removableResourceKeys.has(`${entry.type}:${entry.id}`));

  return {
    removablePresetIds,
    preservedPresetIds,
    missingPresetIds,
    removableResourceEntries,
    preservedResourceEntries,
  };
}

export async function deleteInstalledPackById(id: string, planned?: InstalledPackDeletionPlan): Promise<InstalledPackDeletionPlan> {
  const pack = toneSharingState.installedPacks.find((entry) => entry.id === id);
  if (!pack) {
    throw new Error("Installed pack not found");
  }

  const plan = planned ?? buildInstalledPackDeletionPlan(pack);
  const presetIds = plan.removablePresetIds;
  const resourceEntries = plan.removableResourceEntries;

  for (const presetId of presetIds) {
    postMessage({ type: "deletePreset", presetId });
  }

  if (presetIds.length > 0) {
    const toRemove = new Set(presetIds);
    uiState.presets = uiState.presets.filter((preset) => !toRemove.has(preset.id));
    uiState.filteredPresets = uiState.filteredPresets.filter((preset) => !toRemove.has(preset.id));
    presetIds.forEach((presetId) => {
      uiState.presetCache.delete(presetId);
    });

    if (Array.isArray(uiState.presetFolders)) {
      uiState.presetFolders.forEach((folder) => removePresetIdsFromFolders(folder, toRemove));
      uiState.presetFolders = pruneEmptyPresetFolders(uiState.presetFolders);

      const activeFolderId = uiState.activePresetFolderId ?? "__all__";
      const isVirtualFolder = activeFolderId === "__all__" || activeFolderId === "__favorites__" || activeFolderId === "__recents__";
      const resolvedActiveFolderId = isVirtualFolder || presetFolderExistsById(uiState.presetFolders, activeFolderId)
        ? activeFolderId
        : "__all__";
      uiState.activePresetFolderId = resolvedActiveFolderId;

      postMessage({
        type: "setPresetFolders",
        folders: uiState.presetFolders,
        activeFolderId: resolvedActiveFolderId,
      });
    }

    if (uiState.presetFavorites) {
      const nextFavorites = new Set(uiState.presetFavorites);
      presetIds.forEach((presetId) => nextFavorites.delete(presetId));
      uiState.presetFavorites = nextFavorites;
      postMessage({ type: "setPresetFavorites", favorites: Array.from(nextFavorites) });
    }

    if (uiState.presetRatings) {
      const nextRatings = { ...uiState.presetRatings };
      presetIds.forEach((presetId) => {
        delete nextRatings[presetId];
      });
      uiState.presetRatings = nextRatings;
      postMessage({ type: "setPresetRatings", ratings: nextRatings });
    }

    if (uiState.activePresetId && toRemove.has(uiState.activePresetId)) {
      const nextPreset = uiState.presets[0] ?? null;
      if (nextPreset) {
        uiState.activePresetId = nextPreset.id;
        postMessage({ type: "loadPreset", preset: clonePreset(nextPreset), presetId: nextPreset.id });
      } else {
        uiState.activePresetId = null;
      }
    }

    getToneSharingHostActions().populatePresetDropdown();
    getToneSharingHostActions().renderActivePreset();
  }

  if (resourceEntries.length > 0) {
    postMessage({
      type: "cleanupResourceLibrary",
      scope: "all",
      removeFiles: true,
      resources: resourceEntries,
    });
  }

  toneSharingState.installedPacks = toneSharingState.installedPacks.filter((entry) => entry.id !== id);
  reconcileInstalledPacksWithPresetLibrary();
  if (pack.archivePath) {
    postMessage({ type: "deleteImportedToneSharingPack", path: pack.archivePath });
  }
  persistInstalledPacks();
  await renderInstalledPacks();
  return plan;
}
