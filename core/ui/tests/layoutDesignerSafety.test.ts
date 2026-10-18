import fs from "node:fs";
import path from "node:path";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/bridge.js", () => ({ postMessage: vi.fn(), setAppSetting: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));

import { postMessage } from "../ts/bridge.js";
import { initializeDialogModals } from "../ts/dialogs.js";
import { layoutDesigner } from "../ts/layoutDesigner.js";
import type { EffectLayout, LayoutBackground } from "../ts/layoutTypes.js";
import { uiState } from "../ts/state.js";

const MODALS = path.resolve(__dirname, "../ui-components/modals");

function el<T extends HTMLElement = HTMLElement>(id: string): T {
  const found = document.getElementById(id);
  if (!found) throw new Error(`#${id} missing`);
  return found as T;
}

const designerOpen = () => el("layout-designer-modal").style.display !== "none";
const dialogOpen = () => el("dialog-modal").style.display !== "none";
const flush = () => new Promise((resolve) => setTimeout(resolve, 0));
const pressKey = (key: string) => document.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));

function layoutWith(backgrounds: LayoutBackground[] = []): EffectLayout {
  return {
    effectType: "delay_digital",
    layoutId: "layout-under-test",
    version: 1,
    name: "Echo Face",
    dimensions: { width: 400, height: 280 },
    backgrounds,
    controls: [],
    overlays: [],
    textLabels: [],
  };
}

function sentSaves(): Array<{ layoutId?: string; layout?: EffectLayout }> {
  return vi.mocked(postMessage).mock.calls
    .map(([message]) => message as { type: string; layoutId?: string; layout?: EffectLayout })
    .filter((message) => message.type === "saveEffectLayout");
}

describe("layout designer: nothing is lost by accident", () => {
  beforeAll(() => {
    document.body.innerHTML = [
      fs.readFileSync(path.join(MODALS, "layout-designer-modal.html"), "utf8"),
      fs.readFileSync(path.join(MODALS, "dialog-modal.html"), "utf8"),
    ].join("\n");
    initializeDialogModals();
    HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
  });

  beforeEach(() => {
    vi.mocked(postMessage).mockClear();
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
    if (designerOpen()) layoutDesigner.close();
  });

  it("closes without asking when nothing changed", async () => {
    layoutDesigner.open("delay_digital", layoutWith());
    el("layout-designer-cancel").click();
    await flush();
    expect(dialogOpen()).toBe(false);
    expect(designerOpen()).toBe(false);
  });

  it("asks before Cancel, Escape or a backdrop click throw edits away", async () => {
    layoutDesigner.open("delay_digital", layoutWith());
    el("layout-designer-add-color-bg").click();

    el("layout-designer-cancel").click();
    await flush();
    expect(dialogOpen()).toBe(true);
    el("dialog-cancel").click(); // Keep editing
    await flush();
    expect(designerOpen()).toBe(true);

    el("layout-designer-modal").dispatchEvent(new MouseEvent("mousedown", { bubbles: true }));
    await flush();
    expect(dialogOpen()).toBe(true);

    // Escape answers the dialog, and must not also close the designer underneath it.
    pressKey("Escape");
    await flush();
    expect(dialogOpen()).toBe(false);
    expect(designerOpen()).toBe(true);

    // With nothing selected, Escape asks to close; confirming discards.
    pressKey("Escape"); // deselect the new layer
    pressKey("Escape");
    await flush();
    expect(dialogOpen()).toBe(true);
    el("dialog-ok").click();
    await flush();
    expect(designerOpen()).toBe(false);
  });

  it("asks when only the name field was edited", async () => {
    layoutDesigner.open("delay_digital", layoutWith());
    el<HTMLInputElement>("layout-designer-name").value = "Renamed";
    el("layout-designer-close").click();
    await flush();
    expect(dialogOpen()).toBe(true);
    el("dialog-ok").click();
    await flush();
  });

  it("adds a layer under the remaining one after the bottom layer is deleted", async () => {
    layoutDesigner.open("delay_digital", layoutWith([
      { layerIndex: 0, type: "color", value: "#111111" },
      { layerIndex: 1, type: "color", value: "#222222" },
    ]));
    document.querySelector<HTMLElement>(".layout-designer-background.layer-0")!.click();
    pressKey("Delete");
    el("layout-designer-add-color-bg").click();
    el("layout-designer-save").click();
    await flush();

    const saved = sentSaves().at(-1)?.layout;
    expect(saved?.backgrounds.map((bg) => [bg.layerIndex, bg.value]).sort()).toEqual([[0, "#1a1a2e"], [1, "#222222"]]);
    layoutDesigner.handleSaveResult({ layoutId: "layout-under-test", ok: true });
  });

  it("separates layers an older build saved with the same index", async () => {
    layoutDesigner.open("delay_digital", layoutWith([
      { layerIndex: 1, type: "color", value: "#111111" },
      { layerIndex: 1, type: "color", value: "#222222" },
    ]));
    expect(document.querySelectorAll(".layout-designer-background.layer-0")).toHaveLength(1);
    expect(document.querySelectorAll(".layout-designer-background.layer-1")).toHaveLength(1);
    layoutDesigner.close();
  });

  it("keeps the layout's id and name through Reset, so Save overwrites it", async () => {
    layoutDesigner.open("delay_digital", layoutWith([{ layerIndex: 0, type: "color", value: "#111111" }]));
    el("layout-designer-reset").click();
    el("layout-designer-save").click();
    await flush();

    const sent = sentSaves().at(-1);
    expect(sent?.layoutId).toBe("layout-under-test");
    expect(sent?.layout?.name).toBe("Echo Face");
    expect(sent?.layout?.backgrounds).toEqual([]);
    expect(designerOpen()).toBe(true); // until the engine confirms
    layoutDesigner.handleSaveResult({ layoutId: "layout-under-test", ok: true });
    expect(designerOpen()).toBe(false);
  });

  it("brings the Default Controls toggle back in line on undo", () => {
    layoutDesigner.open("delay_digital", layoutWith());
    const toggle = el<HTMLInputElement>("layout-designer-use-default-controls");
    toggle.checked = true;
    toggle.dispatchEvent(new Event("change"));
    el("layout-designer-undo").click();
    expect(toggle.checked).toBe(false);
    layoutDesigner.close();
  });

  // The sidebar's Remove and Delete buttons used to push no undo step, unlike the Delete key.
  it("lets the sidebar's Remove Background be undone", () => {
    layoutDesigner.open("delay_digital", layoutWith([{ layerIndex: 0, type: "color", value: "#111111" }]));
    document.querySelector<HTMLElement>(".layout-designer-background.layer-0")!.click();
    el("prop-delete-bg").click();
    expect(document.querySelectorAll(".layout-designer-background")).toHaveLength(0);

    el("layout-designer-undo").click();
    expect(document.querySelectorAll(".layout-designer-background")).toHaveLength(1);
    layoutDesigner.close();
  });

  it("lets the sidebar's Delete Label be undone", () => {
    layoutDesigner.open("delay_digital", layoutWith());
    el("layout-designer-add-label").click();
    el("prop-delete-label").click();
    expect(document.querySelectorAll(".layout-text-label")).toHaveLength(0);

    el("layout-designer-undo").click();
    expect(document.querySelectorAll(".layout-text-label")).toHaveLength(1);
    layoutDesigner.close();
  });

  it("keeps redo through a press on an element that does not move it", () => {
    // A press used to be an undo step from the mousedown, which cleared the redo stack: undo,
    // then click to select something, and the undone edit was gone for good.
    layoutDesigner.open("delay_digital", layoutWith());
    el("layout-designer-add-label").click();
    el("layout-designer-add-label").click();
    el("layout-designer-undo").click();
    expect(document.querySelectorAll(".layout-text-label")).toHaveLength(1);

    const label = document.querySelector<HTMLElement>(".layout-text-label")!;
    label.dispatchEvent(new MouseEvent("mousedown", { bubbles: true, clientX: 10, clientY: 10, button: 0 }));
    document.dispatchEvent(new MouseEvent("mouseup", { bubbles: true, clientX: 10, clientY: 10 }));

    el("layout-designer-redo").click();
    expect(document.querySelectorAll(".layout-text-label")).toHaveLength(2);
    layoutDesigner.close();
  });
});
