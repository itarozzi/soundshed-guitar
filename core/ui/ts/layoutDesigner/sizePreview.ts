/**
 * "Preview at panel size": the layout drawn as the params panel would draw it, by the real
 * renderer and applyCustomLayoutScaling, in a panel of a chosen width and height, with the scale
 * it lands at and the image copies it loads. Zooming the designer canvas shows the design; this
 * shows what someone with a narrow window, a large one or a high-DPI screen gets.
 */

import type { BlendParamDef } from "../blendUtils.js";
import { renderCustomLayout, renderCustomLayoutBackdrop } from "../layoutRenderer.js";
import type { EffectLayout } from "../layoutTypes.js";
import type { ParameterDef } from "../presetV2.js";
import { applyCustomLayoutScaling } from "../signalPath/paramsPanel/layoutOverlay.js";
import type { GraphNode } from "../types.js";
import { escapeHtml } from "../utils.js";

/** Panel sizes worth checking: a narrow window, a laptop, a desktop, a large screen. */
export const PREVIEW_PANELS = [
  { label: "Narrow", width: 360, height: 480 },
  { label: "Laptop", width: 760, height: 520 },
  { label: "Desktop", width: 1200, height: 700 },
  { label: "Large", width: 1800, height: 1000 },
] as const;

const DEFAULT_CONTROLS_STUB = `<div class="layout-size-preview-stub">Standard controls</div>`;

/** What the preview reports for one panel size. */
export interface SizePreviewReport {
  scale: number;
  copies: string[];
}

/** Draws `layout` into `container` as the params panel would, and reports how it landed. */
export function renderSizePreview(container: HTMLElement, layout: EffectLayout, paramDefs: ParameterDef[]): SizePreviewReport {
  const params = Object.fromEntries(paramDefs.map((def) => [def.key, def.default]));
  const node = { id: "layout-size-preview", type: layout.effectType, params } as unknown as GraphNode;
  const html = layout.useDefaultControls
    ? renderCustomLayoutBackdrop(node, structuredClone(layout), DEFAULT_CONTROLS_STUB)
    : renderCustomLayout(node, structuredClone(layout), paramDefs as BlendParamDef[], []);
  container.innerHTML = `<section class="default-effect-shell has-custom-layout"><div class="default-effect-shell-main">`
    + `<div class="default-effect-section default-effect-section-controls default-effect-section-custom-layout">${html}</div></div></section>`;
  applyCustomLayoutScaling(container);
  const transform = container.querySelector<HTMLElement>(".custom-layout-container")?.style.transform ?? "";
  const scale = Number(transform.match(/scale\(([\d.]+)\)/)?.[1] ?? 1);
  const copies = [...new Set((html.match(/layout-images\/rendition\/[^/"&]+\/\d+/g) ?? []).map((path) => {
    const [, , id, edge] = path.split("/");
    return `${decodeURIComponent(id)} at ${edge} px`;
  }))];
  return { scale, copies };
}

/** Opens the preview over the designer; closing it puts the live panel's scaling back. */
export function openLayoutSizePreview(layout: EffectLayout, paramDefs: ParameterDef[]): void {
  document.getElementById("layout-size-preview")?.remove();
  const dialog = document.createElement("div");
  dialog.id = "layout-size-preview";
  dialog.className = "layout-size-preview";
  dialog.setAttribute("role", "dialog");
  dialog.setAttribute("aria-label", "Preview at panel size");
  dialog.innerHTML = `
    <div class="layout-size-preview-bar">
      <span class="layout-size-preview-title">Preview at panel size</span>
      ${PREVIEW_PANELS.map((panel, index) => `<button type="button" data-preview-panel="${index}">${escapeHtml(`${panel.label} ${panel.width} × ${panel.height}`)}</button>`).join("")}
      <span class="layout-size-preview-report" aria-live="polite"></span>
      <button type="button" class="layout-size-preview-close" aria-label="Close preview">Close</button>
    </div>
    <div class="layout-size-preview-panel"></div>`;
  document.body.appendChild(dialog);

  const panel = dialog.querySelector<HTMLElement>(".layout-size-preview-panel")!;
  const report = dialog.querySelector<HTMLElement>(".layout-size-preview-report")!;
  const show = (index: number) => {
    const size = PREVIEW_PANELS[index];
    panel.style.width = `${size.width}px`;
    panel.style.height = `${size.height}px`;
    dialog.querySelectorAll<HTMLButtonElement>("[data-preview-panel]").forEach((button) => {
      button.classList.toggle("active", Number(button.dataset.previewPanel) === index);
    });
    const result = renderSizePreview(panel, layout, paramDefs);
    const pixelRatio = Math.round((window.devicePixelRatio || 1) * 100) / 100;
    report.textContent = `Drawn at ${Math.round(result.scale * 100)}% on a ${pixelRatio}× screen`
      + (result.copies.length ? ` · copies: ${result.copies.join(", ")}` : "");
  };
  dialog.querySelectorAll<HTMLButtonElement>("[data-preview-panel]").forEach((button) => {
    button.addEventListener("click", () => show(Number(button.dataset.previewPanel)));
  });
  const close = () => {
    dialog.remove();
    document.removeEventListener("keydown", onKey, true);
    // The preview took over the shared scaling observers; give them back to the live panel.
    applyCustomLayoutScaling(document.getElementById("node-params-panel"));
  };
  const onKey = (event: KeyboardEvent) => {
    if (event.key === "Escape") {
      event.stopPropagation();
      close();
    }
  };
  document.addEventListener("keydown", onKey, true);
  dialog.querySelector(".layout-size-preview-close")!.addEventListener("click", close);
  show(1);
}
