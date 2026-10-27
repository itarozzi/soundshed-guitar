/**
 * The record of which community packs are installed locally: registering one
 * (a pack imported from a file), reconciling it against the preset library,
 * rendering the Installed tab, and asking the engine to remove one. The engine
 * writes the entries of what it installs (install.ts), and decides what removing
 * one takes with it.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { postMessage } from "../bridge.js";
import { uiState } from "../state.js";
import { setActivePresetId } from "../presetLibraryStore.js";
import type { AppSettingValue } from "../types.js";
import { escapeHtml } from "../utils.js";
import { renderToneIconButton } from "./actionButtons.js";
import { element } from "./dom.js";
import { getToneSharingHostActions } from "./hostActions.js";
import { requestInstalledPackRemoval } from "./install.js";
import { clearPackDetail } from "./packDetail.js";
import { browseState, storageKeys, toneSharingState } from "./state.js";
import type { InstalledPackMetadata, InstalledPackSource } from "./types.js";

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
  // The blends the engine's install added: kept so that writing the list back does not lose them.
  const blends = Array.isArray(value.blends)
    ? value.blends.filter((entry): entry is string => typeof entry === "string" && entry.length > 0)
    : undefined;

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
    ...(blends ? { blends } : {}),
  };
}

/** Takes the installed list from the app setting the engine keeps (after it installed or removed one). */
export function syncInstalledPacksFromAppSettings(): void {
  const persisted = uiState.appSettings?.[storageKeys.installedPacks];
  toneSharingState.installedPacks = Array.isArray(persisted)
    ? persisted
        .map((entry) => normalizeInstalledPackMetadata(entry))
        .filter((entry): entry is InstalledPackMetadata => entry !== null)
    : [];
}

export function persistInstalledPacks(): void {
  updateAppSetting(storageKeys.installedPacks, toneSharingState.installedPacks as unknown as AppSettingValue);
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

/**
 * Has the engine take an installed pack out ("deleteInstalledPresetArchive"): its presets, and
 * them out of folders, favourites and ratings; the blends, models and IRs it added that nothing
 * else uses; and its entry. Resolves with the presets removed.
 */
export async function deleteInstalledPackById(id: string): Promise<string[]> {
  const pack = toneSharingState.installedPacks.find((entry) => entry.id === id);
  if (!pack) {
    throw new Error("Installed pack not found");
  }

  const removed = await requestInstalledPackRemoval(id);

  // The engine goes on playing a removed preset until another loads; the UI moves on to one.
  if (uiState.activePresetId && removed.includes(uiState.activePresetId)) {
    const nextPreset = uiState.presets[0] ?? null;
    setActivePresetId(nextPreset?.id ?? null);
    if (nextPreset) {
      postMessage({ type: "loadPreset", presetId: nextPreset.id });
    }
  }
  getToneSharingHostActions().populatePresetDropdown();
  getToneSharingHostActions().renderActivePreset();

  // An entry from before the engine installed packs may name an archive kept on disk.
  if (pack.archivePath) {
    postMessage({ type: "deleteImportedToneSharingPack", path: pack.archivePath });
  }

  syncInstalledPacksFromAppSettings();
  toneSharingState.installedPacks = toneSharingState.installedPacks.filter((entry) => entry.id !== id);
  await renderInstalledPacks();
  return removed;
}
