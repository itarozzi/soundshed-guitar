/**
 * The designer draws its controls with the runtime renderer, so a control is laid out as the
 * params panel shows it: the same markup, sizes, images, label and value text. Edit mode draws
 * each control on its own inside a draggable placeholder; preview mode draws the layout whole.
 * Knobs are bound as the panel binds them (turned to their value, the value in the panel's
 * format) but never send anything.
 */

import { GenericKnob } from "../knob.js";
import { renderCustomLayoutPreviewLayers } from "../layoutRenderer.js";
import type { LayoutResourceControlDef } from "../layoutRenderer.js";
import type { EffectLayout, LayoutControl } from "../layoutTypes.js";
import { parseParamTaper } from "../paramTaper.js";
import { knobValueFormatter } from "../paramValueFormat.js";
import type { GraphNode } from "../types.js";
import type { LayoutResourceCandidate } from "./types.js";

type PreviewParamDefs = Parameters<typeof renderCustomLayoutPreviewLayers>[2];

/** A stand-in node for the effect type, every parameter at its default. */
export function designerPreviewNode(effectType: string, paramDefs: PreviewParamDefs): GraphNode {
  return {
    id: "layout-designer-preview",
    type: effectType,
    params: Object.fromEntries(paramDefs.map((paramDef) => [paramDef.key, paramDef.default ?? 0])),
  } as unknown as GraphNode;
}

/** The effect's resource slots as the renderer takes them, each showing its own name. */
export function designerResourceDefs(candidates: LayoutResourceCandidate[]): LayoutResourceControlDef[] {
  return candidates.map((candidate) => ({
    resourceControlKey: candidate.controlKey,
    displayName: candidate.displayName,
    resourceType: candidate.resourceType,
    resourceIndex: candidate.resourceIndex,
    exposedResourceId: candidate.exposedResourceId,
    allowBrowseFile: candidate.allowBrowseFile,
    currentDisplayName: candidate.displayName,
    currentFilePath: "",
    isMissing: false,
  }));
}

/**
 * One control as the runtime draws it, placed at the top left of its container rather than at
 * its layout position; null when the runtime would not draw it (its parameter or resource slot
 * is not on this effect).
 */
export function controlPreviewElement(
  control: LayoutControl,
  layout: EffectLayout,
  node: GraphNode,
  paramDefs: PreviewParamDefs,
  resources: LayoutResourceControlDef[],
): HTMLElement | null {
  const single: EffectLayout = { ...layout, controls: [{ ...control, position: { x: 0, y: 0 } }], textLabels: [] };
  const template = document.createElement("template");
  template.innerHTML = renderCustomLayoutPreviewLayers(node, single, paramDefs, resources);
  const element = template.content.querySelector<HTMLElement>(".custom-layout-control");
  if (!element) return null;
  element.style.position = "relative";
  element.style.removeProperty("left");
  element.style.removeProperty("top");
  bindPreviewKnobs(element);
  return element;
}

/** Turns each knob under `root` to its value and writes its value text, as the panel's binding does. */
export function bindPreviewKnobs(root: HTMLElement): void {
  root.querySelectorAll<HTMLElement>(".node-param-knob").forEach((knob) => {
    const min = parseFloat(knob.dataset.min || "0");
    const max = parseFloat(knob.dataset.max || "1");
    new GenericKnob({
      knobElement: knob,
      paramId: `layout-designer_${knob.dataset.paramKey ?? "param"}`,
      minValue: min,
      maxValue: max,
      defaultValue: parseFloat(knob.dataset.default || knob.dataset.value || "0"),
      displayFormat: knobValueFormatter(knob),
      valueDisplay: knob.parentElement?.querySelector<HTMLElement>(".node-param-value") ?? null,
      labelElement: knob.parentElement?.querySelector<HTMLElement>(".node-param-label, .custom-control-label") ?? null,
      stepValue: knob.dataset.step ? parseFloat(knob.dataset.step) : undefined,
      taper: parseParamTaper(knob.dataset.taper),
      sendParameter: false,
    });
    // Out of the tab order: the designer's arrow keys move the selection, not a knob.
    knob.tabIndex = -1;
  });
}
