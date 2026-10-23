/**
 * Smaller copies of large layout images, and choosing which copy to draw.
 *
 * A 6000 x 4000 background drawn in a 600 px panel would otherwise be decoded at full size
 * (about 96 MB) and resampled on every paint. When an image is added to a layout the designer
 * makes copies with a longest edge of 1024, 2048 and 4096 px (only those smaller than the
 * original), and the engine keeps them beside the original in images/renditions/. Drawing then
 * picks the smallest copy that still has a pixel for every device pixel it covers.
 *
 * An image with no copies, an SVG, or one whose size the engine could not read is drawn from
 * its original, as every layout image always was. See docs/plans/layout-designer-hires.md §3.
 */

/** Longest-edge sizes a copy is made at, smallest first. */
export const RENDITION_EDGES = [1024, 2048, 4096] as const;

/**
 * Copies are made only of images longer than this on their longest edge. Smaller images are drawn
 * from their own file, pixel for pixel as they always were; copies are for the very large art
 * this was built for, where decoding the original for a small panel is the cost to avoid.
 */
export const RENDITION_MIN_SOURCE_EDGE = 2048;

/** One smaller copy of an image, as the engine lists it. */
export interface LayoutImageRendition {
  /** Longest edge in pixels. */
  edge: number;
  width: number;
  height: number;
  url: string;
}

/** What choosing a copy needs to know about an image. */
export interface RenditionSource {
  url?: string;
  dataUrl?: string;
  width?: number;
  height?: number;
  renditions?: LayoutImageRendition[];
}

/**
 * The copy sizes worth making for an image of this size: each edge smaller than its longest, and
 * none for an image no longer than RENDITION_MIN_SOURCE_EDGE.
 */
export function renditionEdgesFor(width: number, height: number): number[] {
  const longest = Math.max(width, height);
  if (!(longest > RENDITION_MIN_SOURCE_EDGE)) return [];
  return RENDITION_EDGES.filter((edge) => edge < longest);
}

/**
 * The URL to draw an image from when it covers `drawnWidth` CSS pixels across at `pixelRatio`
 * device pixels per CSS pixel: the smallest copy at least that many pixels wide, or the original
 * when no copy is wide enough. A data URL is the image itself and always wins.
 */
export function chooseRenditionUrl(image: RenditionSource, drawnWidth: number, pixelRatio: number): string | null {
  const original = image.dataUrl || image.url || null;
  if (image.dataUrl || !image.renditions?.length || !Number.isFinite(drawnWidth) || drawnWidth <= 0) {
    return original;
  }
  const needed = drawnWidth * Math.max(1, pixelRatio);
  const wideEnough = [...image.renditions]
    .filter((rendition) => rendition.width >= needed && (!image.width || rendition.width < image.width))
    .sort((a, b) => a.width - b.width);
  return wideEnough[0]?.url ?? original;
}

/**
 * How wide, in CSS pixels at the layout's own size, a background image is drawn in a
 * `boxWidth` x `boxHeight` layer, for each background size mode the renderer uses.
 */
export function backgroundDrawnWidth(
  mode: string | undefined,
  scale: number | undefined,
  boxWidth: number,
  boxHeight: number,
  imageWidth: number,
  imageHeight: number,
): number {
  if (!imageWidth || !imageHeight) return boxWidth;
  switch (mode) {
    case "contain":
      return imageWidth * Math.min(boxWidth / imageWidth, boxHeight / imageHeight);
    case "stretch":
      return boxWidth;
    case "custom":
      return boxWidth * (typeof scale === "number" && Number.isFinite(scale) && scale > 0 ? scale : 1);
    case "tile":
      // background-size: tile is not valid CSS, so the browser draws the image at its own size.
      return imageWidth;
    case "cover":
    default:
      return imageWidth * Math.max(boxWidth / imageWidth, boxHeight / imageHeight);
  }
}

/**
 * Makes a copy of `source` with a longest edge of `edge` pixels, as WebP (which keeps alpha),
 * resampled with the browser's high-quality filter. Null when the browser cannot do it.
 */
export async function makeRendition(source: Blob, edge: number): Promise<{ blob: Blob; width: number; height: number } | null> {
  if (typeof createImageBitmap !== "function" || typeof OffscreenCanvas === "undefined") return null;
  const probe = await createImageBitmap(source);
  const longest = Math.max(probe.width, probe.height);
  if (longest <= edge) {
    probe.close();
    return null;
  }
  const width = Math.max(1, Math.round((probe.width * edge) / longest));
  const height = Math.max(1, Math.round((probe.height * edge) / longest));
  probe.close();
  const bitmap = await createImageBitmap(source, { resizeWidth: width, resizeHeight: height, resizeQuality: "high" });
  const canvas = new OffscreenCanvas(width, height);
  const context = canvas.getContext("2d");
  if (!context) {
    bitmap.close();
    return null;
  }
  context.drawImage(bitmap, 0, 0);
  bitmap.close();
  const blob = await canvas.convertToBlob({ type: "image/webp", quality: 0.92 });
  return { blob, width, height };
}
