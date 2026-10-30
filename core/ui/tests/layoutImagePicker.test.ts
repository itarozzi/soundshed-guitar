import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import * as bridge from "../ts/bridge.js";
import { enhanceImageSelects } from "../ts/layoutDesigner/imagePicker.js";
import { imageOptionsHtml } from "../ts/layoutDesigner/images.js";
import { watchSidebar } from "../ts/layoutDesigner/sidebarEnhancements.js";
import type { LayoutImageRef } from "../ts/layoutTypes.js";
import { layoutImageCategory, layoutImageUsage } from "../ts/layoutImageCategories.js";
import { onLayoutImageSelected } from "../ts/messages/layoutHandlers.js";
import { uiState } from "../ts/state.js";

// The designer's image <select>s shown as thumbnail lists; the <select> stays the value.

const IMAGES: LayoutImageRef[] = [
  { imageId: "cap", fileName: "cap.png", url: "layout-images/id/cap?v=1", width: 128, height: 128, type: "knob" },
  { imageId: "strip", fileName: "strip.png", url: "layout-images/id/strip?v=1", width: 128, height: 1280, type: "knob" },
];

function mountSelect(selected?: string, id = "pick"): HTMLSelectElement {
  document.body.innerHTML = `
    <div class="layout-property-row">
      <span class="layout-property-label">Image</span>
      <div class="layout-property-input"><select id="${id}">${imageOptionsHtml("knob", selected)}</select></div>
    </div>`;
  enhanceImageSelects(document.body);
  return document.getElementById(id) as HTMLSelectElement;
}

function open(): HTMLDetailsElement {
  const picker = document.querySelector<HTMLDetailsElement>("details.layout-image-picker")!;
  picker.open = true;
  picker.dispatchEvent(new Event("toggle"));
  return picker;
}

function items(): Array<{ value: string; name: string; selected: boolean }> {
  return Array.from(document.querySelectorAll<HTMLElement>(".layout-image-picker-item")).map((item) => ({
    value: item.dataset.value ?? "",
    name: item.querySelector(".layout-image-picker-name")?.textContent ?? "",
    selected: item.classList.contains("is-selected"),
  }));
}

beforeEach(() => {
  // jsdom has no layout, so no scrolling.
  if (!("scrollIntoView" in Element.prototype)) {
    Element.prototype.scrollIntoView = () => {};
  }
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: IMAGES.map((image) => ({ ...image })) };
});

afterEach(() => {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
  document.body.replaceChildren();
});

describe("image picker", () => {
  it("hides the select and shows the chosen image as a thumbnail", () => {
    const select = mountSelect("cap");
    expect(select.hidden).toBe(true);
    const trigger = document.querySelector(".layout-image-picker-selected")!;
    expect(trigger.querySelector("img")?.getAttribute("src")).toContain("layout-images/id/cap");
    expect(trigger.querySelector(".layout-image-picker-name")?.textContent).toBe("cap.png");
    expect(trigger.querySelector(".layout-image-picker-facts")?.textContent).toBe("Knob · 128×128");
  });

  it("lists every image, with a way to clear the one chosen", () => {
    mountSelect("cap");
    open();
    expect(items()).toEqual([
      { value: "", name: "No image", selected: false },
      { value: "cap", name: "cap.png", selected: true },
      { value: "strip", name: "strip.png", selected: false },
    ]);
    expect(document.querySelectorAll(".layout-image-picker-list img")).toHaveLength(2);
  });

  it("offers no clearing when nothing is chosen, and says when the library is empty", () => {
    mountSelect();
    open();
    expect(items().map((item) => item.value)).toEqual(["cap", "strip"]);

    uiState.layoutLibrary!.images = [];
    mountSelect();
    open();
    expect(items()).toEqual([]);
    expect(document.querySelector(".layout-image-picker-empty")?.textContent).toContain("Browse");
  });

  it("picking a thumbnail sets the select and fires its change event", () => {
    const select = mountSelect("cap");
    const onChange = vi.fn(() => select.value);
    select.addEventListener("change", onChange);
    const picker = open();
    document.querySelector<HTMLElement>('.layout-image-picker-item[data-value="strip"]')!.click();
    expect(onChange).toHaveReturnedWith("strip");
    expect(picker.open).toBe(false);
    expect(document.querySelector(".layout-image-picker-selected .layout-image-picker-name")?.textContent).toBe("strip.png");

    // Picking the image already chosen changes nothing.
    open();
    document.querySelector<HTMLElement>('.layout-image-picker-item[data-value="strip"]')!.click();
    expect(onChange).toHaveBeenCalledTimes(1);
  });

  it("leaves selects of other things alone, and enhances each select once", () => {
    document.body.innerHTML = `<select id="mode"><option value="rotate">Turning face</option></select><select id="pick">${imageOptionsHtml("knob")}</select>`;
    enhanceImageSelects(document.body);
    enhanceImageSelects(document.body);
    expect((document.getElementById("mode") as HTMLSelectElement).hidden).toBe(false);
    expect(document.querySelectorAll("details.layout-image-picker")).toHaveLength(1);
  });

  it("enhances selects rendered into a watched container later", async () => {
    const root = document.createElement("div");
    document.body.append(root);
    watchSidebar(root);
    root.innerHTML = `<select>${imageOptionsHtml("knob", "strip")}</select>`;
    await Promise.resolve();
    expect(root.querySelector("details.layout-image-picker .layout-image-picker-name")?.textContent).toBe("strip.png");
  });
});

describe("an image the engine already had", () => {
  it("keeps the library entry's copies and kind when it is picked again", () => {
    const renditions = [{ edge: 1024, width: 1024, height: 1024, url: "copy" }];
    uiState.layoutLibrary!.images = [{ ...IMAGES[0], version: "7", renditions, type: "background" }];
    onLayoutImageSelected({ type: "layoutImageSelected", purpose: "knob", imageId: "cap", fileName: "cap.png", url: "layout-images/id/cap?v=7", width: 128, height: 128 });
    expect(uiState.layoutLibrary!.images).toHaveLength(1);
    expect(uiState.layoutLibrary!.images[0]).toMatchObject({ imageId: "cap", version: "7", renditions, type: "background" });
  });
});

describe("image categories", () => {
  const library: LayoutImageRef[] = [
    { imageId: "cap", fileName: "cap.png", url: "u/cap", type: "knob" },
    { imageId: "wood", fileName: "wood.png", url: "u/wood", type: "background" },
    { imageId: "bat", fileName: "bat.png", url: "u/bat", type: "switch" },
    { imageId: "odd", fileName: "odd.png", url: "u/odd" },
    { imageId: "f-knob", fileName: "f-knob.svg", url: "u/f-knob", type: "knob", factory: true },
  ];

  beforeEach(() => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: library.map((image) => ({ ...image })) };
  });

  function tabs(): Array<{ filter: string; text: string; active: boolean }> {
    return Array.from(document.querySelectorAll<HTMLElement>(".layout-image-picker-tab")).map((tab) => ({
      filter: tab.dataset.filter ?? "",
      text: tab.textContent ?? "",
      active: tab.classList.contains("is-active"),
    }));
  }

  it("opens on the row's category, counts each, and puts factory images in their own section", () => {
    mountSelect();
    open();
    expect(tabs()).toEqual([
      { filter: "all", text: "All5", active: false },
      { filter: "knob", text: "Knob2", active: true },
      { filter: "background", text: "Background1", active: false },
      { filter: "switch", text: "Switch1", active: false },
      { filter: "general", text: "Misc1", active: false },
    ]);
    expect(items().map((item) => item.value)).toEqual(["cap", "f-knob"]);
    expect(Array.from(document.querySelectorAll(".layout-image-picker-section")).map((el) => el.textContent)).toEqual(["Your images", "Factory"]);
  });

  it("switches category from the tabs, and opens on the chosen image's category", () => {
    mountSelect("bat");
    open();
    expect(items().map((item) => item.value)).toEqual(["", "bat"]);
    document.querySelector<HTMLElement>('.layout-image-picker-tab[data-filter="all"]')!.click();
    expect(items().map((item) => item.value)).toHaveLength(6);
    document.querySelector<HTMLElement>('.layout-image-picker-tab[data-filter="general"]')!.click();
    expect(items().map((item) => item.value)).toEqual(["", "odd"]);
  });

  it("opens on All when the row's category has nothing in it", () => {
    uiState.layoutLibrary!.images = library.filter((image) => image.type !== "switch").map((image) => ({ ...image }));
    document.body.innerHTML = `<select id="pick">${imageOptionsHtml("switch")}</select>`;
    enhanceImageSelects(document.body);
    open();
    expect(tabs().find((tab) => tab.active)?.filter).toBe("all");
  });

  it("files an image dragged onto a tab, and leaves factory images where they ship", () => {
    const post = vi.spyOn(bridge, "postMessage").mockImplementation(() => {});
    const select = mountSelect();
    const onChange = vi.fn();
    select.addEventListener("change", onChange);
    open();
    const backgroundTab = document.querySelector<HTMLElement>('.layout-image-picker-tab[data-category="background"]')!;
    document.elementFromPoint = () => backgroundTab;
    const drag = (item: HTMLElement) => {
      item.dispatchEvent(new MouseEvent("pointerdown", { bubbles: true, clientX: 10, clientY: 10 }));
      document.dispatchEvent(new MouseEvent("pointermove", { bubbles: true, clientX: 10, clientY: 40 }));
      document.dispatchEvent(new MouseEvent("pointerup", { bubbles: true, clientX: 10, clientY: 40 }));
      item.click();
    };

    drag(document.querySelector<HTMLElement>('.layout-image-picker-item[data-value="cap"]')!);
    expect(post).toHaveBeenCalledWith({ type: "setLayoutImageCategory", imageId: "cap", category: "background" });
    expect(uiState.layoutLibrary!.images.find((image) => image.imageId === "cap")?.type).toBe("background");
    expect(items().map((item) => item.value)).toEqual(["f-knob"]);
    // The click that ends a drag picks nothing.
    expect(onChange).not.toHaveBeenCalled();

    drag(document.querySelector<HTMLElement>('.layout-image-picker-item[data-value="f-knob"]')!);
    expect(post).toHaveBeenCalledTimes(1);
    expect(uiState.layoutLibrary!.images.find((image) => image.imageId === "f-knob")?.type).toBe("knob");
    post.mockRestore();
  });

  it("files an image never filed by where the library's layouts use it", () => {
    const layout = {
      effectType: "fx", version: 1, dimensions: { width: 100, height: 100 }, textLabels: [],
      backgrounds: [{ layerIndex: 0, type: "image" as const, value: "odd" }],
      controls: [{ paramKey: "a", type: "toggle" as const, position: { x: 0, y: 0 }, style: { toggleImage: { onImageId: "lit" } } }],
    };
    uiState.layoutLibrary!.byEffectType = { fx: [{ layout, isDefault: true, layoutId: "l" }] };
    const usage = layoutImageUsage();
    expect(layoutImageCategory({ imageId: "odd" }, usage)).toBe("background");
    expect(layoutImageCategory({ imageId: "lit" }, usage)).toBe("switch");
    expect(layoutImageCategory({ imageId: "unused" }, usage)).toBe("general");
    // A filed category wins over use.
    expect(layoutImageCategory({ imageId: "odd", type: "knob" }, usage)).toBe("knob");
  });

  it("takes the category the engine filed a browsed image under", () => {
    onLayoutImageSelected({ type: "layoutImageSelected", purpose: "knob", imageId: "new", fileName: "new.png", url: "u/new", category: "knob" });
    expect(uiState.layoutLibrary!.images.find((image) => image.imageId === "new")?.type).toBe("knob");
  });
});
