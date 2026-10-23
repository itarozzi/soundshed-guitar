/**
 * Align buttons for the selected control, label or rectangle: to the canvas's left, centre or
 * right, and its top, middle or bottom. The element's size is read from the designer canvas, where
 * it is drawn at the layout's own size, so a control's label and value are counted in.
 */

import { escapeHtml } from "../utils.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

export type AlignEdge = "left" | "center" | "right" | "top" | "middle" | "bottom";

const BUTTONS: { edge: AlignEdge; label: string }[] = [
  { edge: "left", label: "Left" },
  { edge: "center", label: "Centre" },
  { edge: "right", label: "Right" },
  { edge: "top", label: "Top" },
  { edge: "middle", label: "Middle" },
  { edge: "bottom", label: "Bottom" },
];

/** Where an element of `size` lands on a `canvas` when aligned to `edge`; the other axis is kept. */
export function alignedPosition(
  edge: AlignEdge,
  position: { x: number; y: number },
  size: { width: number; height: number },
  canvas: { width: number; height: number },
): { x: number; y: number } {
  switch (edge) {
    case "left": return { x: 0, y: position.y };
    case "center": return { x: Math.round((canvas.width - size.width) / 2), y: position.y };
    case "right": return { x: Math.round(canvas.width - size.width), y: position.y };
    case "top": return { x: position.x, y: 0 };
    case "middle": return { x: position.x, y: Math.round((canvas.height - size.height) / 2) };
    case "bottom": return { x: position.x, y: Math.round(canvas.height - size.height) };
  }
}

export function renderAlignGroup(): string {
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Align to Canvas</div>
        <div class="layout-align-buttons">
          ${BUTTONS.map(({ edge, label }) => `<button type="button" data-align="${escapeHtml(edge)}" title="Align to the canvas ${escapeHtml(label.toLowerCase())}">${escapeHtml(label)}</button>`).join("")}
        </div>
      </div>`;
}

/**
 * Wires the buttons: `element` finds the selected item on the designer canvas (for its drawn size)
 * and `position` is the object whose x and y the layout stores.
 */
export function bindAlignGroup(
  host: LayoutPropertiesHost,
  element: () => HTMLElement | null,
  position: { x: number; y: number },
): void {
  document.querySelectorAll<HTMLButtonElement>(".layout-align-buttons [data-align]").forEach((button) => {
    button.addEventListener("click", () => {
      const layout = host.getLayout();
      const target = element();
      if (!layout || !target) return;
      host.pushUndoState();
      const next = alignedPosition(button.dataset.align as AlignEdge, position, { width: target.offsetWidth, height: target.offsetHeight }, layout.dimensions);
      position.x = Math.max(0, next.x);
      position.y = Math.max(0, next.y);
      host.renderCanvas();
      host.renderSidebar();
    });
  });
}
