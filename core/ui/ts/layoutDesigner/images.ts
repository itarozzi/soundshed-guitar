/**
 * Resolving a layout image id to something the WebView can actually load.
 *
 * A base64 data URL is preferred where one exists: the designer draws onto a
 * canvas, and a file path would have to survive the layout being exported and
 * imported on another machine.
 */

import { uiState } from "../state.js";

export function getLayoutImageUrl(imageId: string): string | null {
  const image = uiState.layoutLibrary?.images.find((img) => img.imageId === imageId);
  if (image) {
    if (image.dataUrl) {
      return image.dataUrl;
    }
    if (image.fileName) {
      return `layout-images/${image.fileName}`;
    }
  }
  return null;
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
