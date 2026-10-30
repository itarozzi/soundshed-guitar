import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { enhanceCollapsibleGroups, groupKey } from "../ts/layoutDesigner/collapsibleGroups.js";
import { watchSidebar } from "../ts/layoutDesigner/sidebarEnhancements.js";

// The designer's property groups fold on their titles, remembered by title.

const SIDEBAR = `
  <div class="layout-property-group" id="fit">
    <div class="layout-property-group-title">Fit to Panel</div>
    <div class="layout-property-row" id="fit-row">Mode</div>
  </div>
  <div class="layout-property-group" id="bg">
    <div class="layout-property-group-title">Background Layer 2</div>
    <div class="layout-property-row">Type</div>
  </div>
  <div class="layout-property-group" id="untitled">
    <button type="button">Remove from Layout</button>
  </div>`;

function draw(): HTMLElement {
  const root = document.createElement("div");
  root.innerHTML = SIDEBAR;
  document.body.replaceChildren(root);
  enhanceCollapsibleGroups(root);
  return root;
}

const title = (groupId: string) => document.querySelector<HTMLElement>(`#${groupId} > .layout-property-group-title`)!;
const folded = (groupId: string) => document.getElementById(groupId)!.classList.contains("is-collapsed");

beforeEach(() => localStorage.clear());
afterEach(() => {
  vi.restoreAllMocks();
  document.body.replaceChildren();
});

describe("collapsible property groups", () => {
  it("open by default, and a title click folds and unfolds its group", () => {
    draw();
    expect(folded("fit")).toBe(false);
    expect(title("fit").getAttribute("role")).toBe("button");
    expect(title("fit").getAttribute("aria-expanded")).toBe("true");
    title("fit").click();
    expect(folded("fit")).toBe(true);
    expect(folded("bg")).toBe(false);
    expect(title("fit").getAttribute("aria-expanded")).toBe("false");
    title("fit").click();
    expect(folded("fit")).toBe(false);
  });

  it("stay folded when the sidebar is drawn again, a numbered title folding with its siblings", () => {
    draw();
    title("fit").click();
    title("bg").click();
    expect(groupKey("Background Layer 2")).toBe(groupKey("Background Layer 1"));
    const root = draw();
    root.querySelector("#bg .layout-property-group-title")!.textContent = "Background Layer 1";
    expect(folded("fit")).toBe(true);
    expect(folded("bg")).toBe(true);
  });

  it("fold from the keyboard, and leave groups without a title alone", () => {
    draw();
    title("fit").dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" }));
    expect(folded("fit")).toBe(true);
    title("fit").dispatchEvent(new KeyboardEvent("keydown", { key: " " }));
    expect(folded("fit")).toBe(false);
    expect(document.getElementById("untitled")!.classList.contains("is-collapsed")).toBe(false);
  });

  it("still fold when storage is unavailable", () => {
    vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => { throw new Error("blocked"); });
    vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => { throw new Error("blocked"); });
    draw();
    title("fit").click();
    expect(folded("fit")).toBe(true);
  });

  it("are made foldable however the watched sidebar is drawn", async () => {
    localStorage.setItem("layoutDesigner.collapsedGroups", JSON.stringify(["fit to panel"]));
    const root = document.createElement("div");
    document.body.replaceChildren(root);
    watchSidebar(root);
    root.innerHTML = SIDEBAR;
    await Promise.resolve();
    expect(folded("fit")).toBe(true);
    expect(title("bg").dataset.collapsible).toBe("true");
  });
});
