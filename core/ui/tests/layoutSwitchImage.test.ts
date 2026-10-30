import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import { usableToggleImage, withLayoutSwitchImage } from "../ts/layoutControlImages.js";
import { CanvasProperties } from "../ts/layoutDesigner/canvasProperties.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { layoutImageCategory, layoutImageUsage } from "../ts/layoutImageCategories.js";
import { renderCustomLayout, renderCustomLayoutBackdrop } from "../ts/layoutRenderer.js";
import { layoutRequirements } from "../ts/layoutRequirements.js";
import { layoutImageIds, type EffectLayout, type LayoutControl, type LayoutImageRef } from "../ts/layoutTypes.js";
import { buildDefaultParamControlsHtml } from "../ts/parameterControlMarkup.js";
import { uiState } from "../ts/state.js";
import type { GraphNode } from "../ts/types.js";

// The layout's switch: every toggle without images of its own is drawn from it.

const IMAGES: LayoutImageRef[] = ["off", "on", "own-off", "own-on"].map((id) => ({
  imageId: id, fileName: `${id}.png`, url: `layout-images/id/${id}?v=1`, width: 64, height: 96,
}));
IMAGES.push({ imageId: "pair", fileName: "pair.png", url: "layout-images/id/pair?v=1", width: 64, height: 128 });
const defs: BlendParamDef[] = [{ key: "boost", name: "Boost", default: 0, min: 0, max: 1, unit: "toggle" }];
const node = { id: "n", type: "fx", params: { boost: 1 } } as unknown as GraphNode;
const SWITCH = { offImageId: "off", onImageId: "on", aspect: 1.5 };

function layout(extra: Partial<EffectLayout> = {}): EffectLayout {
  return {
    effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls: [], textLabels: [],
    switchImage: { ...SWITCH },
    ...extra,
  };
}

function toggle(style?: LayoutControl["style"]): LayoutControl {
  return { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style };
}

function mount(html: string): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = html;
  document.body.replaceChildren(host);
  return host;
}

const background = (el: Element | null) => (el as HTMLElement | null)?.style.backgroundImage ?? "";

beforeEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: IMAGES.map((image) => ({ ...image })) };
});

afterEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  document.body.replaceChildren();
});

describe("which toggles take the layout's switch", () => {
  it("one with no images of its own, at the switch's size where it sets none", () => {
    expect(usableToggleImage(SWITCH)).toBe(true);
    expect(usableToggleImage({ offImageId: "off" })).toBe(false);
    expect(usableToggleImage({ strip: { imageId: "pair", frames: 2 } })).toBe(true);
    expect(withLayoutSwitchImage(undefined, { ...SWITCH, width: 48 })).toMatchObject({ toggleImage: { offImageId: "off" }, toggleWidth: 48 });
    expect(withLayoutSwitchImage({ toggleWidth: 30 }, { ...SWITCH, width: 48 })?.toggleWidth).toBe(30);
    const own = { offImageId: "own-off", onImageId: "own-on" };
    expect(withLayoutSwitchImage({ toggleImage: own }, SWITCH)?.toggleImage).toBe(own);
    expect(withLayoutSwitchImage({}, { onImageId: "on" })?.toggleImage).toBeUndefined();
  });
});

describe("drawing", () => {
  it("a positioned toggle draws the layout's switch, at its picture's shape, unless it has its own", () => {
    const host = mount(renderCustomLayout(node, layout({ controls: [toggle()] }), defs));
    const label = host.querySelector<HTMLElement>("label.custom-image-toggle")!;
    expect([label.style.width, label.style.height]).toEqual(["36px", "54px"]);
    expect(background(host.querySelector(".custom-toggle-off"))).toContain("/off");
    expect(background(host.querySelector(".custom-toggle-on"))).toContain("/on");

    const own = mount(renderCustomLayout(node, layout({ controls: [toggle({ toggleImage: { offImageId: "own-off", onImageId: "own-on" } })] }), defs));
    expect(background(own.querySelector(".custom-toggle-off"))).toContain("own-off");
  });

  it("the default controls' toggles draw it in place of the standard switch, the input kept", () => {
    const html = buildDefaultParamControlsHtml([{ key: "boost", name: "Boost", default: 1, min: 0, max: 1, unit: "toggle" }], "n");
    const host = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, switchImage: { strip: { imageId: "pair", frames: 2, aspect: 1 }, width: 40 } }), html));
    const label = host.querySelector<HTMLElement>("label.toggle-switch")!;
    expect(label.classList.contains("custom-image-toggle")).toBe(true);
    expect(label.querySelector(".toggle-slider")).toBeNull();
    expect(label.querySelector("input.node-param-toggle")).not.toBeNull();
    expect([label.style.width, label.style.height]).toEqual(["40px", "40px"]);
    expect(host.querySelector<HTMLElement>(".custom-toggle-on")!.style.backgroundPosition).toBe("0% 100%");

    const plain = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, switchImage: undefined }), html));
    expect(plain.querySelector(".toggle-slider")).not.toBeNull();
  });

  it("a shadow follows the switch's size", () => {
    const html = buildDefaultParamControlsHtml([{ key: "boost", name: "Boost", default: 1, min: 0, max: 1, unit: "toggle" }], "n");
    const host = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, controlShadow: {} }), html));
    // A 36 x 54 switch: a pill as wide as the switch, stroked 36 px.
    expect(host.querySelector("label.toggle-switch > svg.custom-control-shadow polygon")!.getAttribute("stroke-width")).toBe("36");
  });
});

describe("as stored and edited", () => {
  it("is a newer feature, its images are the layout's, and they file under Switch", () => {
    const l = layout({ switchImage: { ...SWITCH, strip: { imageId: "pair", frames: 2 } } });
    expect(layoutRequirements(l)).toEqual(["switch-image"]);
    expect(layoutImageIds(l)).toEqual(["off", "on", "pair"]);
    uiState.layoutLibrary!.byEffectType = { fx: [{ layout: l, isDefault: true, layoutId: "l" }] };
    expect(layoutImageCategory({ imageId: "off" }, layoutImageUsage())).toBe("switch");
  });

  it("the Switches group sets the pictures, their shape, a strip and a size, and clearing them drops it", () => {
    const l = layout({ switchImage: undefined });
    const sidebar = document.createElement("div");
    document.body.replaceChildren(sidebar);
    const host: LayoutPropertiesHost = {
      getLayout: () => l, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => sidebar,
      pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, selectElement: () => {},
      renderSidebar: () => panel.renderCanvasProperties(),
      renderImageOptionsHtml: () => `<option value=""></option>${IMAGES.map((img) => `<option value="${img.imageId}">${img.imageId}</option>`).join("")}`,
      browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
    };
    const panel: CanvasProperties = new CanvasProperties(host);
    panel.renderCanvasProperties();
    const change = (id: string, value: string) => {
      const el = document.getElementById(id) as HTMLInputElement | HTMLSelectElement;
      el.value = value;
      el.dispatchEvent(new Event("change"));
    };
    change("prop-layout-switch-off", "off");
    change("prop-layout-switch-on", "on");
    expect(l.switchImage).toEqual({ offImageId: "off", onImageId: "on", aspect: 1.5 });
    change("prop-layout-switch-strip", "pair");
    change("prop-layout-switch-width", "48");
    expect(l.switchImage).toMatchObject({ strip: { imageId: "pair", frames: 2, aspect: 1 }, width: 48 });
    change("prop-layout-switch-strip", "");
    change("prop-layout-switch-off", "");
    change("prop-layout-switch-on", "");
    expect(l.switchImage).toBeUndefined();
  });
});
