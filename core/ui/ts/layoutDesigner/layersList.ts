/**
 * The designer's layers list: everything in the layout as a short list above the properties,
 * front-most first, so any of it can be picked to edit, including the layout itself. Without it,
 * a background layer covering the canvas took every click, and the layout's own settings (the
 * default controls' position and scale among them) could not be reached.
 */

import type { ParameterDef } from "../presetV2.js";
import type { EffectLayout } from "../layoutTypes.js";
import { escapeHtml } from "../utils.js";
import type { LayoutResourceCandidate, SelectedElement } from "./types.js";

export interface LayersListHost {
  getLayout(): EffectLayout | null;
  getParamDefs(): ParameterDef[];
  getResourceCandidates(): LayoutResourceCandidate[];
  getSelected(): SelectedElement;
  select(element: SelectedElement): void;
}

interface LayerItem {
  element: SelectedElement;
  name: string;
  kind: string;
}

const CONTROL_KINDS: Record<string, string> = { knob: "Knob", slider: "Slider", toggle: "Toggle", dropdown: "Selector" };
const BACKGROUND_KINDS: Record<string, string> = { image: "Image", color: "Colour", gradient: "Gradient" };

function layerGroups(host: LayersListHost, layout: EffectLayout): { title: string; items: LayerItem[] }[] {
  const paramDefs = host.getParamDefs();
  const resources = host.getResourceCandidates();
  // The default controls are placed by the layout's own settings, so they pick the layout.
  const controls: LayerItem[] = layout.useDefaultControls
    ? [{ element: null, name: "Default controls", kind: "Layout" }]
    : layout.controls.map((control) => ({
      element: { type: "control" as const, paramKey: control.paramKey },
      name: control.labelOverride
        || resources.find((candidate) => candidate.controlKey === control.paramKey)?.displayName
        || paramDefs.find((def) => def.key === control.paramKey)?.name
        || control.paramKey,
      kind: CONTROL_KINDS[control.type] ?? control.type,
    })).reverse();
  const labels = layout.textLabels.map((label) => ({
    element: { type: "label" as const, id: label.id },
    name: label.text.trim() || "Label",
    kind: "Text",
  })).reverse();
  const rectangles = (layout.overlays ?? []).map((overlay, index) => ({
    element: { type: "overlay" as const, id: overlay.id },
    name: `Rectangle ${index + 1}`,
    kind: overlay.style?.toggleBypassOnClick ? "Power" : overlay.style?.imageId ? "Image" : "Shape",
  })).reverse();
  const backgrounds = [...layout.backgrounds].sort((a, b) => b.layerIndex - a.layerIndex).map((bg) => ({
    element: { type: "background" as const, layerIndex: bg.layerIndex },
    name: `Background ${bg.layerIndex + 1}`,
    kind: BACKGROUND_KINDS[bg.type] ?? bg.type,
  }));
  return [
    { title: "", items: [{ element: null, name: "Layout", kind: "Size, text, fit" }] },
    { title: "Labels", items: labels },
    { title: "Controls", items: controls },
    { title: "Rectangles", items: rectangles },
    { title: "Backgrounds", items: backgrounds },
  ].filter((group) => group.items.length > 0);
}

function sameElement(a: SelectedElement, b: SelectedElement): boolean {
  if (a === null || b === null) return a === b;
  if (a.type !== b.type) return false;
  switch (a.type) {
    case "control": return a.paramKey === (b as typeof a).paramKey;
    case "background": return a.layerIndex === (b as typeof a).layerIndex;
    default: return a.id === (b as typeof a).id;
  }
}

/** Redraws the list into `container`, the current selection marked. */
export function renderLayersList(container: HTMLElement | null, host: LayersListHost): void {
  const layout = host.getLayout();
  if (!container || !layout) return;
  const selected = host.getSelected();
  const items: LayerItem[] = [];
  container.innerHTML = layerGroups(host, layout).map((group) => `
    ${group.title ? `<div class="layout-layer-heading">${escapeHtml(group.title)}</div>` : ""}
    ${group.items.map((item) => {
      const index = items.push(item) - 1;
      const isSelected = sameElement(item.element, selected);
      return `<button type="button" class="layout-layer-item${isSelected ? " is-selected" : ""}" data-layer-index="${index}"${isSelected ? ` aria-current="true"` : ""}><span class="layout-layer-name">${escapeHtml(item.name)}</span><span class="layout-layer-kind">${escapeHtml(item.kind)}</span></button>`;
    }).join("")}
  `).join("");
  layerItemsByContainer.set(container, items);
}

const layerItemsByContainer = new WeakMap<HTMLElement, LayerItem[]>();

/** Wires the list once: a click picks that layer to edit. */
export function bindLayersList(container: HTMLElement | null, host: LayersListHost): void {
  container?.addEventListener("click", (event) => {
    const button = (event.target as HTMLElement).closest<HTMLElement>(".layout-layer-item");
    const item = button ? layerItemsByContainer.get(container)?.[Number(button.dataset.layerIndex)] : undefined;
    if (item) host.select(item.element);
  });
}
