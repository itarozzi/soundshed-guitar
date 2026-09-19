/**
 * The folder controls in the library popover — create, rename, delete, and
 * moving a preset or a whole folder between parents.
 *
 * The folder *model* lives in ./folders.ts; this is the UI on top of it.
 */

import { generateResourceId } from "../archiveUtils.js";
import { showNotification } from "../notifications.js";
import { presetSearchElement } from "../presets/dom.js";
import { toggleFavoritePreset } from "../presets/favorites.js";
import { ensurePresetFolders, findFolderById, findFolderWithParent, isDescendantFolder, isVirtualPresetFolderId, persistPresetFolders, removePresetFromFolders, sortPresetFoldersAlphabetically } from "../presets/folders.js";
import { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } from "../presets/sorting.js";
import { uiState } from "../state.js";
import type { PresetFolder } from "../types.js";
import { presetFolderDeleteButton, presetFolderRenameButton } from "./dom.js";
import { filterPresets } from "./filter.js";

export function populatePresetFolderSelect(select: HTMLSelectElement | null, selectedId?: string | null): void {
  if (!select) return;

  const folders = sortPresetFoldersAlphabetically(uiState.presetFolders ?? []);
  const options: Array<{ id: string; label: string }> = [
    { id: PRESET_FOLDER_ALL_ID, label: "All Presets" },
  ];

  const buildOptions = (nodes: PresetFolder[], depth: number): void => {
    nodes.forEach((folder) => {
      const indent = "\u00A0".repeat(depth * 2);
      options.push({ id: folder.id, label: `${indent}${folder.name}` });
      if (folder.children?.length) {
        buildOptions(folder.children, depth + 1);
      }
    });
  };

  buildOptions(folders, 0);

  select.innerHTML = options
    .map((option) => `<option value="${option.id}">${option.label}</option>`)
    .join("");

  const resolved = selectedId ?? uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  select.value = resolved || PRESET_FOLDER_ALL_ID;
}

export function applyPresetFoldersFromBackend(folders: PresetFolder[], activeFolderId?: string | null): void {
  uiState.presetFolders = Array.isArray(folders) ? folders : [];
  uiState.activePresetFolderId = activeFolderId ?? PRESET_FOLDER_ALL_ID;
  ensurePresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

export function setActivePresetFolder(folderId: string): void {
  uiState.activePresetFolderId = folderId;
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
  syncPresetFolderToolbarState();
}

export function addPresetToFolder(folderId: string, presetId: string): void {
  const folders = uiState.presetFolders ?? [];
  const folder = findFolderById(folders, folderId);
  if (!folder) {
    return;
  }
  if (!folder.presetIds.includes(presetId)) {
    folder.presetIds.push(presetId);
  }
}

export function movePresetToFolder(presetId: string, folderId: string): void {
  if (folderId === PRESET_FOLDER_FAVORITES_ID) {
    toggleFavoritePreset(presetId);
    return;
  }
  if (folderId === PRESET_FOLDER_RECENTS_ID) {
    return;
  }
  const folders = uiState.presetFolders ?? [];
  removePresetFromFolders(folders, presetId);
  if (folderId !== PRESET_FOLDER_ALL_ID) {
    addPresetToFolder(folderId, presetId);
  }
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

export function movePresetFolder(folderId: string, targetParentId: string): void {
  if (!folderId || isVirtualPresetFolderId(folderId)) {
    return;
  }
  if (targetParentId === PRESET_FOLDER_FAVORITES_ID || targetParentId === PRESET_FOLDER_RECENTS_ID) {
    return;
  }

  const folders = uiState.presetFolders ?? [];
  const result = findFolderWithParent(folders, folderId);
  if (!result) {
    return;
  }

  if (targetParentId && targetParentId !== PRESET_FOLDER_ALL_ID) {
    if (folderId === targetParentId) {
      return;
    }
    if (isDescendantFolder(result.folder, targetParentId)) {
      return;
    }
  }

  if (result.parent) {
    result.parent.children = (result.parent.children ?? []).filter((child) => child.id !== folderId);
  } else {
    uiState.presetFolders = (uiState.presetFolders ?? []).filter((folder) => folder.id !== folderId);
  }

  if (targetParentId && targetParentId !== PRESET_FOLDER_ALL_ID) {
    const targetParent = findFolderById(folders, targetParentId);
    if (!targetParent) {
      return;
    }
    targetParent.children = targetParent.children ?? [];
    targetParent.children.push(result.folder);
  } else {
    uiState.presetFolders = uiState.presetFolders ?? [];
    uiState.presetFolders.push(result.folder);
  }

  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
}

export function createFolder(name: string, parentId?: string): boolean {
  const trimmed = name.trim();
  if (!trimmed) {
    showNotification("Folder name required", "Enter a folder name to create.");
    return false;
  }

  const newFolder: PresetFolder = {
    id: generateResourceId(trimmed),
    name: trimmed,
    children: [],
    presetIds: [],
  };

  if (parentId && parentId !== PRESET_FOLDER_ALL_ID) {
    const parent = findFolderById(uiState.presetFolders ?? [], parentId);
    if (parent) {
      parent.children = parent.children ?? [];
      parent.children.push(newFolder);
    } else {
      uiState.presetFolders?.push(newFolder);
    }
  } else {
    uiState.presetFolders?.push(newFolder);
  }

  persistPresetFolders();
  setActivePresetFolder(newFolder.id);
  showNotification("Folder created", trimmed);
  return true;
}

export function renameFolder(folderId: string, nextName: string): boolean {
  const trimmed = nextName.trim();
  if (!trimmed) {
    showNotification("Folder name required", "Enter a folder name to rename.");
    return false;
  }

  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  if (!folder) {
    showNotification("Folder not found", "Select a valid folder to rename.");
    return false;
  }

  folder.name = trimmed;
  persistPresetFolders();
  filterPresets(presetSearchElement?.value ?? "");
  showNotification("Folder renamed", trimmed);
  return true;
}

export function deleteFolderById(folderId: string): boolean {
  const folders = uiState.presetFolders ?? [];
  const result = findFolderWithParent(folders, folderId);
  if (!result) {
    showNotification("Folder not found", "Select a valid folder to delete.");
    return false;
  }

  if (result.parent) {
    result.parent.children = (result.parent.children ?? []).filter((child) => child.id !== folderId);
  } else {
    uiState.presetFolders = (uiState.presetFolders ?? []).filter((folder) => folder.id !== folderId);
  }

  setActivePresetFolder(PRESET_FOLDER_ALL_ID);
  showNotification("Folder deleted", result.folder.name);
  return true;
}

export function getCurrentRealPresetFolder(): PresetFolder | null {
  const activeFolderId = uiState.activePresetFolderId ?? PRESET_FOLDER_ALL_ID;
  if (isVirtualPresetFolderId(activeFolderId)) {
    return null;
  }
  return findFolderById(uiState.presetFolders ?? [], activeFolderId) ?? null;
}

export function syncPresetFolderToolbarState(): void {
  const activeFolder = getCurrentRealPresetFolder();
  const disabled = !activeFolder;

  if (presetFolderRenameButton) {
    presetFolderRenameButton.disabled = disabled;
  }

  if (presetFolderDeleteButton) {
    presetFolderDeleteButton.disabled = disabled;
  }
}
