/**
 * Making the smaller copies of a large layout image, in the background, once per file version.
 *
 * The first time an image longer than RENDITION_MIN_SOURCE_EDGE is drawn without its copies, the
 * original is fetched once, each missing copy is resampled here (the WebView has the image codecs;
 * the engine does not) and sent to the engine, which checks and keeps it beside every other copy
 * (layouts/renditions). Its reply adds the copy to the library, and later draws use it.
 */

import { postMessage } from "./bridge.js";
import { makeRendition, renditionEdgesFor } from "./layoutRenditions.js";
import type { LayoutImageRef } from "./layoutTypes.js";
import { arrayBufferToBase64 } from "./utils.js";

export interface RenditionMakerDeps {
  fetchBlob: (url: string) => Promise<Blob>;
  make: typeof makeRendition;
  /** Sends one copy to the engine. */
  send: (copy: { imageId: string; version: string; edge: number; data: string }) => void;
}

const defaultDeps: RenditionMakerDeps = {
  fetchBlob: async (url) => {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return response.blob();
  },
  make: makeRendition,
  send: ({ imageId, version, edge, data }) => postMessage({ type: "saveLayoutImageRendition", imageId, version, edge, data }),
};

/** Image versions already being made (or tried) this session, so a redraw does not start another. */
const started = new Set<string>();

/** The copy sizes `image` still lacks, or none when it needs no copies or cannot have them. */
export function missingRenditionEdges(image: LayoutImageRef): number[] {
  if (!image.url || image.dataUrl || !image.version || !image.width || !image.height) return [];
  const have = new Set((image.renditions ?? []).map((rendition) => rendition.edge));
  return renditionEdgesFor(image.width, image.height).filter((edge) => !have.has(edge));
}

/**
 * Starts making the copies `image` lacks, unless that is already under way for this version.
 * Resolves once every copy has been sent (or making them failed, which is only logged: the image
 * is still drawn from its original).
 */
export async function ensureLayoutImageRenditions(image: LayoutImageRef, deps: RenditionMakerDeps = defaultDeps): Promise<void> {
  const edges = missingRenditionEdges(image);
  const key = `${image.imageId}@${image.version}`;
  if (!edges.length || started.has(key)) return;
  started.add(key);

  try {
    const original = await deps.fetchBlob(image.url!);
    for (const edge of edges) {
      const copy = await deps.make(original, edge);
      if (!copy) return;
      deps.send({
        imageId: image.imageId,
        version: image.version!,
        edge,
        data: arrayBufferToBase64(await copy.blob.arrayBuffer()),
      });
    }
  } catch (error) {
    console.warn(`[layout] Could not make smaller copies of ${image.imageId}:`, error);
  }
}

/** Forgets what was started, for tests. */
export function resetRenditionMakerForTests(): void {
  started.clear();
}
