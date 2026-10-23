/**
 * How big a custom layout is drawn in the space the params panel gives it.
 *
 * A layout is designed at a fixed size (its `dimensions`), and the panel scales the whole of it
 * as one piece. A layout with no `scaling` block keeps the rule every layout made before the
 * block existed was drawn by: its own size, centred, when the panel is wide enough, and shrunk to
 * the panel's width when it is not. It never grows. See docs/plans/layout-designer-hires.md.
 */

import { escapeHtml } from "./utils.js";

/** A layout's choice of how it fits the panel; every field is optional. */
export interface LayoutScaling {
  /**
   * "fit-width" (the default) scales to the panel's width; "fit" also keeps the whole layout
   * within the height the panel has; "fixed" always draws it at its own size.
   */
  mode?: "fit-width" | "fit" | "fixed";
  /** Never smaller than this; below it the layout overflows the panel. Default 0. */
  minScale?: number;
  /** Never larger than this. Default 1: a layout grows past its own size only if it asks to. */
  maxScale?: number;
  /** Where a layout narrower than the panel sits: "center" (the default) or "start". */
  align?: "start" | "center";
}

export interface LayoutScaleInput {
  designWidth: number;
  designHeight: number;
  availableWidth: number;
  /** Height the layout may take; absent or not positive means unbounded. */
  availableHeight?: number;
  scaling?: LayoutScaling;
}

export interface LayoutScaleResult {
  /** The uniform scale to draw at. */
  scale: number;
  /** True when the layout is narrower than the panel and sits in the middle of it. */
  centred: boolean;
  /** True when the layout is wider than the panel at this scale (a minScale or "fixed"). */
  overflows: boolean;
}

const MAX_SCALE_LIMIT = 8;

function finitePositive(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : fallback;
}

/** The scale and placement for a layout, from its design size, the space it has and its `scaling`. */
export function computeLayoutScale(input: LayoutScaleInput): LayoutScaleResult {
  const { designWidth, designHeight, availableWidth } = input;
  const scaling = input.scaling ?? {};
  const mode = scaling.mode === "fit" || scaling.mode === "fixed" ? scaling.mode : "fit-width";
  const maxScale = Math.min(MAX_SCALE_LIMIT, finitePositive(scaling.maxScale, 1));
  const minScale = Math.min(maxScale, typeof scaling.minScale === "number" && Number.isFinite(scaling.minScale) && scaling.minScale > 0
    ? scaling.minScale
    : 0);

  let raw = mode === "fixed" ? 1 : availableWidth / designWidth;
  if (mode === "fit" && typeof input.availableHeight === "number" && input.availableHeight > 0) {
    raw = Math.min(raw, input.availableHeight / designHeight);
  }
  const scale = mode === "fixed" ? 1 : Math.max(minScale, Math.min(maxScale, raw));
  const drawnWidth = designWidth * scale;
  const overflows = drawnWidth > availableWidth + 0.5;
  const narrower = drawnWidth < availableWidth - 0.5;

  return {
    scale,
    // The original rule centres a layout drawn at its own size, and a shrunk one fills the width
    // exactly, so it is not centred; a layout that grows stops at maxScale and may be narrower.
    centred: !overflows && scaling.align !== "start" && (scale === 1 || narrower),
    overflows,
  };
}

/**
 * True when a layout's scaling lets it be drawn larger than its own size, so the panel must
 * give it more room than its width; never for a layout without a scaling block.
 */
export function layoutCanGrow(scaling: unknown): boolean {
  if (!scaling || typeof scaling !== "object") return false;
  const { mode, maxScale } = scaling as LayoutScaling;
  return mode !== "fixed" && typeof maxScale === "number" && Number.isFinite(maxScale) && maxScale > 1;
}

/**
 * ` data-layout-scaling="…"` for a layout with a `scaling` block, read by applyCustomLayoutScaling,
 * or nothing for one without, which keeps the original fit-width rule. Only the known fields,
 * each of the right type, go through: the JSON may hold anything.
 */
export function scalingAttribute(layout: { scaling?: unknown }): string {
  const scaling = layout.scaling as Record<string, unknown> | undefined;
  if (!scaling || typeof scaling !== "object") return "";
  const clean: Record<string, unknown> = {};
  if (scaling.mode === "fit-width" || scaling.mode === "fit" || scaling.mode === "fixed") clean.mode = scaling.mode;
  if (scaling.align === "start" || scaling.align === "center") clean.align = scaling.align;
  for (const key of ["minScale", "maxScale"] as const) {
    const value = scaling[key];
    if (typeof value === "number" && Number.isFinite(value) && value > 0) clean[key] = value;
  }
  return ` data-layout-scaling="${escapeHtml(JSON.stringify(clean))}"`;
}

