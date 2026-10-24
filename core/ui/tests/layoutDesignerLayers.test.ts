import fs from "node:fs";
import path from "node:path";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/bridge.js", () => ({ postMessage: vi.fn(), setAppSetting: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));

import { initializeDialogModals } from "../ts/dialogs.js";
import { layoutDesigner } from "../ts/layoutDesigner.js";
import type { EffectLayout } from "../ts/layoutTypes.js";
import { EffectTypeRegistry } from "../ts/presetV2.js";
import { uiState } from "../ts/state.js";

const MODALS = path.resolve(__dirname, "../ui-components/modals");

function layout(useDefaultControls = false): EffectLayout {
  return {
    effectType: "delay_digital",
    layoutId: "layout-layers",
    version: 1,
    name: "Layers",
    dimensions: { width: 400, height: 240 },
    // A background over the whole canvas takes every click on it.
    backgrounds: [{ layerIndex: 0, type: "color", value: "#202020" }, { layerIndex: 1, type: "gradient", value: "linear-gradient(#000, #111)" }],
    controls: [{ paramKey: "time", type: "knob", position: { x: 20, y: 20 } }],
    overlays: [{ id: "rect-1", position: { x: 100, y: 100 }, size: { width: 40, height: 40 } }],
    textLabels: [{ id: "label-1", text: "ECHO", position: { x: 200, y: 20 }, fontSize: 14 }],
    useDefaultControls,
  };
}

const layerNames = () => [...document.querySelectorAll("#layout-designer-layers .layout-layer-item .layout-layer-name")].map((el) => el.textContent);
const selectedName = () => document.querySelector("#layout-designer-layers .layout-layer-item.is-selected .layout-layer-name")?.textContent;
const pick = (name: string) => {
  const item = [...document.querySelectorAll<HTMLElement>("#layout-designer-layers .layout-layer-item")].find((el) => el.querySelector(".layout-layer-name")?.textContent === name);
  if (!item) throw new Error(`no layer ${name}`);
  item.click();
};
const sidebarText = () => document.getElementById("layout-designer-sidebar-content")?.textContent ?? "";

describe("layout designer: the layers list", () => {
  beforeAll(() => {
    document.body.innerHTML = [
      fs.readFileSync(path.join(MODALS, "layout-designer-modal.html"), "utf8"),
      fs.readFileSync(path.join(MODALS, "dialog-modal.html"), "utf8"),
    ].join("\n");
    initializeDialogModals();
    const delay = EffectTypeRegistry.resolve("delay_digital");
    EffectTypeRegistry.register(delay, {
      type: delay,
      displayName: "Digital Delay",
      category: "delay",
      requiresResource: false,
      parameters: [{ key: "time", name: "Time", default: 0.3, min: 0, max: 2, unit: "s" }],
    });
    HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
  });

  beforeEach(() => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
    if (document.getElementById("layout-designer-modal")?.style.display !== "none") layoutDesigner.close();
  });

  it("lists the layout and everything in it, front-most first", () => {
    layoutDesigner.open("delay_digital", layout());
    expect(layerNames()).toEqual(["Layout", "ECHO", "Time", "Rectangle 1", "Background 2", "Background 1"]);
    expect(selectedName()).toBe("Layout");
  });

  it("switches the properties to the picked layer, and back to the layout from a background", () => {
    layoutDesigner.open("delay_digital", layout());
    // A click on the canvas lands on the background covering it.
    document.querySelector<HTMLElement>('.layout-designer-background[data-layer-index="1"]')!.click();
    expect(selectedName()).toBe("Background 2");
    expect(sidebarText()).toContain("Background Layer 2");

    pick("Layout");
    expect(selectedName()).toBe("Layout");
    expect(document.getElementById("prop-label-size")).toBeNull();
    expect(sidebarText()).toContain("Fit to Panel");

    pick("Time");
    expect(selectedName()).toBe("Time");
    expect(document.getElementById("prop-control-type")).not.toBeNull();

    pick("ECHO");
    expect(document.getElementById("prop-label-font-size")).not.toBeNull();
    pick("Rectangle 1");
    expect(document.getElementById("prop-overlay-width")).not.toBeNull();
  });

  it("shows default controls as one layer that opens their position and scale", () => {
    layoutDesigner.open("delay_digital", layout(true));
    expect(layerNames()).toContain("Default controls");
    expect(layerNames()).not.toContain("Time");
    document.querySelector<HTMLElement>('.layout-designer-background[data-layer-index="0"]')!.click();
    expect(document.getElementById("prop-dc-offset-x")).toBeNull();
    pick("Default controls");
    expect(document.getElementById("prop-dc-offset-x")).not.toBeNull();
  });
});
