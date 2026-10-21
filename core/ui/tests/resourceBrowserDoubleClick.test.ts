import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import { isDoubleClickExempt } from "../ts/resourceBrowser/helpers.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource } from "../ts/types.js";

const markup = readFileSync(join(__dirname, "..", "ui-components", "modals", "resource-browser-modal.html"), "utf8");

const nam = (id: string, name: string, metadata?: LibraryResource["metadata"]): LibraryResource => ({
  id,
  name,
  category: "Amps",
  description: "",
  filePath: `C:/Models/${id}.nam`,
  metadata,
});
const imported = nam("tone3000:101", "Source Pack - Clean", {
  provider: "tone3000", toneId: "42", toneTitle: "Source Pack", modelId: "101", architectureVersion: "1",
});
const models = [
  { id: 101, name: "Clean", model_url: "https://www.tone3000.com/api/v1/models/101/download/clean.nam" },
  { id: 102, name: "Crunch", model_url: "https://www.tone3000.com/api/v1/models/102/download/crunch.nam" },
];
const json = (body: unknown): Response => new Response(JSON.stringify(body), { status: 200 });
const modal = () => document.getElementById("resource-browser-modal")!;

/// What a real double click delivers: two clicks counting up, then the dblclick,
/// each to whatever is under the pointer by then. A click that previews repaints
/// the list, so the second click lands on the fresh row, not the one first hit.
function doubleClick(underPointer: () => Element): void {
  underPointer().dispatchEvent(new MouseEvent("click", { bubbles: true, detail: 1 }));
  underPointer().dispatchEvent(new MouseEvent("click", { bubbles: true, detail: 2 }));
  underPointer().dispatchEvent(new MouseEvent("dblclick", { bubbles: true, detail: 2 }));
}

describe("double clicking a resource in the browser", () => {
  const previousLibrary = uiState.resourceLibrary;
  const previousSettings = uiState.appSettings;
  let browser: ResourceBrowserModal;
  let fetchMock: ReturnType<typeof vi.fn<typeof fetch>>;

  beforeEach(() => {
    document.body.innerHTML = markup;
    uiState.resourceLibrary = { nam: [nam("amp-a", "Alpha"), nam("amp-b", "Bravo"), imported], ir: [] };
    uiState.appSettings = { "features.tone3000.enabled": true, "tone3000.useSoundshedToneSearchApi": true };
    window.IPlugSendMsg = () => {};
    vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
    if (!("scrollIntoView" in Element.prototype)) {
      Element.prototype.scrollIntoView = () => {};
    }
    fetchMock = vi.fn<typeof fetch>(async (input) => {
      const url = new URL(String(input));
      return url.pathname.endsWith("/models") ? json({ data: models, total_pages: 1 }) : json({ data: [], total_pages: 1 });
    });
    vi.stubGlobal("fetch", fetchMock);
    browser = new ResourceBrowserModal();
  });

  afterEach(async () => {
    browser.close();
    await new Promise((resolve) => requestAnimationFrame(resolve));
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    delete window.IPlugSendMsg;
    uiState.resourceLibrary = previousLibrary;
    uiState.appSettings = previousSettings;
    document.body.innerHTML = "";
  });

  const libraryRow = (id: string) => modal().querySelector<HTMLElement>(`.resource-browser-item[data-resource-id="${id}"]`)!;

  it.each([
    ["the row", ".resource-browser-item-title"],
    ["its play button", ".resource-browser-item-select"],
  ])("takes a library resource and closes on %s", (_where, selector) => {
    const onSelect = vi.fn();
    browser.open({ resourceType: "nam", nodeId: "amp", currentId: "amp-a", onSelect });

    doubleClick(() => libraryRow("amp-b").querySelector(selector)!);

    expect(onSelect).toHaveBeenCalledWith("amp-b");
    expect(modal().style.display).toBe("none");
  });

  it("leaves a double click on a row's own controls to them", () => {
    const onSelect = vi.fn();
    browser.open({ resourceType: "nam", nodeId: "amp", currentId: "amp-a", onSelect });

    doubleClick(() => libraryRow("amp-b").querySelector(".resource-browser-item-fav-toggle")!);

    expect(onSelect).not.toHaveBeenCalled();
    expect(modal().style.display).not.toBe("none");
  });

  it("only exempts the controls that are not a way to the resource", () => {
    document.body.innerHTML = `
      <button class="fav"></button>
      <button class="play" data-double-click-selects><svg><polygon></polygon></svg></button>`;
    expect(isDoubleClickExempt(document.querySelector<HTMLElement>(".fav")!)).toBe(true);
    expect(isDoubleClickExempt(document.querySelector<HTMLElement>(".play polygon")!)).toBe(false);
  });

  describe("on the Tone3000 tab", () => {
    const toneList = () => document.getElementById("resource-browser-tone3000-list")!;
    const modelRow = (id: number) => toneList().querySelector<HTMLElement>(`.resource-browser-model[data-model-id="${id}"]`)!;
    const downloads = () => fetchMock.mock.calls.filter(([input]) => String(input).includes("/download/"));

    // Opened on a Tone3000 resource, the browser shows that resource's pack expanded.
    async function openOnSourcePack(onSelect: (id: string) => void, nodeId: string | undefined): Promise<void> {
      browser.open({ resourceType: "nam", nodeId, currentId: imported.id, onSelect });
      await vi.waitFor(() => expect(toneList().querySelectorAll(".resource-browser-model")).toHaveLength(2));
    }

    it.each([
      ["a node, after its first click started a preview", "amp"],
      ["no node to preview on", undefined],
    ])("takes a model already in the library and closes, with %s", async (_with, nodeId) => {
      const onSelect = vi.fn();
      await openOnSourcePack(onSelect, nodeId);

      doubleClick(() => modelRow(101).querySelector(".resource-browser-model-name")!);

      await vi.waitFor(() => expect(onSelect).toHaveBeenCalledWith(imported.id));
      expect(modal().style.display).toBe("none");
    });

    it("selects a model already in the library from that copy, without downloading it", async () => {
      // Without a node nothing is previewed, so any download would be the selection's own.
      await openOnSourcePack(() => {}, undefined);

      doubleClick(() => modelRow(101).querySelector(".resource-browser-model-name")!);

      await vi.waitFor(() => expect(modal().style.display).toBe("none"));
      expect(downloads()).toEqual([]);
    });

    it("downloads a new model once for a double click on Select", async () => {
      await openOnSourcePack(() => {}, "amp");

      doubleClick(() => modelRow(102).querySelector(".resource-browser-model-select")!);

      await vi.waitFor(() => expect(downloads()).toHaveLength(1));
      await new Promise((resolve) => setTimeout(resolve, 20));
      expect(downloads()).toHaveLength(1);
    });
  });
});
