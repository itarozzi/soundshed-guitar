/**
 * Which features a layout uses that an older app cannot draw, and the schema version that
 * follows from them.
 *
 * A layout's `version` is the oldest schema that can render it. One that uses nothing newer
 * than the original layout format stays version 1 with no `requires` list, so it is saved
 * exactly as before; one that uses a newer feature is version 2 and lists each such feature,
 * so a later app can say what it cannot show rather than guessing. Older apps ignore both
 * fields and draw what they understand (docs/plans/layout-designer-hires.md, rule 7).
 */

import { LEGACY_DIMENSION_LIMITS, type EffectLayout } from "./layoutTypes.js";

/** The features, in the order they are listed. */
export const LAYOUT_FEATURES = ["large-canvas", "scaling", "text-style"] as const;
export type LayoutFeature = (typeof LAYOUT_FEATURES)[number];

function isSet(value: unknown): boolean {
  return value !== undefined && value !== null && value !== "";
}

/** The newer features `layout` uses, in LAYOUT_FEATURES order. */
export function layoutRequirements(layout: EffectLayout): LayoutFeature[] {
  const used = new Set<LayoutFeature>();

  if (layout.dimensions.width > LEGACY_DIMENSION_LIMITS.maxWidth || layout.dimensions.height > LEGACY_DIMENSION_LIMITS.maxHeight) {
    used.add("large-canvas");
  }

  const scaling = layout.scaling;
  if (scaling && (
    scaling.mode === "fit" || scaling.mode === "fixed"
    || (typeof scaling.maxScale === "number" && scaling.maxScale > 1)
    || (typeof scaling.minScale === "number" && scaling.minScale > 0)
    || scaling.align === "start"
  )) {
    used.add("scaling");
  }

  if (isSet(layout.controlLabelFontSize) || isSet(layout.controlValueFontSize)
    || isSet(layout.controlFontFamily) || isSet(layout.controlLabelMaxWidth)
    || layout.controls.some((control) => isSet(control.style?.labelFontSize) || isSet(control.style?.valueFontSize))) {
    used.add("text-style");
  }

  return LAYOUT_FEATURES.filter((feature) => used.has(feature));
}

/** Sets `version` and `requires` from what `layout` uses, as saving does. */
export function stampLayoutRequirements(layout: EffectLayout): EffectLayout {
  const requires = layoutRequirements(layout);
  if (requires.length) {
    layout.version = 2;
    layout.requires = requires;
  } else {
    layout.version = 1;
    delete layout.requires;
  }
  return layout;
}
