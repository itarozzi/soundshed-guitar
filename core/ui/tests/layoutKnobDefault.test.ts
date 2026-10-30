import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import { GenericKnob } from "../ts/knob.js";
import { withLayoutKnobImage } from "../ts/layoutControlImages.js";
import { CanvasProperties } from "../ts/layoutDesigner/canvasProperties.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { renderCustomLayout, renderCustomLayoutBackdrop } from "../ts/layoutRenderer.js";
import { layoutRequirements } from "../ts/layoutRequirements.js";
import { layoutImageIds, renameLayoutImageIds, type EffectLayout, type LayoutControl, type LayoutImageRef } from "../ts/layoutTypes.js";
import { buildDefaultParamControlsHtml } from "../ts/parameterControlMarkup.js";
import { uiState } from "../ts/state.js";
import type { GraphNode } from "../ts/types.js";

// The layout's own knob image: every knob without one of its own is drawn from it.

const IMAGES: LayoutImageRef[] = ["cap", "skirt", "strip", "own"].map((id) => ({
  imageId: id,
  fileName: `${id}.png`,
  url: `layout-images/id/${id}?v=1`,
  width: 256,
  height: 256,
}));
const defs: BlendParamDef[] = [
  { key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "amount" },
  { key: "tone", name: "Tone", default: 0.5, min: 0, max: 1, unit: "amount" },
  { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, unit: "amount" },
];
const node = { id: "n", type: "fx", params: { gain: 1, tone: 0.5, mix: 0.5 } } as unknown as GraphNode;

function layout(extra: Partial<EffectLayout> = {}): EffectLayout {
  return {
    effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls: [], textLabels: [],
    knobImage: { mode: "rotate", imageId: "cap", baseImageId: "skirt" },
    ...extra,
  };
}

function mount(html: string): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = html;
  document.body.replaceChildren(host);
  return host;
}

function knob(paramKey: string, style?: LayoutControl["style"]): LayoutControl {
  return { paramKey, type: "knob", position: { x: 0, y: 0 }, style };
}

beforeEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: IMAGES.map((image) => ({ ...image })) };
});

afterEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  document.body.replaceChildren();
});

describe("which knobs take the layout's knob image", () => {
  const image = { mode: "rotate" as const, imageId: "cap" };

  it("a knob with no image of its own and the Default style, or none", () => {
    expect(withLayoutKnobImage(undefined, image)?.knobImage).toBe(image);
    expect(withLayoutKnobImage({ knobStyle: "default", knobDiameter: 64 }, image)).toMatchObject({ knobImage: image, knobDiameter: 64 });
  });

  it("not a knob with its own image or another style, and nothing when the layout's is unusable", () => {
    const own = { mode: "filmstrip" as const, imageId: "own", frames: 32 };
    expect(withLayoutKnobImage({ knobImage: own }, image)?.knobImage).toBe(own);
    for (const knobStyle of ["pedal", "amp", "minimal", "custom"]) {
      expect(withLayoutKnobImage({ knobStyle }, image)?.knobImage).toBeUndefined();
    }
    expect(withLayoutKnobImage({}, { mode: "rotate" })?.knobImage).toBeUndefined();
  });
});

describe("positioned controls", () => {
  it("draw the layout's knob image unless they set their own image or style", () => {
    const host = mount(renderCustomLayout(node, layout({
      controls: [
        knob("gain", { knobStyle: "default", knobDiameter: 64 }),
        knob("tone", { knobStyle: "pedal" }),
        knob("mix", { knobImage: { mode: "filmstrip", imageId: "own", frames: 16 } }),
      ],
    }), defs));
    const [gain, tone, mix] = [...host.querySelectorAll<HTMLElement>(".node-param-knob")];
    expect(gain.dataset).toMatchObject({ knobImage: "rotate", customKnobImage: "true" });
    expect(gain.style.width).toBe("64px");
    expect([...gain.children].map((child) => child.className)).toEqual(["custom-knob-base", "custom-knob-face", "knob-indicator"]);
    expect(gain.querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(135deg)");
    expect(tone.hasAttribute("data-knob-image")).toBe(false);
    expect(tone.classList.contains("custom-knob-pedal")).toBe(true);
    expect(mix.dataset.knobImage).toBe("filmstrip");
  });
});

describe("default controls", () => {
  it("draw each standard knob from the layout's knob image, turned to its value, and leave toggles alone", () => {
    const host = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true }), buildDefaultParamControlsHtml([
      { key: "gain", name: "Gain", default: 1, min: 0, max: 1, unit: "amount" },
      { key: "freq", name: "Freq", default: 100, min: 10, max: 1000, unit: "hz", taper: "log" },
      { key: "boost", name: "Boost", default: 1, min: 0, max: 1, unit: "toggle" },
    ], "n")));
    const knobs = [...host.querySelectorAll<HTMLElement>(".custom-layout-default-controls .node-param-knob")];
    expect(knobs).toHaveLength(2);
    for (const k of knobs) {
      expect(k.dataset).toMatchObject({ customKnobImage: "true", knobImage: "rotate", minAngle: "-135", maxAngle: "135" });
      expect(k.style.width).toBe("38px");
      expect(k.style.backgroundImage).toBe("none");
      expect(k.querySelector<HTMLElement>(".custom-knob-face")!.style.backgroundImage).toContain("layout-images/id/cap");
      expect(k.lastElementChild!.className).toBe("knob-indicator");
    }
    expect(knobs[0].querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(135deg)");
    // 100 Hz is halfway along a log 10..1000 Hz knob.
    expect(knobs[1].querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(0deg)");
    expect(host.querySelector(".toggle-switch .custom-knob-face")).toBeNull();
  });

  it("turn the face as the knob is bound and moved", () => {
    const host = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, knobImage: { mode: "filmstrip", imageId: "strip", frames: 5 } }),
      buildDefaultParamControlsHtml([{ key: "gain", name: "Gain", default: 0, min: 0, max: 1, unit: "amount" }], "n")));
    const element = host.querySelector<HTMLElement>(".node-param-knob")!;
    const bound = new GenericKnob({ knobElement: element, paramId: "n_gain", minValue: 0, maxValue: 1, defaultValue: 0, displayFormat: String, sendParameter: false });
    bound.setValue(0.75);
    expect(element.querySelector<HTMLElement>(".custom-knob-face")!.style.backgroundPosition).toBe("0% 75%");
  });

  it("are unchanged without a usable layout knob image", () => {
    const html = buildDefaultParamControlsHtml([{ key: "gain", name: "Gain", default: 0, min: 0, max: 1, unit: "amount" }], "n");
    const host = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, knobImage: undefined }), html));
    expect(host.querySelector(".node-param-knob")!.hasAttribute("data-custom-knob-image")).toBe(false);
    const missing = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, knobImage: { mode: "rotate", imageId: "not-in-library" } }), html));
    expect(missing.querySelector(".node-param-knob")!.hasAttribute("data-custom-knob-image")).toBe(false);
  });
});

describe("the layout's knob image as stored", () => {
  it("is a newer feature, and its images are the layout's (exported, renamed on import)", () => {
    const l = layout();
    expect(layoutRequirements(l)).toEqual(["knob-image"]);
    expect(layoutImageIds(l)).toEqual(["cap", "skirt"]);
    renameLayoutImageIds(l, new Map([["cap", "cap_2"], ["skirt", "skirt_2"]]));
    expect(l.knobImage).toEqual({ mode: "rotate", imageId: "cap_2", baseImageId: "skirt_2" });
    expect(layoutRequirements(layout({ knobImage: undefined }))).toEqual([]);
  });
});

describe("the canvas panel's Knobs group", () => {
  function setup(l: EffectLayout): void {
    const sidebar = document.createElement("div");
    document.body.replaceChildren(sidebar);
    const host: LayoutPropertiesHost = {
      getLayout: () => l, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => sidebar,
      pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, selectElement: () => {},
      renderSidebar: () => panel.renderCanvasProperties(),
      renderImageOptionsHtml: () => `<option value="">-</option><option value="cap">cap</option><option value="strip">strip</option>`,
      browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
    };
    const panel = new CanvasProperties(host);
    panel.renderCanvasProperties();
  }
  const change = (id: string, value: string) => {
    const element = document.getElementById(id) as HTMLSelectElement | HTMLInputElement;
    element.value = value;
    element.dispatchEvent(new Event("change"));
  };

  it("sets the layout's knob image, and Standard removes it", () => {
    const l = layout({ knobImage: undefined });
    setup(l);
    change("prop-layout-knob-mode", "filmstrip");
    change("prop-layout-knob-image", "strip");
    change("prop-layout-knob-frames", "128");
    expect(l.knobImage).toEqual({ mode: "filmstrip", imageId: "strip", frames: 128 });
    change("prop-layout-knob-mode", "");
    expect(l.knobImage).toBeUndefined();
  });
});
