/**
 * The categories layout images are filed under in the designer's image lists: Knob, Background,
 * Switch and Misc ("general").
 *
 * The engine keeps each user image's category (image-categories.json) and sends it as the image's
 * `type`; factory images are filed by the folder they ship in. An image never filed, such as one
 * added before categories, is filed by where the library's layouts use it.
 */

import type { EffectLayout, LayoutImageRef } from "./layoutTypes.js";
import { uiState } from "./state.js";

export type LayoutImageCategory = "knob" | "background" | "switch" | "general";

export const LAYOUT_IMAGE_CATEGORIES: ReadonlyArray<{ id: LayoutImageCategory; label: string }> = [
  { id: "knob", label: "Knob" },
  { id: "background", label: "Background" },
  { id: "switch", label: "Switch" },
  { id: "general", label: "Misc" },
];

export function isLayoutImageCategory(value: unknown): value is LayoutImageCategory {
  return LAYOUT_IMAGE_CATEGORIES.some((category) => category.id === value);
}

export function layoutImageCategoryLabel(category: LayoutImageCategory): string {
  return LAYOUT_IMAGE_CATEGORIES.find((entry) => entry.id === category)?.label ?? "Misc";
}

/** What each image is used as in `layout`: the first use found wins, backgrounds first. */
function addLayoutUsage(layout: EffectLayout, usage: Map<string, LayoutImageCategory>): void {
  const use = (imageId: string | undefined, category: LayoutImageCategory) => {
    if (imageId && !usage.has(imageId)) usage.set(imageId, category);
  };
  for (const bg of layout.backgrounds ?? []) {
    if (bg.type === "image") use(bg.value, "background");
  }
  for (const knobImage of [layout.knobImage, ...(layout.controls ?? []).map((control) => control.style?.knobImage)]) {
    use(knobImage?.imageId, "knob");
    use(knobImage?.baseImageId, "knob");
    use(knobImage?.glareImageId, "knob");
  }
  for (const control of layout.controls ?? []) {
    use(control.style?.knobImageId, "knob");
    use(control.style?.toggleImage?.onImageId, "switch");
    use(control.style?.toggleImage?.offImageId, "switch");
  }
}

/** The category each unfiled image would take from the library's layouts. */
export function layoutImageUsage(): Map<string, LayoutImageCategory> {
  const usage = new Map<string, LayoutImageCategory>();
  for (const entries of Object.values(uiState.layoutLibrary?.byEffectType ?? {})) {
    for (const entry of entries ?? []) {
      if (entry?.layout) addLayoutUsage(entry.layout, usage);
    }
  }
  return usage;
}

/** The category `image` is filed under; an unfiled one by where layouts use it, else Misc. */
export function layoutImageCategory(image: LayoutImageRef, usage: Map<string, LayoutImageCategory> = layoutImageUsage()): LayoutImageCategory {
  if (isLayoutImageCategory(image.type)) return image.type;
  return usage.get(image.imageId) ?? "general";
}
