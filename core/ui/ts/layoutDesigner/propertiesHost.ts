/**
 * What the properties panels need from the layout designer around them.
 *
 * The designer builds this as an object of closures rather than passing itself,
 * so the members the panels use stay private to `LayoutDesignerModal`.
 *
 * Two things are worth knowing about the undo hooks. `pushUndoState` takes a
 * full snapshot, and is what a discrete action uses. `pushSidebarUndoOnce`
 * takes one snapshot for a whole run of sidebar edits — dragging a slider
 * fires a change per pixel, and each of those must not become its own undo
 * step.
 */

import type { EffectLayout, LayoutControl } from "../layoutTypes.js";
import type { ParameterDef } from "../presetV2.js";
import type { LayoutResourceCandidate, SelectedElement } from "./types.js";

export interface LayoutPropertiesHost {
  /** The layout being edited. Null before one is opened. */
  getLayout(): EffectLayout | null;
  /** The parameters the effect exposes, which a control can be bound to. */
  getParamDefs(): ParameterDef[];
  /** Resources the effect can point a control at (models, IRs). */
  getResourceCandidates(): LayoutResourceCandidate[];
  /** The panel these render into. */
  getSidebarContent(): HTMLElement | null;

  /** Snapshot for undo — one discrete edit. */
  pushUndoState(): void;
  /** Snapshot for undo — once per run of sidebar edits, not once per event. */
  pushSidebarUndoOnce(): void;

  renderCanvas(): void;
  renderSidebar(): void;
  selectElement(element: SelectedElement): void;

  /** The <option> list of images already in this layout's library. */
  renderImageOptionsHtml(purpose: "background" | "knob", selectedImageId?: string): string;
  browseBackgroundImage(layerIndex?: number): void;
  browseKnobImage(control: LayoutControl): void;
  /** Whether this control picks a resource rather than driving a parameter. */
  isResourceControl(control: LayoutControl): boolean;
}
