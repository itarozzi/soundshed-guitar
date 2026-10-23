import { beforeAll, describe, expect, it } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import { renderCustomLayout } from "../ts/layoutRenderer.js";
import { layoutRequirements, stampLayoutRequirements } from "../ts/layoutRequirements.js";
import { sanitizeLayout, type EffectLayout } from "../ts/layoutTypes.js";
import { applyCustomLayoutScaling } from "../ts/signalPath/paramsPanel/layoutOverlay.js";
import type { GraphNode } from "../ts/types.js";

const node = { id: "n1", type: "delay_digital", params: { time: 0.5 } } as unknown as GraphNode;
const defs: BlendParamDef[] = [{ key: "time", name: "Time", default: 0.5, min: 0, max: 1, unit: "amount" }];

function layoutWith(extra: Partial<EffectLayout> = {}): EffectLayout {
  return {
    effectType: "delay_digital",
    version: 1,
    dimensions: { width: 480, height: 320 },
    backgrounds: [],
    controls: [{ paramKey: "time", type: "knob", position: { x: 16, y: 16 } }],
    textLabels: [],
    ...extra,
  };
}

function render(layout: EffectLayout): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = renderCustomLayout(node, layout, defs);
  return host;
}

beforeAll(() => {
  globalThis.ResizeObserver ??= class {
    observe(): void {}
    unobserve(): void {}
    disconnect(): void {}
  } as unknown as typeof ResizeObserver;
});

describe("the scaling block reaches the panel only when a layout has one", () => {
  it("writes nothing for a layout without one", () => {
    const container = render(layoutWith()).querySelector<HTMLElement>(".custom-layout-container")!;
    expect(container.hasAttribute("data-layout-scaling")).toBe(false);
  });

  it("marks the layout as one that grows only when its scaling lets it grow", () => {
    const outer = (layout: EffectLayout) => render(layout).querySelector<HTMLElement>(".custom-layout-scale-outer")!;
    expect(outer(layoutWith()).hasAttribute("data-layout-grows")).toBe(false);
    expect(outer(layoutWith({ scaling: { mode: "fit" } })).hasAttribute("data-layout-grows")).toBe(false);
    expect(outer(layoutWith({ scaling: { mode: "fixed", maxScale: 3 } })).hasAttribute("data-layout-grows")).toBe(false);
    expect(outer(layoutWith({ scaling: { maxScale: 2 } })).hasAttribute("data-layout-grows")).toBe(true);
  });

  it("writes only the known fields, each of the right type", () => {
    const scaling = { mode: "fit", maxScale: 2, minScale: -1, align: "sideways", extra: "<script>" } as unknown as EffectLayout["scaling"];
    const container = render(layoutWith({ scaling })).querySelector<HTMLElement>(".custom-layout-container")!;
    expect(JSON.parse(container.dataset.layoutScaling!)).toEqual({ mode: "fit", maxScale: 2 });
  });
});

describe("control text sizes, font and label width", () => {
  it("keep the original sizes when the layout sets none", () => {
    const host = render(layoutWith());
    expect(host.querySelector<HTMLElement>(".custom-control-label")!.getAttribute("style")).toBe(
      "font-size: 10px; color: var(--text-dark-secondary); margin-bottom: 4px;",
    );
    expect(host.querySelector<HTMLElement>(".node-param-value")!.style.fontSize).toBe("9px");
  });

  it("use the layout's defaults, a control's own size over them, and clamp what is not sane", () => {
    const host = render(layoutWith({
      controlLabelFontSize: 22,
      controlValueFontSize: 400,
      controlFontFamily: "Georgia, serif",
      controlLabelMaxWidth: 240,
      controls: [
        { paramKey: "time", type: "knob", position: { x: 16, y: 16 }, style: { labelFontSize: 30 } },
      ],
    }));
    const label = host.querySelector<HTMLElement>(".custom-control-label")!;
    const value = host.querySelector<HTMLElement>(".node-param-value")!;
    expect(label.style.fontSize).toBe("30px");
    expect(label.style.maxWidth).toBe("240px");
    expect(label.style.fontFamily).toBe("Georgia, serif");
    expect(value.style.fontSize).toBe("96px");
    expect(value.style.fontFamily).toBe("Georgia, serif");
  });

  it("drops a font family that could do more than set the font", () => {
    const host = render(layoutWith({ controlFontFamily: "x; background: url(https://example.com)" }));
    expect(host.querySelector<HTMLElement>(".custom-control-label")!.style.fontFamily).toBe("");
  });
});

describe("applyCustomLayoutScaling with a scaling block", () => {
  function scaled(layout: EffectLayout, panelWidth: number, setup?: (container: HTMLElement) => void) {
    const container = render(layout);
    document.body.appendChild(container);
    const outer = container.querySelector<HTMLElement>(".custom-layout-scale-outer")!;
    const inner = container.querySelector<HTMLElement>(".custom-layout-container")!;
    Object.defineProperty(outer, "offsetWidth", { configurable: true, value: panelWidth });
    setup?.(container);
    applyCustomLayoutScaling(container);
    const result = { transform: inner.style.transform, marginLeft: inner.style.marginLeft, height: outer.style.height, overflowX: outer.style.overflowX };
    container.remove();
    return result;
  }

  it("grows to fill a wider panel up to maxScale, and centres the drawn size once it stops", () => {
    expect(scaled(layoutWith({ scaling: { maxScale: 4 } }), 960)).toEqual({ transform: "scale(2)", marginLeft: "0px", height: "640px", overflowX: "hidden" });
    expect(scaled(layoutWith({ scaling: { maxScale: 1.5 } }), 960)).toEqual({ transform: "translateX(120px) scale(1.5)", marginLeft: "auto", height: "480px", overflowX: "hidden" });
  });

  it("in fit mode stays within the height that can be seen without scrolling", () => {
    const result = scaled(layoutWith({ scaling: { mode: "fit", maxScale: 4 } }), 960, (container) => {
      const scroller = document.createElement("div");
      scroller.style.overflowY = "auto";
      Object.defineProperty(scroller, "clientHeight", { configurable: true, value: 168 });
      scroller.getBoundingClientRect = () => ({ top: 0, left: 0, right: 960, bottom: 168, width: 960, height: 168, x: 0, y: 0, toJSON: () => ({}) });
      container.replaceWith(scroller);
      scroller.appendChild(container);
    });
    // 168 px to the scroller's bottom, less 8 px of breathing room: 160 / 320 = 0.5.
    expect(result.transform).toBe(`translateX(${(960 - 240) / 2}px) scale(0.5)`);
    expect(result.height).toBe("160px");
  });

  it("scrolls sideways rather than shrinking past minScale", () => {
    expect(scaled(layoutWith({ scaling: { minScale: 0.75 } }), 240).overflowX).toBe("auto");
  });
});

describe("sanitizeLayout", () => {
  it("snaps to a layout's own grid, and to 8 px for one that chose none or an unusable one", () => {
    const fine = sanitizeLayout(layoutWith({ gridSize: 4, controls: [{ paramKey: "time", type: "knob", position: { x: 13, y: 22 } }] }));
    expect(fine.controls[0].position).toEqual({ x: 12, y: 24 });
    for (const gridSize of [undefined, 0, 2.5, 500]) {
      const coarse = sanitizeLayout(layoutWith({ gridSize, controls: [{ paramKey: "time", type: "knob", position: { x: 13, y: 22 } }] }));
      expect(coarse.controls[0].position).toEqual({ x: 16, y: 24 });
    }
  });

  it("keeps a canvas larger than the old 1024 x 768 limit, up to 4096", () => {
    expect(sanitizeLayout(layoutWith({ dimensions: { width: 2048, height: 1152 } })).dimensions).toEqual({ width: 2048, height: 1152 });
    expect(sanitizeLayout(layoutWith({ dimensions: { width: 9000, height: 9000 } })).dimensions).toEqual({ width: 4096, height: 4096 });
  });
});

describe("layout requirements and version", () => {
  it("leave a layout that uses nothing new at version 1 with no list", () => {
    const layout = stampLayoutRequirements(layoutWith({ scaling: { mode: "fit-width", maxScale: 1, align: "center" } }));
    expect(layout.version).toBe(1);
    expect(layout.requires).toBeUndefined();
  });

  it("list each newer feature, and drop the list again when they are gone", () => {
    const layout = layoutWith({ dimensions: { width: 2048, height: 768 }, scaling: { maxScale: 2 }, controlLabelFontSize: 14 });
    expect(layoutRequirements(layout)).toEqual(["large-canvas", "scaling", "text-style"]);
    stampLayoutRequirements(layout);
    expect(layout.version).toBe(2);
    expect(layout.requires).toEqual(["large-canvas", "scaling", "text-style"]);

    layout.dimensions = { width: 1024, height: 768 };
    delete layout.scaling;
    delete layout.controlLabelFontSize;
    stampLayoutRequirements(layout);
    expect(layout.version).toBe(1);
    expect(layout.requires).toBeUndefined();
  });

  it("count a control's own text size", () => {
    expect(layoutRequirements(layoutWith({ controls: [{ paramKey: "time", type: "knob", position: { x: 0, y: 0 }, style: { valueFontSize: 12 } }] })))
      .toEqual(["text-style"]);
  });
});
