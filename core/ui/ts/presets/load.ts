/**
 * Loading a preset into the engine: fetching its metadata, resolving the
 * resources it attaches, and the index read at startup.
 */

import { postMessage } from "../bridge.js";
import { REMOTE_BASE_URL, getDefaultPresets } from "../dataLibraries.js";
import { showConfirm } from "../dialogs.js";
import { appendLog } from "../logging.js";
import { clearNotification, showNotification } from "../notifications.js";
import { setFavoriteToggleState } from "../presets/favorites.js";
import { requestPresetFromBackend } from "../presets/fetch.js";
import { stripLegacyGlobals } from "../presets/sanitize.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { clonePreset, setActivePresetDraft, setActivePresetIsNew, setActivePresetSnapshot, setPresetDirty, uiState } from "../state.js";
import type { Attachment, GlobalSignalChainConfig, Preset } from "../types.js";
import { arrayBufferToBase64, isRemoteUrl, resolveAttachmentUrl } from "../utils.js";
import { updatePresetActionButtons } from "./actions.js";
import { recordPresetInHistory } from "./history.js";
import { renderPresetUI, updatePresetDropdownSelection } from "./library.js";
import { hasGraphNodes } from "./validate.js";

export function bindLoadButtons(): void {
  const loadModelBtn = document.getElementById("load-model-btn");
  const loadIRBtn = document.getElementById("load-ir-btn");

  if (loadModelBtn) {
    loadModelBtn.addEventListener("click", () => {
      postMessage({ type: "browseModel" });
      appendLog("browseModel → requested");
    });
  }

  if (loadIRBtn) {
    loadIRBtn.addEventListener("click", () => {
      postMessage({ type: "browseIR" });
      appendLog("browseIR → requested");
    });
  }
}

export function loadModelFromPath(filePath: string): void {
  postMessage({
    type: "loadModel",
    filePath,
  });
  appendLog(`loadModel → ${filePath}`);
}

export function loadIRFromPath(filePath: string): void {
  postMessage({
    type: "loadIR",
    filePath,
  });
  appendLog(`loadIR → ${filePath}`);
}

export function requestSignalPathTest(): void {
  clearNotification();
  postMessage({
    type: "runSignalPathTest",
    frequency: 440,
    duration: 1.0,
  });
}

export async function loadPresetMetadata(presetId: string): Promise<Preset> {
  if (uiState.presetCache.has(presetId)) {
    const cached = stripLegacyGlobals(clonePreset(uiState.presetCache.get(presetId) ?? null) as Preset);
    if (hasGraphNodes(cached)) {
      return cached;
    }
    const backendPreset = await requestPresetFromBackend(presetId);
    const resolved = stripLegacyGlobals(backendPreset);
    uiState.presetCache.set(resolved.id, resolved);
    return clonePreset(resolved) as Preset;
  }

  const localPreset = uiState.presets.find((preset) => preset.id === presetId);
  if (localPreset) {
    const cleaned = stripLegacyGlobals(localPreset);
    uiState.presetCache.set(localPreset.id, cleaned);
    if (!hasGraphNodes(cleaned)) {
      const backendPreset = await requestPresetFromBackend(presetId);
      const resolved = stripLegacyGlobals(backendPreset);
      uiState.presetCache.set(resolved.id, resolved);
      return clonePreset(resolved) as Preset;
    }
    return clonePreset(cleaned) as Preset;
  }

  if (!REMOTE_BASE_URL) {
    throw new Error("Remote preset service is not configured.");
  }

  const baseUrl = REMOTE_BASE_URL.replace(/\/$/, "");
  const response = await fetch(`${baseUrl}/presets/${encodeURIComponent(presetId)}`);
  if (!response.ok) {
    throw new Error(`Failed to fetch preset ${presetId}: ${response.status}`);
  }

  const data = await response.json();
  const preset = Array.isArray(data) ? data[0] : data;
  if (!preset) {
    throw new Error(`Preset ${presetId} not found`);
  }

  const cleaned = stripLegacyGlobals(preset as Preset);
  uiState.presetCache.set(cleaned.id, cleaned);
  return clonePreset(cleaned) as Preset;
}

export async function enrichAttachment(attachment: Attachment): Promise<Attachment> {
  if (attachment.data) {
    return attachment;
  }

  const url = resolveAttachmentUrl(attachment, REMOTE_BASE_URL);
  if (!url || !isRemoteUrl(url)) {
    return attachment;
  }

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to fetch attachment from ${url}`);
  }

  const buffer = await response.arrayBuffer();
  return { ...attachment, data: arrayBufferToBase64(buffer) };
}

export async function applyPresetFromLibrary(presetId: string): Promise<void> {
  if (uiState.presetDirty && uiState.activePresetId && uiState.activePresetId !== presetId) {
    const confirmDiscard = await showConfirm("Discard unsaved changes?", "Unsaved changes");
    if (!confirmDiscard) {
      return;
    }
    setPresetDirty(false);
  }
  try {
    clearNotification();
    const preset = await loadPresetMetadata(presetId);
    const attachments = await Promise.all((preset.attachments ?? []).map(enrichAttachment));
    const presetWithGlobals = preset as Preset & { globalSignalChain?: GlobalSignalChainConfig };
    const hasGlobalChain = Boolean(presetWithGlobals.globalSignalChain);
    const resolvedChain = hasGlobalChain
      ? JSON.parse(JSON.stringify(presetWithGlobals.globalSignalChain)) as GlobalSignalChainConfig
      : null;
    const presetPayload: Preset = {
      ...stripLegacyGlobals(preset),
      attachments,
      ...(hasGlobalChain && resolvedChain ? { globalSignalChain: resolvedChain } : {}),
    };
    const sceneId = normalizePresetScenes(presetPayload, uiState.activePresetSceneId ?? undefined);
    uiState.activePresetSceneId = sceneId;

    if (hasGlobalChain && resolvedChain) {
      uiState.globalSignalChain = resolvedChain;
    }
    uiState.presetCache.set(presetPayload.id, clonePreset(presetPayload));
    uiState.activePresetId = presetPayload.id;
    setActivePresetIsNew(false);
    setActivePresetSnapshot(presetPayload);
    setActivePresetDraft(presetPayload);
    setPresetDirty(false);
    setFavoriteToggleState(presetPayload.id);
    updatePresetDropdownSelection();
    // Set loading state BEFORE rendering so all render functions (list, details,
    // signal path bar) see it and bake the loading class/overlay into their output.
    uiState.presetLoadingId = presetPayload.id;
    renderPresetUI(clonePreset(presetPayload));
    updatePresetActionButtons();
    postMessage({
      type: "loadPreset",
      preset: presetPayload,
      ...(sceneId ? { sceneId } : {}),
    });
    recordPresetInHistory(presetPayload.id);
  } catch (error) {
    uiState.presetLoadingId = null;
    console.error("Failed to apply preset", error);
    showNotification("Failed to apply preset", error instanceof Error ? error.message : "Unknown error");
  }
}

export async function loadPresetIndex(): Promise<void> {
  try {
    if (!REMOTE_BASE_URL) {
      throw new Error("Remote preset service disabled");
    }

    const response = await fetch(`${REMOTE_BASE_URL.replace(/\/$/, "")}/presets`);
    if (!response.ok) {
      throw new Error(`Failed to fetch presets index: ${response.status}`);
    }

    const data = await response.json();
    const presets = Array.isArray(data) ? data : data.presets ?? [];
    const basePresets = presets.length ? presets : getDefaultPresets();
    uiState.presets = [...basePresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  } catch (error) {
    console.error("Failed to load preset index", error);
    const basePresets = getDefaultPresets();
    uiState.presets = [...basePresets];
    uiState.filteredPresets = uiState.presets.slice();
    uiState.presets.forEach((preset) => {
      uiState.presetCache.set(preset.id, preset);
    });
    renderPresetUI(uiState.presetCache.get(uiState.activePresetId ?? "") ?? null);
  }
}
