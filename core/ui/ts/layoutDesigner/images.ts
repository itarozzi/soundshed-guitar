/**
 * Resolving a layout image id to something the WebView can actually load.
 *
 * The lookup itself is resolveLayoutImageUrl (layoutImages.ts), shared with the runtime
 * renderer so the designer and the params panel always draw the same file for an id.
 */

import { resolveLayoutImageUrl } from "../layoutImages.js";
import type { LayoutImageRef } from "../layoutTypes.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";

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

/**
 * The library images a picker offers for a background or a knob: those of that kind first, then
 * the one already chosen, then the rest, each group by name.
 */
export function reusableLayoutImages(purpose: "background" | "knob", selectedImageId?: string): LayoutImageRef[] {
  const images = uiState.layoutLibrary?.images ?? [];
  const available = images.filter((img) => Boolean(img.imageId) && (Boolean(img.dataUrl) || Boolean(img.url) || Boolean(img.fileName)));
  const preferredType = purpose === "background" ? "background" : "knob";
  const compatible = available.filter((img) => img.imageId === selectedImageId || !img.type || img.type === "general" || img.type === preferredType);
  return [...compatible].sort((a, b) => {
    const aPriority = a.type === preferredType ? 0 : a.imageId === selectedImageId ? 1 : 2;
    const bPriority = b.type === preferredType ? 0 : b.imageId === selectedImageId ? 1 : 2;
    if (aPriority !== bPriority) {
      return aPriority - bPriority;
    }
    return (a.fileName || a.imageId).toLowerCase().localeCompare((b.fileName || b.imageId).toLowerCase());
  });
}

/**
 * The <option> list for an image picker, each image with its kind and pixel size where known.
 * Its options are marked data-layout-image, which is what enhanceImageSelects (imagePicker.ts) looks
 * for to show the list as thumbnails, an empty library's list included.
 */
export function imageOptionsHtml(purpose: "background" | "knob", selectedImageId?: string): string {
  const options = [
    `<option value="" data-layout-image>Select existing image...</option>`,
    ...reusableLayoutImages(purpose, selectedImageId).map((img) => {
      const selected = selectedImageId && img.imageId === selectedImageId ? "selected" : "";
      const typeLabel = img.type ? ` [${img.type}]` : "";
      const sizeLabel = img.width && img.height ? ` ${img.width}×${img.height}` : "";
      // Both can come from an imported archive's manifest.
      const name = img.fileName || img.imageId;
      return `<option value="${escapeHtml(img.imageId)}" data-layout-image ${selected}>${escapeHtml(name)}${escapeHtml(typeLabel + sizeLabel)}</option>`;
    }),
  ];
  return options.join("");
}
