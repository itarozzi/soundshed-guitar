/**
 * Layout images: the list of them, requested on demand, and where each one loads from.
 *
 * The engine lists images by URL (`layout-images/id/<id>`), served by the editor's resource
 * provider, so the list is small; it still arrives separately from `layoutLibraryLoaded`, the
 * first time something draws a layout, and is refreshed after the library reloads.
 */

import { postMessage } from "./bridge.js";
import { ensureLayoutImageRenditions, missingRenditionEdges } from "./layoutRenditionMaker.js";
import { backgroundDrawnWidth, chooseRenditionUrl } from "./layoutRenditions.js";
import { layoutCanGrow } from "./layoutScaling.js";
import type { EffectLayout, LayoutBackground } from "./layoutTypes.js";
import { uiState } from "./state.js";

/**
 * Where image `imageId` loads from, or null when the library does not hold it. A data URL
 * wins where there is one (an image imported this session, an older layout's embedded data):
 * it is the image itself.
 *
 * Given `drawnWidth`, the CSS pixels across it is drawn at its largest, a large image is drawn
 * from its smallest copy that still has a pixel for every device pixel (layoutRenditions.ts); an
 * image with no copies is drawn from its own file. A large image without its copies starts
 * having them made.
 */
export function resolveLayoutImageUrl(imageId: string, drawnWidth?: number): string | null {
  const image = uiState.layoutLibrary?.images.find((img) => img.imageId === imageId);
  if (!image) {
    return null;
  }
  if (missingRenditionEdges(image).length) {
    void ensureLayoutImageRenditions(image);
  }
  if (drawnWidth !== undefined) {
    const pixelRatio = typeof window !== "undefined" && window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
    return chooseRenditionUrl(image, drawnWidth, pixelRatio);
  }
  return image.dataUrl || image.url || null;
}

/** How large a layout is ever drawn relative to its own size: its maxScale if it may grow, else 1. */
export function layoutLargestScale(layout: Pick<EffectLayout, "scaling">): number {
  return layoutCanGrow(layout.scaling) ? Math.min(8, layout.scaling!.maxScale!) : 1;
}

/** CSS pixels across which a background layer's image is drawn at the layout's largest. */
export function backgroundImageDrawnWidth(bg: LayoutBackground, layout: Pick<EffectLayout, "dimensions" | "scaling">): number | undefined {
  const image = uiState.layoutLibrary?.images.find((img) => img.imageId === bg.value);
  if (!image?.width || !image.height) return undefined;
  const { width, height } = layout.dimensions;
  return backgroundDrawnWidth(bg.size, bg.scale, width, height, image.width, image.height) * layoutLargestScale(layout);
}

/** CSS pixels across which a custom knob face is drawn at the layout's largest (38 px knob, 4 px inset). */
export function knobFaceDrawnWidth(layout: Pick<EffectLayout, "scaling">): number {
  return 30 * layoutLargestScale(layout);
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
