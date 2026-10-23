import { describe, expect, it } from "vitest";
import { CanvasProperties } from "../ts/layoutDesigner/canvasProperties.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import type { EffectLayout } from "../ts/layoutTypes.js";

function setup(layout: EffectLayout): { sidebar: HTMLElement; panel: CanvasProperties; renders: () => number } {
  const sidebar = document.createElement("div");
  document.body.replaceChildren(sidebar);
  let renders = 0;
  const host: LayoutPropertiesHost = {
    getLayout: () => layout,
    getParamDefs: () => [],
    getResourceCandidates: () => [],
    getSidebarContent: () => sidebar,
    pushUndoState: () => {},
    pushSidebarUndoOnce: () => {},
    renderCanvas: () => { renders++; },
    renderSidebar: () => {},
    selectElement: () => {},
    renderImageOptionsHtml: () => "",
    browseBackgroundImage: () => {},
    browseKnobImage: () => {},
    isResourceControl: () => false,
  };
  const panel = new CanvasProperties(host);
  panel.renderCanvasProperties();
  return { sidebar, panel, renders: () => renders };
}

function change(id: string, value: string): void {
  const element = document.getElementById(id) as HTMLInputElement | HTMLSelectElement;
  element.value = value;
  element.dispatchEvent(new Event("change"));
}

function blank(): EffectLayout {
  return { effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls: [], textLabels: [] };
}

describe("the canvas properties panel", () => {
  it("sets Fit to Panel, and removes the scaling block once it is back to the defaults", () => {
    const layout = blank();
    const { renders } = setup(layout);
    change("prop-scaling-mode", "fit");
    change("prop-scaling-max", "2.5");
    change("prop-scaling-align", "start");
    expect(layout.scaling).toEqual({ mode: "fit", maxScale: 2.5, align: "start" });
    expect(renders()).toBe(3);

    change("prop-scaling-mode", "");
    change("prop-scaling-max", "");
    change("prop-scaling-align", "");
    expect(layout.scaling).toBeUndefined();
  });

  it("clamps scale limits to what the panel allows", () => {
    const layout = blank();
    setup(layout);
    change("prop-scaling-max", "40");
    change("prop-scaling-min", "-3");
    expect(layout.scaling).toEqual({ maxScale: 8, minScale: 0 });
  });

  it("sets the grid and text defaults, and an emptied field removes the setting", () => {
    const layout = blank();
    setup(layout);
    change("prop-grid-size", "4.4");
    change("prop-layout-label-size", "18");
    change("prop-layout-value-size", "2");
    change("prop-layout-font", "  Georgia, serif ");
    change("prop-layout-label-width", "200");
    expect(layout).toMatchObject({ gridSize: 4, controlLabelFontSize: 18, controlValueFontSize: 6, controlFontFamily: "Georgia, serif", controlLabelMaxWidth: 200 });

    for (const id of ["prop-grid-size", "prop-layout-label-size", "prop-layout-value-size", "prop-layout-font", "prop-layout-label-width"]) {
      change(id, "");
    }
    expect(layout).toEqual(blank());
  });

  it("shows what the layout already holds", () => {
    const layout = { ...blank(), gridSize: 16, scaling: { mode: "fixed" as const, minScale: 0.5 }, controlLabelFontSize: 12 };
    setup(layout);
    expect((document.getElementById("prop-grid-size") as HTMLInputElement).value).toBe("16");
    expect((document.getElementById("prop-scaling-mode") as HTMLSelectElement).value).toBe("fixed");
    expect((document.getElementById("prop-scaling-min") as HTMLInputElement).value).toBe("0.5");
    expect((document.getElementById("prop-layout-label-size") as HTMLInputElement).value).toBe("12");
  });
});
