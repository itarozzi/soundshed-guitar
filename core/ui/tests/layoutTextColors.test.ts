import { describe, expect, it } from "vitest";
import { normalizeImportedLayout } from "../ts/layoutDesigner/layoutImport.js";
import {
  colorInputValue,
  defaultControlsColorAttrs,
  renderCustomLayoutBackdrop,
  renderCustomLayoutPreviewLayers,
  resolveControlTextColors,
} from "../ts/layoutRenderer.js";
import type { EffectLayout, LayoutControl } from "../ts/layoutTypes.js";
import type { GraphNode } from "../ts/types.js";

const node = { id: "n1", type: "delay_digital", params: { time: 0.5 } } as unknown as GraphNode;
const timeDef = { key: "time", name: "Time", default: 0.5, min: 0, max: 1 };

function knob(style?: LayoutControl["style"]): LayoutControl {
  return { paramKey: "time", type: "knob", position: { x: 0, y: 0 }, style };
}

function render(layout: Partial<EffectLayout>): { label: HTMLElement; value: HTMLElement } {
  const host = document.createElement("div");
  host.innerHTML = renderCustomLayoutPreviewLayers(node, { textLabels: [], ...layout } as EffectLayout, [timeDef]);
  return {
    label: host.querySelector<HTMLElement>(".custom-control-label")!,
    value: host.querySelector<HTMLElement>(".node-param-value")!,
  };
}

describe("control label and value colours", () => {
  it("keeps the theme's colours when nothing is set", () => {
    const { label, value } = render({ controls: [knob()] });
    expect(label.style.color).toBe("var(--text-dark-secondary)");
    expect(value.style.color).toBe("var(--text-tertiary)");
  });

  it("colours a control's value as it colours its label", () => {
    const { label, value } = render({ controls: [knob({ labelColor: "#ff0000", valueColor: "#00ff00" })] });
    expect(label.style.color).toBe("rgb(255, 0, 0)");
    expect(value.style.color).toBe("rgb(0, 255, 0)");
  });

  it("uses the layout's defaults, and a control's own colour over them", () => {
    const layout = { controlLabelColor: "#112233", controlValueColor: "#445566" };
    const inherited = render({ ...layout, controls: [knob()] });
    expect(inherited.label.style.color).toBe("rgb(17, 34, 51)");
    expect(inherited.value.style.color).toBe("rgb(68, 85, 102)");

    const own = render({ ...layout, controls: [knob({ valueColor: "#ffffff" })] });
    expect(own.label.style.color).toBe("rgb(17, 34, 51)");
    expect(own.value.style.color).toBe("rgb(255, 255, 255)");
  });

  it("drops a colour that could do more than colour, falling through to the default", () => {
    const layout = { controlValueColor: "#445566" } as EffectLayout;
    expect(resolveControlTextColors(layout, knob({ valueColor: "red; background: url(x)" })).value).toBe("#445566");
    expect(resolveControlTextColors({ controlLabelColor: "url(https://example.com)" } as EffectLayout).label).toBeNull();
  });

  it("carries the layout's defaults to the default controls of a backdrop layout", () => {
    expect(defaultControlsColorAttrs({})).toEqual({ classes: "", style: "" });

    const host = document.createElement("div");
    host.innerHTML = renderCustomLayoutBackdrop(
      node,
      { dimensions: { width: 400, height: 280 }, backgrounds: [], textLabels: [], controlValueColor: "#445566" } as unknown as EffectLayout,
      `<span class="node-param-value">0.50</span>`,
    );
    const wrapper = host.querySelector<HTMLElement>(".custom-layout-default-controls")!;
    expect(wrapper.classList.contains("has-layout-value-color")).toBe(true);
    expect(wrapper.classList.contains("has-layout-label-color")).toBe(false);
    expect(wrapper.style.getPropertyValue("--layout-value-color").trim()).toBe("#445566");
  });

  it("reads all three colours back from an imported layout, typed", () => {
    const layout = normalizeImportedLayout({
      effectType: "delay_digital",
      controlLabelColor: "#112233",
      controlValueColor: 7,
      controls: [{ paramKey: "time", type: "knob", position: { x: 0, y: 0 }, style: { valueColor: "#abcdef" } }],
    })!;
    expect(layout.controlLabelColor).toBe("#112233");
    expect(layout.controlValueColor).toBeUndefined();
    expect(layout.controls[0].style?.valueColor).toBe("#abcdef");
  });

  it("gives a colour picker only #rrggbb", () => {
    expect(colorInputValue("#A0b0C0", "#ffffff")).toBe("#A0b0C0");
    expect(colorInputValue("rgb(1, 2, 3)", "#ffffff")).toBe("#ffffff");
    expect(colorInputValue(null, "#a0a0a0")).toBe("#a0a0a0");
  });
});
