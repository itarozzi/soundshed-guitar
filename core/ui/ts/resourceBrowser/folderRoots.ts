/**
 * The folder roots the user has added, and where each effect role was last
 * browsing inside them.
 *
 * All of this lives in app settings rather than on the tab, because it has to
 * outlive the modal: reopening the picker for an IR Cab should land back where
 * that node type was, not wherever some other node type left the shared
 * browser. The context key is what keeps those apart, so it is a parameter
 * here rather than state.
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";
import type { AppSettingValue } from "../types.js";
import { FOLDER_ACTIVE_ROOT_SETTING, FOLDER_LAST_LOCATIONS_SETTING, FOLDER_ROOTS_SETTING } from "./settings.js";
import type { FolderLocation, FolderRoot } from "./types.js";

export function getFolderRoots(): FolderRoot[] {
  const raw = uiState.appSettings?.[FOLDER_ROOTS_SETTING];
  if (!Array.isArray(raw)) return [];
  const roots: FolderRoot[] = [];
  for (const item of raw as unknown[]) {
    if (!item || typeof item !== "object") continue;
    const entry = item as { id?: unknown; label?: unknown; path?: unknown };
    if (typeof entry.id !== "string" || typeof entry.path !== "string") continue;
    const label = typeof entry.label === "string" ? entry.label : entry.path;
    roots.push({ id: entry.id, label, path: entry.path });
  }
  return roots;
}

export function setFolderRoots(roots: FolderRoot[]): void {
  const serialized = roots as unknown as AppSettingValue;
  updateAppSetting(FOLDER_ROOTS_SETTING, serialized);
}

export function getActiveRootId(): string {
  const raw = uiState.appSettings?.[FOLDER_ACTIVE_ROOT_SETTING];
  return typeof raw === "string" ? raw : "";
}

export function setActiveRootId(id: string): void {
  updateAppSetting(FOLDER_ACTIVE_ROOT_SETTING, id);
}

export function getActiveRoot(): FolderRoot | null {
  const roots = getFolderRoots();
  if (!roots.length) return null;
  const activeId = getActiveRootId();
  return roots.find((r) => r.id === activeId) ?? roots[0];
}

export function getFolderLastLocations(): Record<string, FolderLocation> {
  const raw = uiState.appSettings?.[FOLDER_LAST_LOCATIONS_SETTING];
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return {};
  const locations: Record<string, FolderLocation> = {};
  for (const [key, value] of Object.entries(raw as Record<string, unknown>)) {
    if (!value || typeof value !== "object") continue;
    const entry = value as { rootId?: unknown; path?: unknown };
    if (typeof entry.rootId !== "string" || typeof entry.path !== "string") continue;
    locations[key] = { rootId: entry.rootId, path: entry.path };
  }
  return locations;
}

export function setFolderLastLocations(locations: Record<string, FolderLocation>): void {
  const serialized = locations as unknown as AppSettingValue;
  updateAppSetting(FOLDER_LAST_LOCATIONS_SETTING, serialized);
}

/// Records where this effect role was last browsing, so the next IR Cab (or
/// NAM Amp, etc.) picker reopens there rather than wherever any other node type
/// happened to leave the shared folder browser.
export function rememberFolderLocation(contextKey: string, path: string): void {
  if (!path) return;
  const root = getActiveRoot();
  if (!root) return;

  const locations = getFolderLastLocations();
  const existing = locations[contextKey];
  if (existing && existing.rootId === root.id && existing.path === path) {
    return;
  }
  locations[contextKey] = { rootId: root.id, path };
  setFolderLastLocations(locations);
}

export function normalizeFolderPath(value: string): string {
  return value.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
}

export function isPathWithinRoot(path: string, rootPath: string): boolean {
  const normalizedPath = normalizeFolderPath(path);
  const normalizedRoot = normalizeFolderPath(rootPath);
  if (!normalizedPath || !normalizedRoot) return false;
  return normalizedPath === normalizedRoot || normalizedPath.startsWith(`${normalizedRoot}/`);
}

/// Where the folder tab should open for the current effect role: the remembered
/// location if it still lives under a known root, otherwise the active root.
export function resolveFolderStartLocation(contextKey: string): { root: FolderRoot | null; path: string } {
  const roots = getFolderRoots();
  if (!roots.length) {
    return { root: null, path: "" };
  }

  const remembered = getFolderLastLocations()[contextKey];
  if (remembered) {
    const rememberedRoot = roots.find((r) => r.id === remembered.rootId);
    if (rememberedRoot) {
      const path = isPathWithinRoot(remembered.path, rememberedRoot.path)
        ? remembered.path
        : rememberedRoot.path;
      return { root: rememberedRoot, path };
    }
  }

  const activeRoot = getActiveRoot() ?? roots[0];
  return { root: activeRoot, path: activeRoot.path };
}
