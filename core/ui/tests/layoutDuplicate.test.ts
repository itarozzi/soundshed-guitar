import { describe, expect, it } from "vitest";

import { duplicateLayout } from "../ts/layoutTypes.js";
import type { EffectLayout } from "../ts/layoutTypes.js";

function layout(name: string): EffectLayout {
  return {
    effectType: "amp_nam",
    layoutId: "layout-original",
    version: 1,
    name,
    dimensions: { width: 400, height: 200 },
    backgrounds: [{ layerIndex: 0, type: "image", value: "img-1" }],
    controls: [{ paramKey: "gain", type: "knob", position: { x: 10, y: 10 } }],
    textLabels: [],
    createdAt: "2026-01-01T00:00:00.000Z",
    modifiedAt: "2026-02-01T00:00:00.000Z",
  };
}

describe("duplicating a layout", () => {
  it("gives the copy its own id and leaves the original as it was", () => {
    const original = layout("Plexi");
    const before = JSON.stringify(original);
    const copy = duplicateLayout(original);

    expect(copy.layoutId).not.toBe("layout-original");
    expect(copy.name).toBe("Plexi (copy)");
    expect(copy.modifiedAt).toBeUndefined();
    expect(copy.createdAt).not.toBe(original.createdAt);
    // The design itself is copied, images by the same id.
    expect(copy.backgrounds).toEqual(original.backgrounds);
    expect(copy.controls).toEqual(original.controls);
    copy.controls[0].position.x = 99;
    expect(JSON.stringify(original)).toBe(before);
  });

  it("picks a copy name that is not taken, and does not stack copy suffixes", () => {
    expect(duplicateLayout(layout("Plexi"), ["Plexi", "Plexi (copy)"]).name).toBe("Plexi (copy 2)");
    expect(duplicateLayout(layout("Plexi (copy)"), ["Plexi (copy)", "Plexi (copy 2)"]).name).toBe("Plexi (copy 3)");
    expect(duplicateLayout(layout("")).name).toBe("Layout (copy)");
  });
});
