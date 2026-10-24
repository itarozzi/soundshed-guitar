import { describe, expect, it } from "vitest";

import { MIN_RESOURCE_WIDTH, clampControlWidth, setControlWidth } from "../ts/layoutDesigner/controlWidth.js";
import { renderCustomLayout } from "../ts/layoutRenderer.js";
import type { LayoutResourceControlDef } from "../ts/layoutRenderer.js";
import type { EffectLayout, LayoutControl } from "../ts/layoutTypes.js";
import type { GraphNode } from "../ts/types.js";

const RESOURCE: LayoutResourceControlDef = {
  resourceControlKey: "__resource__:primary:0",
  displayName: "Model",
  resourceType: "nam",
  resourceIndex: 0,
  currentDisplayName: "A model with a very long name indeed",
};

function layoutWith(control: LayoutControl): EffectLayout {
  return {
    effectType: "amp_nam",
    version: 1,
    dimensions: { width: 600, height: 200 },
    backgrounds: [],
    controls: [control],
    textLabels: [],
  };
}

const resourceControl = (): LayoutControl => ({ paramKey: "__resource__:primary:0", bindingType: "resource", type: "dropdown", position: { x: 100, y: 20 } });
const node = { id: "n1", type: "amp_nam", params: {} } as unknown as GraphNode;

describe("a resource selector's width", () => {
  it("is held between the minimum and the room before the layout's right edge", () => {
    const control = resourceControl();
    const layout = layoutWith(control);
    expect(clampControlWidth(layout, control, 40)).toBe(MIN_RESOURCE_WIDTH);
    expect(clampControlWidth(layout, control, 320.4)).toBe(320);
    expect(clampControlWidth(layout, control, 900)).toBe(500);
  });

  it("is stored as the control's size, keeping a height, and clears back to fitting the name", () => {
    const control = { ...resourceControl(), size: { width: 200, height: 64 } };
    setControlWidth(control, 280);
    expect(control.size).toEqual({ width: 280, height: 64 });
    setControlWidth(control, null);
    expect(control.size).toBeUndefined();
    setControlWidth(control, 240);
    expect(control.size).toEqual({ width: 240, height: 0 });
  });

  it("makes the drawn selector take that width; without one it fits the name", () => {
    const sized = { ...resourceControl(), size: { width: 260, height: 0 } };
    const host = document.createElement("div");
    host.innerHTML = renderCustomLayout(node, layoutWith(sized), [], [RESOURCE]);
    const drawn = host.querySelector<HTMLElement>(".custom-layout-control")!;
    expect(drawn.classList.contains("has-width")).toBe(true);
    expect(drawn.style.width).toBe("260px");
    expect(drawn.style.maxWidth).toBe("500px");

    host.innerHTML = renderCustomLayout(node, layoutWith(resourceControl()), [], [RESOURCE]);
    expect(host.querySelector(".custom-layout-control")!.classList.contains("has-width")).toBe(false);
  });
});
