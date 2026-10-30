import { afterEach, describe, expect, it } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import { CanvasProperties } from "../ts/layoutDesigner/canvasProperties.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { bindControlShadowRow, renderControlShadowRow } from "../ts/layoutDesigner/shadowProperties.js";
import { renderCustomLayout, renderCustomLayoutBackdrop } from "../ts/layoutRenderer.js";
import { layoutRequirements } from "../ts/layoutRequirements.js";
import { CONTROL_SHADOW_DEFAULTS, knobShadowSvg, toggleShadowSvg, usableControlShadow } from "../ts/layoutShadows.js";
import type { EffectLayout, LayoutControl } from "../ts/layoutTypes.js";
import { buildDefaultParamControlsHtml } from "../ts/parameterControlMarkup.js";
import type { GraphNode } from "../ts/types.js";

// Shadows the knobs and toggles cast from the layout's one light, each as tall as it is wide.

const defs: BlendParamDef[] = [
  { key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "amount" },
  { key: "boost", name: "Boost", default: 0, min: 0, max: 1, unit: "toggle" },
  { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, unit: "amount" },
];
const node = { id: "n", type: "fx", params: { gain: 0.5, boost: 1, mix: 0.5 } } as unknown as GraphNode;

function layout(extra: Partial<EffectLayout> = {}): EffectLayout {
  return {
    effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls: [], textLabels: [],
    controlShadow: {},
    ...extra,
  };
}

function control(paramKey: string, type: LayoutControl["type"], style?: LayoutControl["style"]): LayoutControl {
  return { paramKey, type, position: { x: 0, y: 0 }, style };
}

function mount(html: string): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = html;
  document.body.replaceChildren(host);
  return host;
}

/** The polygon's points: the footprint's start and where the sweep ends. */
function points(svg: Element): number[][] {
  return (svg.querySelector("polygon")!.getAttribute("points") ?? "").split(" ").map((pair) => pair.split(",").map(Number));
}

afterEach(() => document.body.replaceChildren());

describe("the layout's light", () => {
  it("is off without controlShadow, the defaults for {}, and each field kept in range", () => {
    expect(usableControlShadow(undefined)).toBeNull();
    expect(usableControlShadow([])).toBeNull();
    expect(usableControlShadow({})).toEqual(CONTROL_SHADOW_DEFAULTS);
    expect(usableControlShadow({ angle: -90, elevation: 5, softness: 3, opacity: -1 })).toEqual({ angle: 270, elevation: 15, softness: 1, opacity: 0 });
  });

  it("sweeps a knob's footprint away from the light, as far as the knob is tall over tan(elevation)", () => {
    // Light from the top left at 45 degrees: a 38 px knob casts 38 px towards the bottom right.
    mount(knobShadowSvg({ ...CONTROL_SHADOW_DEFAULTS, angle: 315, elevation: 45 }, 38));
    const svg = document.querySelector("svg.custom-control-shadow")!;
    const [[x1, y1], [x2, y2]] = points(svg);
    expect([x1, y1]).toEqual([19, 19]);
    expect(x2 - x1).toBeCloseTo(26.87, 1);
    expect(y2 - y1).toBeCloseTo(26.87, 1);
    // A round footprint: the line is stroked as wide as the knob, with round ends.
    expect(svg.querySelector("polygon")!.getAttribute("stroke-width")).toBe("38");

    // A light from the right, higher up: a shorter shadow to the left.
    mount(knobShadowSvg({ ...CONTROL_SHADOW_DEFAULTS, angle: 90, elevation: 60 }, 38));
    const ends = points(document.querySelector("svg")!);
    const start = ends.find(([x]) => x === 19)!;
    const end = ends.find(([x]) => x !== 19)!;
    expect(start[0] - end[0]).toBeCloseTo(38 / Math.tan(Math.PI / 3), 1);
    expect(end[1]).toBeCloseTo(19, 5);
  });

  it("gives a toggle a pill footprint that stands as tall as it is narrow", () => {
    mount(toggleShadowSvg({ ...CONTROL_SHADOW_DEFAULTS, angle: 0, elevation: 45 }, 36, 20));
    const svg = document.querySelector("svg")!;
    expect(svg.querySelector("polygon")!.getAttribute("stroke-width")).toBe("20");
    // Light from the top: the inset ends sweep 20 px straight down.
    expect(points(svg).map(([, y]) => y).sort((a, b) => a - b)).toEqual([10, 10, 30, 30]);
  });
});

describe("positioned controls", () => {
  it("knobs and toggles cast a shadow behind everything they draw, sliders and switched-off controls none", () => {
    const host = mount(renderCustomLayout(node, layout({
      controls: [control("gain", "knob"), control("boost", "toggle"), control("mix", "slider")],
    }), defs));
    const [gain, boost, mix] = [...host.querySelectorAll<HTMLElement>(".custom-layout-control")];
    const knob = gain.querySelector(".node-param-knob")!;
    expect(knob.firstElementChild?.matches("svg.custom-control-shadow")).toBe(true);
    expect(gain.classList.contains("has-control-shadow")).toBe(true);
    const toggle = boost.querySelector("label.toggle-switch")!;
    expect(toggle.firstElementChild?.matches("svg.custom-control-shadow")).toBe(true);
    // The switch's checked styling still finds its slider next to the input.
    expect(toggle.querySelector("input + .toggle-slider")).not.toBeNull();
    expect(mix.querySelector(".custom-control-shadow")).toBeNull();
    expect(mix.classList.contains("has-control-shadow")).toBe(false);

    const off = mount(renderCustomLayout(node, layout({ controls: [control("gain", "knob", { castShadow: false })] }), defs));
    expect(off.querySelector(".custom-control-shadow")).toBeNull();
    const none = mount(renderCustomLayout(node, layout({ controlShadow: undefined, controls: [control("gain", "knob")] }), defs));
    expect(none.querySelector(".custom-control-shadow, .has-control-shadow")).toBeNull();
  });

  it("an image knob's shadow is as wide as its diameter, a styled knob's as its 38 px design", () => {
    const host = mount(renderCustomLayout(node, layout({
      controls: [control("gain", "knob", { knobDiameter: 64, knobStyle: "pedal" })],
    }), defs));
    expect(host.querySelector("svg.custom-control-shadow polygon")!.getAttribute("stroke-width")).toBe("38");
  });
});

describe("default controls", () => {
  it("each knob and toggle the params panel builds casts one, its parent isolated", () => {
    const html = buildDefaultParamControlsHtml([
      { key: "gain", name: "Gain", default: 1, min: 0, max: 1, unit: "amount" },
      { key: "boost", name: "Boost", default: 1, min: 0, max: 1, unit: "toggle" },
    ], "n");
    const host = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true }), html));
    const knob = host.querySelector(".node-param-knob")!;
    expect(knob.firstElementChild?.matches("svg.custom-control-shadow")).toBe(true);
    expect(knob.parentElement!.classList.contains("has-control-shadow")).toBe(true);
    expect(host.querySelector("label.toggle-switch > svg.custom-control-shadow")).not.toBeNull();
    const plain = mount(renderCustomLayoutBackdrop(node, layout({ useDefaultControls: true, controlShadow: undefined }), html));
    expect(plain.querySelector(".custom-control-shadow")).toBeNull();
  });
});

describe("as stored and edited", () => {
  it("is a newer feature", () => {
    expect(layoutRequirements(layout())).toEqual(["control-shadow"]);
    expect(layoutRequirements(layout({ controlShadow: undefined }))).toEqual([]);
  });

  function hostFor(l: EffectLayout, sidebar: HTMLElement, rerender: () => void): LayoutPropertiesHost {
    return {
      getLayout: () => l, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => sidebar,
      pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, selectElement: () => {},
      renderSidebar: rerender, renderImageOptionsHtml: () => "",
      browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
    };
  }
  const change = (id: string, value: string | boolean) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLSelectElement;
    if (typeof value === "boolean") (element as HTMLInputElement).checked = value;
    else element.value = value;
    element.dispatchEvent(new Event("change"));
  };

  it("the Shadows group turns the light on with its defaults, sets it, and turns it off", () => {
    const l = layout({ controlShadow: undefined });
    const sidebar = document.createElement("div");
    document.body.replaceChildren(sidebar);
    const panel: CanvasProperties = new CanvasProperties(hostFor(l, sidebar, () => panel.renderCanvasProperties()));
    panel.renderCanvasProperties();
    expect(document.getElementById("prop-shadow-angle")).toBeNull();
    change("prop-shadow-on", true);
    expect(l.controlShadow).toEqual(CONTROL_SHADOW_DEFAULTS);
    change("prop-shadow-angle", "90");
    change("prop-shadow-elevation", "200");
    change("prop-shadow-softness", "20");
    change("prop-shadow-opacity", "70");
    expect(l.controlShadow).toEqual({ angle: 90, elevation: 80, softness: 0.2, opacity: 0.7 });
    change("prop-shadow-on", false);
    expect(l.controlShadow).toBeUndefined();
  });

  it("a knob's Cast Shadow switch is shown while the layout casts shadows, and off is stored as false", () => {
    const knob = control("gain", "knob");
    expect(renderControlShadowRow(knob, layout({ controlShadow: undefined }), false)).toBe("");
    expect(renderControlShadowRow(control("mix", "slider"), layout(), false)).toBe("");
    const sidebar = mount(renderControlShadowRow(knob, layout(), false));
    bindControlShadowRow(knob, hostFor(layout(), sidebar, () => {}));
    change("prop-cast-shadow", false);
    expect(knob.style?.castShadow).toBe(false);
    change("prop-cast-shadow", true);
    expect(knob.style && "castShadow" in knob.style).toBe(false);
  });
});
