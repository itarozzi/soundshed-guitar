import { afterEach, describe, expect, it } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import { GenericKnob } from "../ts/knob.js";
import { bindControlImageGroup, bindOverlayImageGroup, renderControlImageGroup, renderOverlayImageGroup } from "../ts/layoutDesigner/controlImageProperties.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { renderCustomLayout } from "../ts/layoutRenderer.js";
import { layoutRequirements, stampLayoutRequirements } from "../ts/layoutRequirements.js";
import { layoutImageIds, renameLayoutImageIds, type EffectLayout, type LayoutControl, type LayoutImageRef, type LayoutRectangleOverlay } from "../ts/layoutTypes.js";
import { uiState } from "../ts/state.js";
import type { GraphNode } from "../ts/types.js";

const IMAGES: LayoutImageRef[] = ["strip", "cap", "skirt", "glare", "on", "off", "rail", "thumb", "logo"].map((id) => ({
  imageId: id,
  fileName: `${id}.png`,
  url: `layout-images/id/${id}?v=1`,
  width: 256,
  height: 256,
}));
const defs: BlendParamDef[] = [
  { key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "amount" },
  { key: "boost", name: "Boost", default: 0, min: 0, max: 1, unit: "toggle" },
  { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, unit: "amount" },
];

function node(bypassed = false): GraphNode {
  return { id: "n", type: "fx", params: { gain: 0.5, boost: 1, mix: 0.25 }, bypassed } as unknown as GraphNode;
}

function layout(controls: LayoutControl[], overlays: LayoutRectangleOverlay[] = []): EffectLayout {
  return { effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls, overlays, textLabels: [] };
}

function render(l: EffectLayout, bypassed = false): HTMLElement {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: IMAGES.map((image) => ({ ...image })) };
  const host = document.createElement("div");
  host.innerHTML = renderCustomLayout(node(bypassed), l, defs);
  return host;
}

afterEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  document.body.replaceChildren();
});

describe("image controls in the params panel", () => {
  it("draws a filmstrip knob at its frame and diameter", () => {
    const host = render(layout([{ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobDiameter: 80, knobImage: { mode: "filmstrip", imageId: "strip", frames: 65 } } }]));
    const knob = host.querySelector<HTMLElement>(".knob")!;
    expect(knob.dataset).toMatchObject({ knobImage: "filmstrip", frames: "65", customKnobImage: "true" });
    expect(knob.style.width).toBe("80px");
    const face = knob.querySelector<HTMLElement>(".custom-knob-face")!;
    expect(face.style.backgroundImage).toContain("layout-images/id/strip");
    expect(face.style.backgroundPosition).toBe("0% 50%");
  });

  it("draws a turning face with a still base and glare", () => {
    const host = render(layout([{ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobImage: { mode: "rotate", imageId: "cap", baseImageId: "skirt", glareImageId: "glare" } } }]));
    const knob = host.querySelector<HTMLElement>(".knob")!;
    expect([...knob.children].map((child) => child.className)).toEqual(["custom-knob-base", "custom-knob-face", "custom-knob-glare", "knob-indicator"]);
    expect(knob.querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(0deg)");
  });

  it("falls back to the knob's own style when its image knob cannot be drawn", () => {
    const host = render(layout([{ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobStyle: "pedal", knobImage: { mode: "rotate", imageId: "not-in-library" } } }]));
    const knob = host.querySelector<HTMLElement>(".knob")!;
    expect(knob.classList.contains("custom-knob-pedal")).toBe(true);
    expect(knob.hasAttribute("data-knob-image")).toBe(false);
  });

  it("zooms a styled knob to its diameter, and leaves a knob without one alone", () => {
    expect(render(layout([{ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobDiameter: 57 } }])).querySelector<HTMLElement>(".knob")!.getAttribute("style")).toBe("zoom: 1.5;");
    expect(render(layout([{ paramKey: "gain", type: "knob", position: { x: 0, y: 0 } }])).querySelector<HTMLElement>(".knob")!.getAttribute("style")).toBe("");
  });

  it("draws an image toggle's pictures at its size", () => {
    const host = render(layout([{ paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style: { toggleWidth: 48, toggleHeight: 96, toggleImage: { onImageId: "on", offImageId: "off" } } }]));
    const label = host.querySelector<HTMLElement>("label.toggle-switch")!;
    expect(label.classList.contains("custom-image-toggle")).toBe(true);
    expect(label.style.height).toBe("96px");
    expect(label.querySelector("input.node-param-toggle")).not.toBeNull();
    expect(label.querySelector(".toggle-slider")).toBeNull();
    expect(label.querySelectorAll(".custom-toggle-image")).toHaveLength(2);
  });

  it("gives an image slider its length and pictures", () => {
    const slider = render(layout([{ paramKey: "mix", type: "slider", position: { x: 0, y: 0 }, style: { sliderLength: 200, sliderImage: { trackImageId: "rail", thumbImageId: "thumb" } } }])).querySelector<HTMLInputElement>("input.custom-layout-slider")!;
    expect(slider.hasAttribute("data-image-slider")).toBe(true);
    expect(slider.style.width).toBe("200px");
    expect(slider.getAttribute("style")).toContain("--slider-thumb-image");
  });

  it("draws an overlay's image only while the overlay is shown", () => {
    const overlays: LayoutRectangleOverlay[] = [{ id: "led", position: { x: 0, y: 0 }, size: { width: 32, height: 32 }, style: { visibilityMode: "enabled", imageId: "logo", imageFit: "cover", backgroundOpacity: 0, borderWidth: 0 } }];
    expect(render(layout([], overlays)).querySelector<HTMLElement>(".custom-layout-overlay")!.style.backgroundSize).toBe("cover");
    expect(render(layout([], overlays), true).querySelector(".custom-layout-overlay")).toBeNull();
  });
});

describe("GenericKnob on an image knob", () => {
  function knobFrom(control: LayoutControl): { knob: GenericKnob; element: HTMLElement } {
    const host = render(layout([control]));
    document.body.appendChild(host);
    const element = host.querySelector<HTMLElement>(".knob")!;
    const knob = new GenericKnob({ knobElement: element, paramId: "t", minValue: 0, maxValue: 1, defaultValue: 0.5, displayFormat: String, sendParameter: false });
    return { knob, element };
  }

  it("moves a filmstrip to the frame for each value", () => {
    const { knob, element } = knobFrom({ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobImage: { mode: "filmstrip", imageId: "strip", frames: 5, columns: 1 } } });
    const face = element.querySelector<HTMLElement>(".custom-knob-face")!;
    knob.setValue(1);
    expect(face.style.backgroundPosition).toBe("0% 100%");
    knob.setValue(0.25);
    expect(face.style.backgroundPosition).toBe("0% 25%");
  });

  it("turns a face over its own range, and an old custom face over -135..135", () => {
    const ranged = knobFrom({ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobImage: { mode: "rotate", imageId: "cap", minAngle: 0, maxAngle: 300 } } });
    ranged.knob.setValue(1);
    expect(ranged.element.querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(300deg)");

    const legacy = knobFrom({ paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobStyle: "custom", knobImageId: "cap" } });
    legacy.knob.setValue(1);
    expect(legacy.element.querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(135deg)");
  });
});

describe("saving an image-control layout", () => {
  it("lists each new feature, and writes what an older app should draw instead", () => {
    const l = layout(
      [
        { paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobStyle: "custom", knobImageId: "old", knobImage: { mode: "filmstrip", imageId: "strip", frames: 64 } } },
        { paramKey: "mix", type: "knob", position: { x: 0, y: 0 }, style: { knobDiameter: 64, knobImage: { mode: "rotate", imageId: "cap" } } },
        { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style: { toggleImage: { onImageId: "on", offImageId: "off" } } },
      ],
      [{ id: "o", position: { x: 0, y: 0 }, size: { width: 8, height: 8 }, style: { imageId: "logo" } }],
    );
    stampLayoutRequirements(l);
    expect(l.version).toBe(2);
    expect(l.requires).toEqual(["control-size", "knob-image", "image-toggle", "image-overlay"]);
    // A filmstrip falls back to a standard knob; a turning face to a custom-image knob turning the same picture.
    expect(l.controls[0].style).toMatchObject({ knobStyle: "default" });
    expect(l.controls[0].style?.knobImageId).toBeUndefined();
    expect(l.controls[1].style).toMatchObject({ knobStyle: "custom", knobImageId: "cap" });
  });

  it("does not count a toggle with only one picture, which is drawn as a plain switch", () => {
    expect(layoutRequirements(layout([{ paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style: { toggleImage: { onImageId: "on" } } }]))).toEqual([]);
  });
});

describe("a layout's image references", () => {
  const l = (): EffectLayout => ({
    ...layout(
      [
        { paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobImageId: "cap", knobImage: { mode: "rotate", imageId: "cap", baseImageId: "skirt", glareImageId: "glare" } } },
        { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 }, style: { toggleImage: { onImageId: "on", offImageId: "off" } } },
        { paramKey: "mix", type: "slider", position: { x: 0, y: 0 }, style: { sliderImage: { trackImageId: "rail", thumbImageId: "thumb" } } },
      ],
      [{ id: "o", position: { x: 0, y: 0 }, size: { width: 8, height: 8 }, style: { imageId: "logo" } }],
    ),
    backgrounds: [{ layerIndex: 0, type: "image", value: "bg" }],
  });

  it("lists every one, each once, as export and the engine's copy on save do", () => {
    expect(layoutImageIds(l())).toEqual(["bg", "cap", "skirt", "glare", "on", "off", "rail", "thumb", "logo"]);
  });

  it("renames every one on import", () => {
    const renamed = l();
    renameLayoutImageIds(renamed, new Map(layoutImageIds(renamed).map((id) => [id, `${id}_2`])));
    expect(layoutImageIds(renamed)).toEqual(["bg_2", "cap_2", "skirt_2", "glare_2", "on_2", "off_2", "rail_2", "thumb_2", "logo_2"]);
  });
});

describe("the Image & Size panel", () => {
  function host(): LayoutPropertiesHost {
    return {
      getLayout: () => null, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => null,
      pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, renderSidebar: () => {}, selectElement: () => {},
      renderImageOptionsHtml: () => `<option value="">-</option><option value="strip">strip</option><option value="logo">logo</option>`,
      browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
    };
  }
  const change = (id: string, value: string) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLSelectElement;
    if (element instanceof HTMLInputElement && element.type === "checkbox") element.checked = value === "true";
    else element.value = value;
    element.dispatchEvent(new Event("change"));
  };

  it("turns a knob into a filmstrip and back, keeping a custom face's picture", () => {
    const control: LayoutControl = { paramKey: "gain", type: "knob", position: { x: 0, y: 0 }, style: { knobStyle: "custom", knobImageId: "strip" } };
    const h = host();
    document.body.innerHTML = renderControlImageGroup(control, false, (purpose, selected) => h.renderImageOptionsHtml(purpose, selected));
    bindControlImageGroup(control, h);
    change("prop-ci-knob-mode", "filmstrip");
    expect(control.style?.knobImage).toEqual({ mode: "filmstrip", imageId: "strip", frames: 64 });

    document.body.innerHTML = renderControlImageGroup(control, false, (purpose, selected) => h.renderImageOptionsHtml(purpose, selected));
    bindControlImageGroup(control, h);
    change("prop-ci-knob-frames", "128");
    change("prop-ci-knob-columns", "8");
    change("prop-ci-knob-diameter", "96");
    expect(control.style).toMatchObject({ knobDiameter: 96, knobImage: { frames: 128, columns: 8 } });

    change("prop-ci-knob-diameter", "");
    change("prop-ci-knob-mode", "");
    expect(control.style?.knobDiameter).toBeUndefined();
    expect(control.style?.knobImage).toBeUndefined();
  });

  it("keeps a toggle's picture pair only while it holds a picture", () => {
    const control: LayoutControl = { paramKey: "boost", type: "toggle", position: { x: 0, y: 0 } };
    const h = host();
    document.body.innerHTML = renderControlImageGroup(control, false, (purpose, selected) => h.renderImageOptionsHtml(purpose, selected));
    bindControlImageGroup(control, h);
    change("prop-ci-toggle-on", "strip");
    expect(control.style?.toggleImage).toEqual({ onImageId: "strip" });
    change("prop-ci-toggle-on", "");
    expect(control.style?.toggleImage).toBeUndefined();
  });

  it("clears a rectangle's fill and border when it first gets an image", () => {
    const overlay: LayoutRectangleOverlay = { id: "o", position: { x: 0, y: 0 }, size: { width: 8, height: 8 }, style: { backgroundOpacity: 0.25, borderWidth: 1 } };
    const h = host();
    document.body.innerHTML = renderOverlayImageGroup(overlay, (purpose, selected) => h.renderImageOptionsHtml(purpose, selected));
    bindOverlayImageGroup(overlay, h);
    change("prop-ci-overlay-image", "logo");
    expect(overlay.style).toMatchObject({ imageId: "logo", backgroundOpacity: 0, borderWidth: 0 });
  });
});
