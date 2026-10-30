import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import * as bridge from "../ts/bridge.js";
import { bindControlImageGroup, renderControlImageGroup } from "../ts/layoutDesigner/controlImageProperties.js";
import { bindLayoutAssetsGroup, renderLayoutAssetsGroup } from "../ts/layoutDesigner/layoutAssets.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { cleanCredit } from "../ts/layoutImageCredits.js";
import { layoutImageCategory, layoutImageUsage } from "../ts/layoutImageCategories.js";
import { squareFrameStrip, stripFor, usableStrip } from "../ts/layoutKnobImages.js";
import { renderCustomLayout } from "../ts/layoutRenderer.js";
import { layoutRequirements } from "../ts/layoutRequirements.js";
import { layoutImageIds, type EffectLayout, type LayoutControl, type LayoutImageRef } from "../ts/layoutTypes.js";
import { uiState } from "../ts/state.js";
import type { GraphNode } from "../ts/types.js";

// Strips of frames: knobs that count their own frames, toggles and sliders drawn from one, and
// the licence and credit that travel with an image.

const IMAGES: LayoutImageRef[] = [
  { imageId: "knob101", fileName: "knob101.png", url: "layout-images/id/knob101?v=1", width: 64, height: 6464 },
  { imageId: "row32", fileName: "row32.png", url: "layout-images/id/row32?v=1", width: 2048, height: 64 },
  { imageId: "switch2", fileName: "switch2.png", url: "layout-images/id/switch2?v=1", width: 64, height: 128 },
  { imageId: "slider", fileName: "slider.png", url: "layout-images/id/slider?v=1", width: 32, height: 12800 },
];
const defs: BlendParamDef[] = [
  { key: "boost", name: "Boost", default: 0, min: 0, max: 1, unit: "toggle" },
  { key: "level", name: "Level", default: 0.5, min: 0, max: 10, unit: "amount" },
];
const node = { id: "n", type: "fx", params: { boost: 1, level: 5 } } as unknown as GraphNode;

function layout(controls: LayoutControl[]): EffectLayout {
  return { effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls, textLabels: [] };
}

function mount(html: string): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = html;
  document.body.replaceChildren(host);
  return host;
}

function hostFor(): LayoutPropertiesHost {
  return {
    getLayout: () => null, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => null,
    pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, selectElement: () => {},
    renderSidebar: () => {}, renderImageOptionsHtml: () => "",
    browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
  } as unknown as LayoutPropertiesHost;
}

const options = () => `<option value=""></option>${IMAGES.map((img) => `<option value="${img.imageId}">${img.imageId}</option>`).join("")}`;

function change(id: string, value: string): void {
  const element = document.getElementById(id) as HTMLInputElement | HTMLSelectElement;
  element.value = value;
  element.dispatchEvent(new Event("change"));
}

beforeEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: IMAGES.map((image) => ({ ...image })) };
});

afterEach(() => {
  vi.restoreAllMocks();
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  document.body.replaceChildren();
});

describe("strip shapes", () => {
  it("count the frames of a strip of square frames, in a column or a row", () => {
    expect(squareFrameStrip(64, 6464)).toEqual({ frames: 101, columns: 1 });
    expect(squareFrameStrip(2048, 64)).toEqual({ frames: 32, columns: 32 });
    expect(squareFrameStrip(64, 100)).toBeNull();
    expect(squareFrameStrip(64, 64)).toBeNull();
    expect(squareFrameStrip(undefined, 64)).toBeNull();
  });

  it("lay out a strip by its shape, with one frame's height over its width", () => {
    expect(stripFor("s", 64, 128, 2)).toEqual({ imageId: "s", frames: 2, aspect: 1 });
    expect(stripFor("s", 128, 64, 2)).toEqual({ imageId: "s", frames: 2, columns: 2, aspect: 1 });
    expect(stripFor("s", 32, 12800, 100)).toEqual({ imageId: "s", frames: 100, aspect: 4 });
    expect(stripFor("s", undefined, undefined, 1)).toEqual({ imageId: "s", frames: 2 });
    expect(usableStrip({ imageId: "s", frames: 1 })).toBeNull();
  });
});

describe("drawing", () => {
  it("a strip toggle shows its first frame off and its last on, at the frame's shape", () => {
    const host = mount(renderCustomLayout(node, layout([
      { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style: { toggleImage: { strip: { imageId: "switch2", frames: 2, aspect: 1 } } } },
    ]), defs));
    const toggle = host.querySelector<HTMLElement>("label.custom-image-toggle")!;
    expect([toggle.style.width, toggle.style.height]).toEqual(["36px", "36px"]);
    expect(host.querySelector<HTMLElement>(".custom-toggle-off")!.style.backgroundPosition).toBe("0% 0%");
    expect(host.querySelector<HTMLElement>(".custom-toggle-on")!.style.backgroundPosition).toBe("0% 100%");
  });

  it("a strip slider shows the frame for its value, runs up a tall frame, and follows the input", () => {
    const host = mount(renderCustomLayout(node, layout([
      { paramKey: "level", type: "slider", position: { x: 0, y: 0 }, style: { sliderLength: 120, sliderImage: { strip: { imageId: "slider", frames: 101, aspect: 4 } } } },
    ]), defs));
    const wrapper = host.querySelector<HTMLElement>(".custom-slider-strip")!;
    expect(wrapper.classList.contains("is-vertical")).toBe(true);
    expect([wrapper.style.width, wrapper.style.height]).toEqual(["30px", "120px"]);
    const frame = wrapper.querySelector<HTMLElement>(".custom-slider-strip-frame")!;
    expect(frame.style.backgroundPosition).toBe("0% 50%");
    const input = wrapper.querySelector<HTMLInputElement>("input[data-slider-strip]")!;
    input.value = "10";
    input.dispatchEvent(new Event("input", { bubbles: true }));
    expect(frame.style.backgroundPosition).toBe("0% 100%");
  });

  it("strips are newer features, images of the layout, and file a switch's picture under Switch", () => {
    const l = layout([
      { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style: { toggleImage: { strip: { imageId: "switch2", frames: 2 } } } },
      { paramKey: "level", type: "slider", position: { x: 0, y: 0 }, style: { sliderImage: { strip: { imageId: "slider", frames: 101 } } } },
    ]);
    expect(layoutRequirements(l)).toEqual(["image-toggle", "image-slider"]);
    expect(layoutImageIds(l)).toEqual(["switch2", "slider"]);
    uiState.layoutLibrary!.byEffectType = { fx: [{ layout: l, isDefault: true, layoutId: "l" }] };
    expect(layoutImageCategory({ imageId: "switch2" }, layoutImageUsage())).toBe("switch");
  });
});

describe("the designer", () => {
  it("a filmstrip knob counts its frames from the strip it is given", () => {
    const control: LayoutControl = { paramKey: "level", type: "knob", position: { x: 0, y: 0 }, style: { knobImage: { mode: "filmstrip", imageId: "", frames: 64 } } };
    mount(renderControlImageGroup(control, false, options));
    bindControlImageGroup(control, hostFor());
    change("prop-ci-knob-image", "knob101");
    expect(control.style?.knobImage).toMatchObject({ imageId: "knob101", frames: 101 });
    expect(control.style?.knobImage?.columns).toBeUndefined();
    change("prop-ci-knob-image", "row32");
    expect(control.style?.knobImage).toMatchObject({ imageId: "row32", frames: 32, columns: 32 });
  });

  it("a toggle takes a switch strip, and its frames can be set", () => {
    const control: LayoutControl = { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 } };
    mount(renderControlImageGroup(control, false, options));
    bindControlImageGroup(control, hostFor());
    change("prop-ci-toggle-strip", "switch2");
    expect(control.style?.toggleImage?.strip).toEqual({ imageId: "switch2", frames: 2, aspect: 1 });

    mount(renderControlImageGroup(control, false, options));
    bindControlImageGroup(control, hostFor());
    change("prop-ci-toggle-strip-frames", "4");
    expect(control.style?.toggleImage?.strip).toEqual({ imageId: "switch2", frames: 4, aspect: 0.5 });
    change("prop-ci-toggle-strip", "");
    expect(control.style?.toggleImage).toBeUndefined();
  });

  it("a slider strip keeps its frames until they are set, and takes the frame's shape", () => {
    const control: LayoutControl = { paramKey: "level", type: "slider", position: { x: 0, y: 0 } };
    mount(renderControlImageGroup(control, false, options));
    bindControlImageGroup(control, hostFor());
    change("prop-ci-slider-strip", "slider");
    expect(control.style?.sliderImage?.strip).toEqual({ imageId: "slider", frames: 64, aspect: 12800 / 64 / 32 });
    mount(renderControlImageGroup(control, false, options));
    bindControlImageGroup(control, hostFor());
    change("prop-ci-slider-strip-frames", "100");
    expect(control.style?.sliderImage?.strip).toEqual({ imageId: "slider", frames: 100, aspect: 4 });
  });
});

describe("licence and credit", () => {
  it("a credit line is one line of at most 300 bytes", () => {
    expect(cleanCredit("  Knob by\nAda  ")).toBe("Knob by Ada");
    expect(new TextEncoder().encode(cleanCredit("é".repeat(400))).length).toBeLessThanOrEqual(300);
    expect(cleanCredit(42)).toBe("");
  });

  it("Images in this Layout records an image's licence and credit, but not a factory image's", () => {
    const post = vi.spyOn(bridge, "postMessage").mockImplementation(() => {});
    uiState.layoutLibrary!.images.push({ imageId: "factory", fileName: "f.svg", url: "layout-images/id/factory?v=1", factory: true });
    const l = layout([
      { paramKey: "level", type: "knob", position: { x: 0, y: 0 }, style: { knobImage: { mode: "filmstrip", imageId: "knob101", frames: 101, baseImageId: "factory" } } },
    ]);
    const host = mount(renderLayoutAssetsGroup(l, () => ""));
    expect(host.querySelectorAll(".layout-asset-license")).toHaveLength(1);
    bindLayoutAssetsGroup(l, hostFor(), host);
    const license = host.querySelector<HTMLSelectElement>(".layout-asset-license")!;
    const credit = host.querySelector<HTMLInputElement>(".layout-asset-credit")!;
    license.value = "CC-BY-4.0";
    credit.value = "Knob by Ada";
    credit.dispatchEvent(new Event("change"));
    expect(post).toHaveBeenCalledWith({ type: "setLayoutImageCredit", imageId: "knob101", license: "CC-BY-4.0", credit: "Knob by Ada" });
    expect(uiState.layoutLibrary!.images.find((img) => img.imageId === "knob101")).toMatchObject({ license: "CC-BY-4.0", credit: "Knob by Ada" });
  });
});
