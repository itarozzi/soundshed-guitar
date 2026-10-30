/**
 * Custom effect layouts and the images they are drawn from.
 */

import { layoutDesigner } from "../layoutDesigner.js";
import { completeImagePickerBrowse } from "../layoutDesigner/imagePicker.js";
import { isLayoutImageCategory } from "../layoutImageCategories.js";
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
  // The designer says "Layout saved" and closes once its own save is confirmed.
  layoutDesigner.handleSaveResult({ layoutId: savePayload.layoutId, ok: true });
  // layoutLibraryLoaded will follow and trigger a full refresh.
}

export function onLayoutSaveFailed(payload: IncomingPayload): void {
  const failPayload = payload as { layoutId?: string; message?: string };
  appendLog(`Layout save failed${failPayload.layoutId ? ` (${failPayload.layoutId})` : ""}: ${failPayload.message ?? "unknown error"}`);
  if (!layoutDesigner.handleSaveResult({ layoutId: failPayload.layoutId, ok: false, message: failPayload.message })) {
    showNotification(`Layout not saved: ${failPayload.message ?? "the engine could not write it"}`);
  }
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
  const imgPayload = payload as {
    purpose?: string;
    imageId?: string;
    fileName?: string;
    url?: string;
    width?: number;
    height?: number;
    bytes?: number;
    version?: string;
    renditions?: LayoutImageRef["renditions"];
    /** The category the engine filed it under; the message's own "type" is taken. */
    category?: string;
    factory?: boolean;
    /** Set when an image picker's Browse asked for it: the image goes back to that picker. */
    pickerId?: string;
    layerIndex?: number;
    paramKey?: string;
  };
  if (imgPayload.purpose && imgPayload.imageId && imgPayload.fileName) {
    // Add image to layout library so it can be resolved. The engine answers with an image the
    // library already has when the picked file is identical to it; that entry keeps its copies.
    if (uiState.layoutLibrary) {
      const existingIdx = uiState.layoutLibrary.images.findIndex(img => img.imageId === imgPayload.imageId);
      const existing = existingIdx >= 0 ? uiState.layoutLibrary.images[existingIdx] : undefined;
      const imageEntry: LayoutImageRef = {
        ...existing,
        imageId: imgPayload.imageId,
        fileName: imgPayload.fileName,
        url: imgPayload.url,
        width: imgPayload.width,
        height: imgPayload.height,
        bytes: imgPayload.bytes,
        version: imgPayload.version ?? existing?.version,
        renditions: imgPayload.renditions ?? existing?.renditions,
        type: isLayoutImageCategory(imgPayload.category) ? imgPayload.category : existing?.type,
        factory: imgPayload.factory ?? existing?.factory,
      };
      if (existingIdx >= 0) {
        uiState.layoutLibrary.images[existingIdx] = imageEntry;
      } else {
        uiState.layoutLibrary.images.push(imageEntry);
      }
    }
    if (imgPayload.pickerId && completeImagePickerBrowse(imgPayload.pickerId, imgPayload.imageId)) {
      return;
    }
    layoutDesigner.handleImageSelected(
      imgPayload.purpose,
      imgPayload.imageId,
      imgPayload.layerIndex,
      imgPayload.paramKey
    );
  }
}

/** A smaller copy of a large image was kept: list it, so the next draw can use it. */
export function onLayoutImageRenditionSaved(payload: IncomingPayload): void {
  const saved = payload as { imageId?: string; edge?: number; width?: number; height?: number; url?: string };
  const image = uiState.layoutLibrary?.images.find((img) => img.imageId === saved.imageId);
  if (!image || typeof saved.edge !== "number" || !saved.url || !saved.width || !saved.height) {
    return;
  }
  const rendition = { edge: saved.edge, width: saved.width, height: saved.height, url: saved.url };
  image.renditions = [...(image.renditions ?? []).filter((r) => r.edge !== rendition.edge), rendition]
    .sort((a, b) => a.edge - b.edge);
  appendLog(`Layout image copy kept: ${saved.imageId} at ${saved.edge} px`);
  // The largest copy arrives last; draw again once it is in.
  if (!image.width || !image.height || rendition.edge >= Math.max(...image.renditions.map((r) => r.edge))) {
    refreshSelectedNodeParams();
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
