/**
 * "Images in this layout": every picture the layout draws on, how big it is, what it costs to
 * draw, where it is used, and a way to swap it for another library image everywhere at once.
 *
 * The costs are what matter with high-resolution art: a 6000 x 4000 PNG decodes to 96 MB however
 * small the panel draws it. Images over 2048 px get smaller copies automatically (layoutRenditions);
 * a filmstrip taller than a GPU texture does not, and is flagged.
 */

import { resolveLayoutImageUrl } from "../layoutImages.js";
import { visitLayoutImageRefs, type EffectLayout, type LayoutImageRef } from "../layoutTypes.js";
import { RENDITION_MIN_SOURCE_EDGE } from "../layoutRenditions.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

/** The tallest strip most GPUs draw as one texture. */
const MAX_TEXTURE_EDGE = 16384;
/** Decoding more than this for one image is worth knowing about. */
const LARGE_DECODE_BYTES = 64 * 1024 * 1024;

export interface LayoutAssetRow {
  imageId: string;
  image?: LayoutImageRef;
  /** RGBA bytes once decoded at full size. */
  decodedBytes?: number;
  uses: string[];
  warnings: string[];
}

function megabytes(bytes: number): string {
  return bytes >= 1024 * 1024 ? `${(bytes / (1024 * 1024)).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

/** One row per image the layout draws on, in the order the layout uses them. */
export function layoutAssets(layout: EffectLayout): LayoutAssetRow[] {
  const rows = new Map<string, LayoutAssetRow>();
  visitLayoutImageRefs(layout, (imageId, _set, where) => {
    let row = rows.get(imageId);
    if (!row) {
      const image = uiState.layoutLibrary?.images.find((img) => img.imageId === imageId);
      row = { imageId, image, uses: [], warnings: [] };
      if (image?.width && image.height) row.decodedBytes = image.width * image.height * 4;
      rows.set(imageId, row);
    }
    if (!row.uses.includes(where)) row.uses.push(where);
  });

  const filmstrips = new Map<string, number>();
  for (const control of layout.controls) {
    const knobImage = control.style?.knobImage;
    if (knobImage?.mode === "filmstrip" && knobImage.imageId) filmstrips.set(knobImage.imageId, knobImage.columns ?? 1);
  }

  for (const row of rows.values()) {
    const { image } = row;
    if (!image) {
      row.warnings.push("Not in the image library, so nothing is drawn for it.");
      continue;
    }
    const longest = Math.max(image.width ?? 0, image.height ?? 0);
    if (row.decodedBytes && row.decodedBytes > LARGE_DECODE_BYTES) {
      row.warnings.push(`Decodes to ${megabytes(row.decodedBytes)} at full size${longest > RENDITION_MIN_SOURCE_EDGE ? "; smaller copies are drawn where they are sharp enough" : ""}.`);
    }
    if (filmstrips.has(row.imageId) && (image.height ?? 0) > MAX_TEXTURE_EDGE) {
      row.warnings.push(`Taller than ${MAX_TEXTURE_EDGE.toLocaleString("en")} px, which many GPUs cannot draw in one piece: lay the frames out in columns.`);
    }
  }
  return [...rows.values()];
}

/** The group's markup, or empty for a layout that uses no images. */
export function renderLayoutAssetsGroup(layout: EffectLayout, imageOptions: (selectedImageId?: string) => string): string {
  const rows = layoutAssets(layout);
  if (!rows.length) return "";
  const items = rows.map((row, index) => {
    const { image } = row;
    const thumbnail = resolveLayoutImageUrl(row.imageId, 48);
    const facts = image
      ? [
          image.width && image.height ? `${image.width} × ${image.height}` : "",
          image.bytes ? megabytes(image.bytes) : "",
          row.decodedBytes ? `${megabytes(row.decodedBytes)} decoded` : "",
          image.renditions?.length ? `${image.renditions.length} smaller ${image.renditions.length === 1 ? "copy" : "copies"}` : "",
        ].filter(Boolean).join(" · ")
      : "";
    return `
        <div class="layout-asset-row" data-asset-index="${index}">
          <div class="layout-asset-head">
            ${thumbnail ? `<img class="layout-asset-thumb" src="${escapeHtml(thumbnail)}" alt="">` : `<span class="layout-asset-thumb layout-asset-thumb-missing"></span>`}
            <div class="layout-asset-text">
              <div class="layout-asset-name" title="${escapeHtml(row.imageId)}">${escapeHtml(image?.fileName || row.imageId)}</div>
              <div class="layout-asset-facts">${escapeHtml(facts)}</div>
              <div class="layout-asset-uses">${escapeHtml(row.uses.join(", "))}</div>
            </div>
          </div>
          ${row.warnings.map((warning) => `<div class="layout-asset-warning">${escapeHtml(warning)}</div>`).join("")}
          <div class="layout-property-row">
            <span class="layout-property-label">Replace</span>
            <div class="layout-property-input">
              <select class="layout-asset-replace" data-image-id="${escapeHtml(row.imageId)}" title="Use another library image everywhere this one is used">${imageOptions(row.imageId)}</select>
            </div>
          </div>
        </div>`;
  });
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Images in this Layout</div>${items.join("")}
      </div>`;
}

/** Points every use of an image at the one picked in its Replace list. */
export function bindLayoutAssetsGroup(layout: EffectLayout, host: LayoutPropertiesHost, root: ParentNode = document): void {
  root.querySelectorAll<HTMLSelectElement>("select.layout-asset-replace").forEach((select) => {
    select.addEventListener("change", () => {
      const from = select.dataset.imageId;
      const to = select.value;
      if (!from || !to || to === from) return;
      host.pushUndoState();
      visitLayoutImageRefs(layout, (imageId, set) => {
        if (imageId === from) set(to);
      });
      host.renderCanvas();
      host.renderSidebar();
    });
  });
}
