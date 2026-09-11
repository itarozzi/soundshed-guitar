/**
 * Custom effect layouts and the images they are drawn from.
 */

import { layoutDesigner } from "../layoutDesigner.js";
import { areLayoutImagesLoaded, ensureLayoutImagesLoaded, markLayoutImagesLoaded } from "../layoutImages.js";
import { renderLayoutList } from "../layoutManager.js";
import type { LayoutImageRef, LayoutLibrary } from "../layoutTypes.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { renderActivePreset } from "../presets.js";
import { refreshSelectedNodeParams } from "../signalPath.js";
import { uiState } from "../state.js";
import type { IncomingPayload } from "./types.js";

export function onLayoutLibraryLoaded(payload: IncomingPayload): void {
  const libraryPayload = payload as { layoutLibrary?: LayoutLibrary };
  if (libraryPayload.layoutLibrary) {
    // Images are no longer shipped in this payload (loaded on demand). Preserve any
    // images we already received so an open designer keeps its backgrounds, then
    // refresh them if they had been loaded (picks up additions/removals).
    const previousImages = uiState.layoutLibrary?.images ?? [];
    uiState.layoutLibrary = libraryPayload.layoutLibrary;
    if ((!uiState.layoutLibrary.images || uiState.layoutLibrary.images.length === 0) && previousImages.length) {
      uiState.layoutLibrary.images = previousImages;
    }
    if (areLayoutImagesLoaded()) {
      ensureLayoutImagesLoaded(true);
    }
    appendLog("Layout library loaded");
    renderLayoutList();
    // Ensure any open node params panel picks up the updated layout mapping.
    renderActivePreset();
  }
}

export function onLayoutSaved(payload: IncomingPayload): void {
  const savePayload = payload as { effectType?: string; blendId?: string; layoutId?: string; lookupKey?: string };
  const displayKey = savePayload.blendId ? `${savePayload.effectType} (blend: ${savePayload.blendId})` : savePayload.effectType;
  appendLog(`Layout saved for ${displayKey ?? "effect"}${savePayload.layoutId ? ` (${savePayload.layoutId})` : ""}`);
  showNotification("Layout saved");
  // layoutLibraryLoaded will follow and trigger a full refresh.
}

export function onLayoutImagesLoaded(payload: IncomingPayload): void {
  const imagesPayload = payload as { images?: LayoutImageRef[] };
  const images = Array.isArray(imagesPayload.images) ? imagesPayload.images : [];
  if (!uiState.layoutLibrary) {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  }
  uiState.layoutLibrary.images = images;
  markLayoutImagesLoaded();
  appendLog(`Layout images loaded (${images.length})`);
  renderLayoutList();
  // Re-render the designer canvas so backgrounds appear once images arrive.
  layoutDesigner.notifyImagesLoaded();
  // Refresh the live signal-path node params panel so custom-layout backgrounds resolve.
  refreshSelectedNodeParams();
}

export function onLayoutImageSelected(payload: IncomingPayload): void {
  console.log("[Messages] layoutImageSelected received:", payload);
  const imgPayload = payload as { purpose?: string; imageId?: string; fileName?: string; dataUrl?: string; layerIndex?: number; paramKey?: string };
  if (imgPayload.purpose && imgPayload.imageId && imgPayload.fileName) {
    // Add image to layout library so it can be resolved
    if (uiState.layoutLibrary) {
      const existingIdx = uiState.layoutLibrary.images.findIndex(img => img.imageId === imgPayload.imageId);
      const imageEntry = { 
        imageId: imgPayload.imageId, 
        fileName: imgPayload.fileName, 
        dataUrl: imgPayload.dataUrl,
        type: imgPayload.purpose as "background" | "knob" | "general" 
      };
      if (existingIdx >= 0) {
        uiState.layoutLibrary.images[existingIdx] = imageEntry;
      } else {
        uiState.layoutLibrary.images.push(imageEntry);
      }
    }
    layoutDesigner.handleImageSelected(
      imgPayload.purpose,
      imgPayload.imageId,
      imgPayload.layerIndex,
      imgPayload.paramKey
    );
  }
}

export function onLayoutExportSaved(payload: IncomingPayload): void {
  const exportPayload = payload as { path?: string };
  if (exportPayload.path) {
    showNotification(`Layout exported to ${exportPayload.path}`);
    appendLog(`Layout exported: ${exportPayload.path}`);
  }
}

export function onLayoutExportFailed(payload: IncomingPayload): void {
  const failPayload = payload as { message?: string };
  showNotification(failPayload.message ?? "Layout export failed");
  appendLog(`Layout export failed: ${failPayload.message ?? "unknown error"}`);
}
