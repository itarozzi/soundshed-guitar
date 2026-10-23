/**
 * Layout images: the list of them, requested on demand, and where each one loads from.
 *
 * The engine lists images by URL (`layout-images/id/<id>`), served by the editor's resource
 * provider, so the list is small; it still arrives separately from `layoutLibraryLoaded`, the
 * first time something draws a layout, and is refreshed after the library reloads.
 */

import { postMessage } from "./bridge.js";
import { uiState } from "./state.js";

/**
 * Where image `imageId` loads from, or null when the library does not hold it. A data URL
 * wins where there is one (an image imported this session, an older layout's embedded data):
 * it is the image itself.
 */
export function resolveLayoutImageUrl(imageId: string): string | null {
  const image = uiState.layoutLibrary?.images.find((img) => img.imageId === imageId);
  if (!image) {
    return null;
  }
  return image.dataUrl || image.url || null;
}

let loaded = false;
let pending = false;

/**
 * Request the layout image set if it has not been loaded yet.
 * @param force Re-request even if already loaded (used after a library reload).
 */
export function ensureLayoutImagesLoaded(force = false): void {
  if (pending) return;
  if (loaded && !force) return;
  pending = true;
  postMessage({ type: "requestLayoutImages" });
}

/** Called when a `layoutImagesLoaded` message arrives. */
export function markLayoutImagesLoaded(): void {
  pending = false;
  loaded = true;
}

/** Whether the layout image set has been received at least once. */
export function areLayoutImagesLoaded(): boolean {
  return loaded;
}
