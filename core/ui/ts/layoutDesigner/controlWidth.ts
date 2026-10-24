/**
 * A resource selector's width on the layout: the room its background art leaves for the
 * resource's name. Unset, the selector is as wide as the name, up to the layout's right edge; set,
 * it takes that width in the layout's own pixels and a longer name ends in an ellipsis. Set in
 * the sidebar or by dragging the selected selector's right edge.
 */

import type { EffectLayout, LayoutControl } from "../layoutTypes.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

/** The narrowest a selector can be when its buttons cannot be measured. */
export const MIN_RESOURCE_WIDTH = 120;

/** Room kept for the name: a few letters and the caret (the label's min-width, custom-layouts.css). */
const NAME_MIN_WIDTH = 64;

/**
 * The narrowest the drawn selector can be and still show its buttons and a few letters of the
 * name: its buttons (Browse, the arrows, Clear, whichever it has), the gaps between them, and
 * the name's minimum.
 */
export function measuredMinWidth(placeholder: Element | null): number {
  const row = placeholder?.querySelector<HTMLElement>(".resource-controls");
  if (!row) return MIN_RESOURCE_WIDTH;
  const items = Array.from(row.children).filter((el): el is HTMLElement => el instanceof HTMLElement && el.offsetWidth > 0);
  const buttons = items.filter((el) => !el.classList.contains("resource-picker-label"));
  const gap = parseFloat(getComputedStyle(row).columnGap) || 0;
  const width = buttons.reduce((sum, el) => sum + el.offsetWidth, 0) + gap * buttons.length + NAME_MIN_WIDTH;
  return Math.max(MIN_RESOURCE_WIDTH, Math.ceil(width));
}

/** `width` held between `min` and the room left before the layout's right edge. */
export function clampControlWidth(layout: EffectLayout, control: LayoutControl, width: number, min = MIN_RESOURCE_WIDTH): number {
  const room = Math.max(min, Math.round(layout.dimensions.width - control.position.x));
  return Math.max(min, Math.min(room, Math.round(width)));
}

/** Sets the control's width, or with null leaves it to the name; a stored height is kept. */
export function setControlWidth(control: LayoutControl, width: number | null): void {
  if (width === null) {
    delete control.size;
    return;
  }
  control.size = { width, height: control.size?.height ?? 0 };
}

/** The drag handle on a selected selector's right edge. */
export function appendWidthHandle(placeholder: HTMLElement): void {
  const handle = document.createElement("div");
  handle.className = "layout-resize-handle layout-width-handle";
  handle.dataset.controlHandle = "width";
  handle.title = "Drag to set the width the name can use";
  placeholder.appendChild(handle);
}

/** Shows a width mid-drag on the drawn control, as the renderer draws a set width. */
export function previewControlWidth(placeholder: HTMLElement, width: number): void {
  const drawn = placeholder.querySelector<HTMLElement>(".custom-layout-control");
  if (!drawn) return;
  drawn.style.width = `${width}px`;
  drawn.classList.add("has-width");
  placeholder.dataset.dragWidth = String(width);
}

export function renderWidthGroup(control: LayoutControl): string {
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Width</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Width</span>
          <div class="layout-property-input">
            <input type="number" id="prop-control-width" min="${MIN_RESOURCE_WIDTH}" step="8" placeholder="Fit name" value="${control.size ? Math.round(control.size.width) : ""}" title="The room the name can use, in layout pixels; empty fits the name">
          </div>
        </div>
        <div class="layout-property-hint">The room the background leaves for the name. A longer name ends in “…”. Empty fits the name. Drag the selector’s right edge to set it on the canvas.</div>
      </div>`;
}

/** `placeholder` finds the selector on the designer canvas, to measure its narrowest width. */
export function bindWidthGroup(host: LayoutPropertiesHost, control: LayoutControl, placeholder: () => Element | null): void {
  const input = document.getElementById("prop-control-width") as HTMLInputElement | null;
  if (input) input.min = String(measuredMinWidth(placeholder()));
  input?.addEventListener("change", () => {
    const layout = host.getLayout();
    if (!layout) return;
    host.pushSidebarUndoOnce();
    const value = parseFloat(input.value);
    const width = input.value.trim() === "" || !Number.isFinite(value) ? null : clampControlWidth(layout, control, value, measuredMinWidth(placeholder()));
    setControlWidth(control, width);
    input.value = width === null ? "" : String(width);
    host.renderCanvas();
  });
}
