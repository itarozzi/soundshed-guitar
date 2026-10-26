/**
 * Right-click on a folder in the preset chooser's folder tree, or a long press on
 * a touch screen (touchLongPress.ts). It offers "Create Pack…", which starts a new
 * Tone Sharing draft pack holding the folder's presets (its subfolders' included).
 *
 * The menu hangs from <body> so the popover's clipping can't trap it, which means
 * its position is divided by the app's CSS zoom.
 */

import { switchMainPanel } from "../navigation.js";
import { showNotification } from "../notifications.js";
import { getUiZoom } from "../pointerDrag.js";
import { clonePreset, uiState } from "../state.js";
import { initializeTouchLongPress } from "../touchLongPress.js";
import { ensureToneSharingSignedIn, openFolderPackModal, openToneSharingSignInModal } from "../toneSharingPanel.js";
import type { Preset } from "../types.js";
import { escapeHtml } from "../utils.js";
import { presetFolderTreeElement } from "./dom.js";
import { getPresetsForFolderId } from "./folderArchive.js";
import { findFolderById, isVirtualPresetFolderId } from "./folders.js";
import { requestPresetFromBackend } from "./fetch.js";
import { closePresetLibraryPopover } from "./popover.js";

/** How far inside the window the menu's edges are kept. */
const VIEWPORT_MARGIN_PX = 4;

let menuElement: HTMLElement | null = null;
let listenersBound = false;

function bindDismissListeners(): void {
  if (listenersBound) {
    return;
  }
  listenersBound = true;
  // Capture phase, so a control that stops the event still closes the menu.
  document.addEventListener("pointerdown", (event) => {
    if (menuElement && !menuElement.contains(event.target as Node | null)) {
      closePresetFolderMenu();
    }
  }, true);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      closePresetFolderMenu();
    }
  });
  document.addEventListener("scroll", closePresetFolderMenu, true);
  window.addEventListener("resize", closePresetFolderMenu);
  window.addEventListener("blur", closePresetFolderMenu);
}

/**
 * Listens on the folder tree itself, which outlives the rows it re-renders, so
 * the rows need no listener of their own.
 */
export function initializePresetFolderMenu(): void {
  initializeTouchLongPress();
  presetFolderTreeElement?.addEventListener("contextmenu", (event) => {
    const row = event.target instanceof Element ? event.target.closest<HTMLElement>(".preset-folder-item[data-folder-id]") : null;
    const folderId = row?.dataset.folderId ?? "";
    if (!folderId || isVirtualPresetFolderId(folderId)) {
      return;
    }
    event.preventDefault();
    openPresetFolderMenu(folderId, event.clientX, event.clientY);
  });
}

export function closePresetFolderMenu(): void {
  menuElement?.remove();
  menuElement = null;
}

/** Opens the folder's menu at the pointer. Virtual folders (All, Favourites, Recents) have none. */
export function openPresetFolderMenu(folderId: string, clientX: number, clientY: number): void {
  closePresetFolderMenu();
  if (isVirtualPresetFolderId(folderId)) {
    return;
  }
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  if (!folder) {
    return;
  }
  bindDismissListeners();

  const presetCount = getPresetsForFolderId(folderId).length;
  const menu = document.createElement("div");
  menu.className = "preset-folder-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", `${folder.name} folder`);
  // The compact Controls sheet closes on clicks outside the bar unless they land in one of its popovers.
  menu.setAttribute("data-control-bar-popover", "");
  menu.innerHTML = `
    <div class="preset-folder-menu-title">${escapeHtml(folder.name)}</div>
    <button class="preset-folder-menu-item" type="button" role="menuitem" data-action="createPack"${presetCount === 0 ? " disabled" : ""}>
      <span>Create Pack…</span>
      <span class="preset-folder-menu-hint">${presetCount === 1 ? "1 preset" : `${presetCount} presets`}</span>
    </button>
  `;

  menu.addEventListener("click", (event) => {
    const button = event.target instanceof Element ? event.target.closest<HTMLButtonElement>(".preset-folder-menu-item") : null;
    if (!button || button.disabled) {
      return;
    }
    closePresetFolderMenu();
    if (button.dataset.action === "createPack") {
      void createPackFromFolder(folderId);
    }
  });
  menu.addEventListener("contextmenu", (event) => event.preventDefault());

  // Measured in place, then pulled back inside the window if it would overhang.
  document.body.appendChild(menu);
  const zoom = getUiZoom();
  const { width, height } = menu.getBoundingClientRect();
  const viewWidth = window.innerWidth / zoom;
  const viewHeight = window.innerHeight / zoom;
  const x = clientX / zoom;
  const y = clientY / zoom;
  menu.style.left = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(x, viewWidth - width / zoom - VIEWPORT_MARGIN_PX))}px`;
  menu.style.top = `${Math.max(VIEWPORT_MARGIN_PX, Math.min(y, viewHeight - height / zoom - VIEWPORT_MARGIN_PX))}px`;
  menuElement = menu;
  menu.querySelector<HTMLButtonElement>(".preset-folder-menu-item:not(:disabled)")?.focus();
}

/** The preset with its full graph: the cached copy when it has one, else the engine's. */
async function loadFullPreset(presetId: string): Promise<Preset> {
  const cached = uiState.presetCache.get(presetId);
  if (cached?.graph?.nodes?.length) {
    return clonePreset(cached);
  }
  return requestPresetFromBackend(presetId);
}

async function createPackFromFolder(folderId: string): Promise<void> {
  const folder = findFolderById(uiState.presetFolders ?? [], folderId);
  if (!folder) {
    return;
  }
  const presets = getPresetsForFolderId(folderId);
  closePresetLibraryPopover();

  let signedIn = false;
  try {
    signedIn = await ensureToneSharingSignedIn();
  } catch {
    signedIn = false;
  }
  if (!signedIn) {
    showNotification("Sign in to create a pack", "Packs are shared through Tone Sharing.");
    switchMainPanel("sharing");
    openToneSharingSignInModal();
    return;
  }

  await openFolderPackModal(folder.name, presets, loadFullPreset);
}
