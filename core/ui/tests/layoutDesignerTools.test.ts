import { afterEach, beforeAll, describe, expect, it } from "vitest";
import { alignedPosition } from "../ts/layoutDesigner/alignTools.js";
import { bindLayoutAssetsGroup, layoutAssets, renderLayoutAssetsGroup } from "../ts/layoutDesigner/layoutAssets.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { renderSizePreview } from "../ts/layoutDesigner/sizePreview.js";
import { layoutImageIds, type EffectLayout, type LayoutImageRef } from "../ts/layoutTypes.js";
import type { ParameterDef } from "../ts/presetV2.js";
import { uiState } from "../ts/state.js";

const library: LayoutImageRef[] = [
  { imageId: "huge", fileName: "huge.png", url: "layout-images/id/huge?v=1", width: 6000, height: 4000, bytes: 9_000_000,
    renditions: [{ edge: 1024, width: 1024, height: 683, url: "layout-images/rendition/huge/1024?v=1" }] },
  { imageId: "strip", fileName: "strip.png", url: "layout-images/id/strip?v=1", width: 128, height: 128 * 200, bytes: 2_000_000 },
  { imageId: "logo", fileName: "logo.png", url: "layout-images/id/logo?v=1", width: 400, height: 100, bytes: 20_000 },
];

function layout(): EffectLayout {
  return {
    effectType: "fx",
    version: 2,
    dimensions: { width: 600, height: 400 },
    backgrounds: [{ layerIndex: 0, type: "image", value: "huge", size: "cover" }],
    controls: [
      { paramKey: "gain", type: "knob", position: { x: 10, y: 10 }, labelOverride: "Drive", style: { knobImage: { mode: "filmstrip", imageId: "strip", frames: 200 } } },
      { paramKey: "level", type: "knob", position: { x: 90, y: 10 }, style: { knobStyle: "custom", knobImageId: "gone" } },
    ],
    overlays: [{ id: "o", position: { x: 0, y: 0 }, size: { width: 100, height: 25 }, style: { imageId: "logo" } }],
    textLabels: [],
  };
}

beforeAll(() => {
  globalThis.ResizeObserver ??= class {
    observe(): void {}
    unobserve(): void {}
    disconnect(): void {}
  } as unknown as typeof ResizeObserver;
});

afterEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  document.body.replaceChildren();
});

describe("images in this layout", () => {
  it("lists each image once with where it is used", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: library };
    const rows = layoutAssets(layout());
    expect(rows.map((row) => [row.imageId, row.uses])).toEqual([
      ["huge", ["Background 1"]],
      ["strip", ["Drive: filmstrip"]],
      ["gone", ["level: knob face"]],
      ["logo", ["Rectangle image"]],
    ]);
  });

  it("flags a large decode, an over-tall filmstrip and a missing image", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: library };
    const byId = Object.fromEntries(layoutAssets(layout()).map((row) => [row.imageId, row]));
    expect(byId.huge.decodedBytes).toBe(6000 * 4000 * 4);
    expect(byId.huge.warnings[0]).toContain("smaller copies are drawn");
    expect(byId.strip.warnings[0]).toContain("columns");
    expect(byId.gone.warnings[0]).toContain("Not in the image library");
    expect(byId.logo.warnings).toEqual([]);
  });

  it("replaces an image everywhere it is used", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: library };
    const l = layout();
    let renders = 0;
    const host = { pushUndoState: () => {}, renderCanvas: () => { renders++; }, renderSidebar: () => {} } as unknown as LayoutPropertiesHost;
    document.body.innerHTML = renderLayoutAssetsGroup(l, () => `<option value="">-</option><option value="logo">logo</option><option value="huge">huge</option>`);
    bindLayoutAssetsGroup(l, host);
    const select = document.querySelector<HTMLSelectElement>('select.layout-asset-replace[data-image-id="huge"]')!;
    select.value = "logo";
    select.dispatchEvent(new Event("change"));
    expect(layoutImageIds(l)).toEqual(["logo", "strip", "gone"]);
    expect(renders).toBe(1);
  });

  it("shows nothing for a layout without images", () => {
    expect(renderLayoutAssetsGroup({ ...layout(), backgrounds: [], controls: [], overlays: [] }, () => "")).toBe("");
  });
});

describe("align to canvas", () => {
  const canvas = { width: 600, height: 400 };
  const size = { width: 100, height: 50 };
  const at = { x: 37, y: 91 };

  it("moves along one axis and keeps the other", () => {
    expect(alignedPosition("left", at, size, canvas)).toEqual({ x: 0, y: 91 });
    expect(alignedPosition("center", at, size, canvas)).toEqual({ x: 250, y: 91 });
    expect(alignedPosition("right", at, size, canvas)).toEqual({ x: 500, y: 91 });
    expect(alignedPosition("top", at, size, canvas)).toEqual({ x: 37, y: 0 });
    expect(alignedPosition("middle", at, size, canvas)).toEqual({ x: 37, y: 175 });
    expect(alignedPosition("bottom", at, size, canvas)).toEqual({ x: 37, y: 350 });
  });
});

describe("preview at panel size", () => {
  const defs: ParameterDef[] = [{ key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "amount" }];

  it("draws the layout as the panel would and reports the scale and copies it loads", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: library };
    const panel = document.createElement("div");
    document.body.appendChild(panel);
    // jsdom has no layout: give the layout's outer box the width a 300 px panel would.
    const original = Object.getOwnPropertyDescriptor(HTMLElement.prototype, "offsetWidth");
    Object.defineProperty(HTMLElement.prototype, "offsetWidth", { configurable: true, get() { return (this as HTMLElement).classList.contains("custom-layout-scale-outer") ? 300 : 0; } });
    try {
      const report = renderSizePreview(panel, layout(), defs);
      expect(report.scale).toBe(0.5);
      expect(report.copies).toEqual(["huge at 1024 px"]);
      expect(panel.querySelector(".default-effect-shell.has-custom-layout .custom-layout-container")).not.toBeNull();
    } finally {
      if (original) Object.defineProperty(HTMLElement.prototype, "offsetWidth", original);
    }
  });
});
