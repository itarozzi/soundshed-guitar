/**
 * Resolving a layout image id to something the WebView can actually load.
 *
 * The lookup itself is resolveLayoutImageUrl (layoutImages.ts), shared with the runtime
 * renderer so the designer and the params panel always draw the same file for an id.
 */

import { resolveLayoutImageUrl } from "../layoutImages.js";

export function getLayoutImageUrl(imageId: string): string | null {
  return resolveLayoutImageUrl(imageId);
}

/** Load an image so it can be drawn onto the thumbnail canvas. */
export function loadThumbnailImage(src: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = src;
  });
}
