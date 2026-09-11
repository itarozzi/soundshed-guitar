/**
 * Pack artwork: downscaling what the user uploads, and fetching thumbnails
 * lazily as cards scroll into view.
 *
 * Thumbnails are fetched as blobs (the API wants the session header) and handed
 * to the DOM as object URLs, so this module also owns revoking them.
 */

import { buildApiUrl } from "./api.js";
import { toneSharingState } from "./state.js";
import type { ResizedImageResult, ToneSharingPack } from "./types.js";

export const packThumbnailObjectUrls = new Map<string, string>();

export function clearPackThumbnailObjectUrls(): void {
  for (const objectUrl of packThumbnailObjectUrls.values()) {
    URL.revokeObjectURL(objectUrl);
  }
  packThumbnailObjectUrls.clear();
}

export async function resizeImageToMaxWidth(source: Blob, maxWidth: number): Promise<ResizedImageResult> {
  const sourceUrl = URL.createObjectURL(source);
  try {
    const image = await new Promise<HTMLImageElement>((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = () => reject(new Error("Failed to load image"));
      img.src = sourceUrl;
    });

    const targetWidth = Math.max(1, Math.min(maxWidth, image.naturalWidth));
    const targetHeight = Math.max(1, Math.round((targetWidth / image.naturalWidth) * image.naturalHeight));

    const canvas = document.createElement("canvas");
    canvas.width = targetWidth;
    canvas.height = targetHeight;
    const context = canvas.getContext("2d");
    if (!context) {
      throw new Error("Canvas context unavailable");
    }

    context.drawImage(image, 0, 0, targetWidth, targetHeight);

    const outputType = source.type === "image/png" || source.type === "image/webp" || source.type === "image/jpeg"
      ? source.type
      : "image/jpeg";

    const blob = await new Promise<Blob>((resolve, reject) => {
      canvas.toBlob(
        (value) => {
          if (!value) {
            reject(new Error("Failed to encode resized image"));
            return;
          }
          resolve(value);
        },
        outputType,
        outputType === "image/png" ? undefined : 0.9
      );
    });

    return {
      blob,
      width: targetWidth,
      height: targetHeight
    };
  } finally {
    URL.revokeObjectURL(sourceUrl);
  }
}

export async function buildPackImageVariants(source: File): Promise<{
  small: ResizedImageResult;
  large: ResizedImageResult;
}> {
  const large = await resizeImageToMaxWidth(source, 2048);
  const small = await resizeImageToMaxWidth(large.blob, 512);
  return { small, large };
}

export async function resolvePackThumbnailUrl(pack: ToneSharingPack): Promise<string> {
  const thumbnailPath = pack.thumbnailUrl?.trim();
  if (!thumbnailPath) {
    return "";
  }

  const cacheKey = `${toneSharingState.apiBase}|${pack.id}|${thumbnailPath}`;
  const cached = packThumbnailObjectUrls.get(cacheKey);
  if (cached) {
    return cached;
  }

  const response = await fetch(buildApiUrl(thumbnailPath), {
    headers: toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {},
    credentials: "include"
  });
  if (!response.ok) {
    throw new Error(`Thumbnail load failed (${response.status})`);
  }

  const blob = await response.blob();
  const objectUrl = URL.createObjectURL(blob);
  packThumbnailObjectUrls.set(cacheKey, objectUrl);
  return objectUrl;
}

let packThumbnailObserver: IntersectionObserver | null = null;

/**
 * Lazily load pack hero thumbnails: each pack card carries a `data-pack-thumbnail`
 * path and only fetches its image once it scrolls into (or near) the viewport.
 */
export function observePackThumbnails(feed: HTMLElement): void {
  packThumbnailObserver?.disconnect();
  packThumbnailObserver = null;

  const cards = Array.from(feed.querySelectorAll<HTMLElement>("[data-pack-thumbnail]"));
  if (!cards.length) {
    return;
  }

  const loadCardThumbnail = (card: HTMLElement): void => {
    const thumbnailPath = card.dataset.packThumbnail;
    card.removeAttribute("data-pack-thumbnail");
    if (!thumbnailPath) {
      return;
    }
    const pack: ToneSharingPack = {
      id: card.dataset.id ?? "",
      title: card.dataset.title ?? "",
      thumbnailUrl: thumbnailPath,
    };
    void resolvePackThumbnailUrl(pack)
      .then((objectUrl) => {
        if (objectUrl) {
          card.style.setProperty("--tone-pack-thumbnail", `url('${objectUrl}')`);
        }
      })
      .catch(() => {});
  };

  if (typeof IntersectionObserver === "undefined") {
    cards.forEach(loadCardThumbnail);
    return;
  }

  packThumbnailObserver = new IntersectionObserver(
    (entries, observer) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          const card = entry.target as HTMLElement;
          observer.unobserve(card);
          loadCardThumbnail(card);
        }
      }
    },
    { rootMargin: "200px" },
  );
  cards.forEach((card) => packThumbnailObserver?.observe(card));
}
