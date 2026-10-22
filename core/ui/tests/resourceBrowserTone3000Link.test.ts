import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const mocks = vi.hoisted(() => ({
  fetchModels: vi.fn(),
  notify: vi.fn(),
}));
vi.mock("../ts/tone3000.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  ensureTone3000Session: async () => undefined,
  isTone3000AuthReady: () => true,
}));
vi.mock("../ts/tone3000Shared.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  fetchTone3000Models: mocks.fetchModels,
}));
vi.mock("../ts/notifications.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  showNotification: mocks.notify,
}));

import { Features, getFeatureSettingKey } from "../ts/featureFlags.js";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource } from "../ts/types.js";

const markup = readFileSync(join(__dirname, "..", "ui-components", "modals", "resource-browser-modal.html"), "utf8");

const fromTone3000: LibraryResource = {
  id: "tone3000:418176",
  name: "JCM800 2203 - G4",
  category: "amp",
  description: "",
  filePath: "C:/NAM/jcm-g4.nam",
  metadata: { provider: "tone3000", toneId: "1071", toneTitle: "Marshall JCM 800 2203", modelId: "418176", architectureVersion: "2" },
} as LibraryResource;
const local: LibraryResource = {
  id: "local-1", name: "My Capture", category: "amp", description: "", filePath: "C:/NAM/mine.nam", metadata: { provider: "local" },
} as LibraryResource;

function activeTab(): string | undefined {
  return document.querySelector<HTMLElement>(".resource-browser-tab-btn.active")?.dataset.tab;
}

function selectedRow(): string | undefined {
  return document.querySelector<HTMLElement>("#resource-browser-library-list .resource-browser-item-row.is-selected")?.dataset.resourceId;
}

describe("the Library details panel's way back to Tone3000", () => {
  const previousLibrary = uiState.resourceLibrary;
  const previousSettings = uiState.appSettings;

  beforeEach(() => {
    document.body.innerHTML = markup;
    window.IPlugSendMsg = () => {};
    uiState.resourceLibrary = { ...previousLibrary, nam: [fromTone3000, local] };
    uiState.appSettings = { ...previousSettings, [getFeatureSettingKey(Features.Tone3000)]: true };
    mocks.fetchModels.mockReset().mockResolvedValue([{ id: 418176, name: "JCM800 2203 - G4", model_url: "https://x/g4.nam" }]);
    mocks.notify.mockReset();
    vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
    if (!("scrollIntoView" in Element.prototype)) {
      Element.prototype.scrollIntoView = () => {};
    }
  });

  afterEach(async () => {
    await new Promise((resolve) => requestAnimationFrame(resolve));
    delete window.IPlugSendMsg;
    vi.unstubAllGlobals();
    uiState.resourceLibrary = previousLibrary;
    uiState.appSettings = previousSettings;
    document.body.innerHTML = "";
  });

  function openDetails(resourceId: string): HTMLElement {
    new ResourceBrowserModal().open({ resourceType: "nam", contextKey: "test-link", onSelect: () => {} });
    const list = document.getElementById("resource-browser-library-list")!;
    document.querySelector<HTMLElement>('.resource-browser-tab-btn[data-tab="library"]')?.click();
    list.querySelector<HTMLElement>(`.resource-browser-item-details-btn[data-resource-id="${resourceId}"]`)!.click();
    return list;
  }

  it("offers a Tone3000 model's tone, and opens the Tone3000 tab on it", async () => {
    const list = openDetails(fromTone3000.id);
    const button = list.querySelector<HTMLButtonElement>(".resource-browser-details-tone3000-btn");
    expect(button?.textContent?.trim()).toBe('Open "Marshall JCM 800 2203" in Tone3000');

    button!.click();
    await vi.waitFor(() => expect(mocks.fetchModels).toHaveBeenCalled());

    expect(activeTab()).toBe("tone3000");
    const tone = document.querySelector<HTMLElement>("#resource-browser-tone3000-list .resource-browser-tone");
    expect(tone?.dataset.toneId).toBe("1071");
    // Opened on the model's own architecture, as opening the picker on it does.
    expect(mocks.fetchModels).toHaveBeenCalledWith(expect.objectContaining({ id: "1071" }), "2");
    expect(mocks.notify).not.toHaveBeenCalled();
  });

  it("offers nothing for a model that did not come from Tone3000, or with Tone3000 off", () => {
    expect(openDetails(local.id).querySelector(".resource-browser-details-tone3000-btn")).toBeNull();

    uiState.appSettings = { ...uiState.appSettings, [getFeatureSettingKey(Features.Tone3000)]: false };
    expect(openDetails(fromTone3000.id).querySelector(".resource-browser-details-tone3000-btn")).toBeNull();
  });

  it("falls back to the Library when the link's tone is refused", async () => {
    mocks.fetchModels.mockRejectedValue(new Error("Model fetch failed: 403"));
    openDetails(fromTone3000.id).querySelector<HTMLButtonElement>(".resource-browser-details-tone3000-btn")!.click();

    await vi.waitFor(() => expect(mocks.notify).toHaveBeenCalled());
    expect(activeTab()).toBe("library");
    expect(mocks.notify.mock.calls[0][1]).toContain("Model fetch failed: 403");
  });
});

describe("a picker opened on a model from Tone3000 whose tone will not open", () => {
  const previousLibrary = uiState.resourceLibrary;
  const previousSettings = uiState.appSettings;

  beforeEach(() => {
    document.body.innerHTML = markup;
    window.IPlugSendMsg = () => {};
    uiState.resourceLibrary = { ...previousLibrary, nam: [fromTone3000, local] };
    uiState.appSettings = { ...previousSettings, [getFeatureSettingKey(Features.Tone3000)]: true };
    mocks.notify.mockReset();
    mocks.fetchModels.mockReset();
    vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
    if (!("scrollIntoView" in Element.prototype)) {
      Element.prototype.scrollIntoView = () => {};
    }
  });

  afterEach(async () => {
    await new Promise((resolve) => requestAnimationFrame(resolve));
    delete window.IPlugSendMsg;
    vi.unstubAllGlobals();
    uiState.resourceLibrary = previousLibrary;
    uiState.appSettings = previousSettings;
    document.body.innerHTML = "";
  });

  const openOnModel = (): void => new ResourceBrowserModal().open({
    resourceType: "nam", contextKey: "test-fallback", nodeId: "amp", currentId: fromTone3000.id, onSelect: () => {},
  });

  it("shows the model selected in the Library when its pack is refused (403)", async () => {
    mocks.fetchModels.mockRejectedValue(new Error("Model fetch failed: 403"));
    openOnModel();

    await vi.waitFor(() => expect(mocks.notify).toHaveBeenCalledOnce());
    expect(activeTab()).toBe("library");
    expect(selectedRow()).toBe(fromTone3000.id);
    expect(mocks.notify.mock.calls[0][0]).toBe("Tone3000 pack unavailable");
  });

  it("does the same when the pack has no models left", async () => {
    mocks.fetchModels.mockResolvedValue([]);
    openOnModel();

    await vi.waitFor(() => expect(mocks.notify).toHaveBeenCalledOnce());
    expect(activeTab()).toBe("library");
    expect(selectedRow()).toBe(fromTone3000.id);
  });

  it("clears a remembered search that would hide the model", async () => {
    mocks.fetchModels.mockRejectedValue(new Error("Model fetch failed: 403"));
    const browser = new ResourceBrowserModal();
    browser.open({ resourceType: "nam", contextKey: "test-fallback", nodeId: "amp", currentId: local.id, onSelect: () => {} });
    const search = document.getElementById("resource-browser-library-search") as HTMLInputElement;
    search.value = "My Capture";
    search.dispatchEvent(new Event("input"));
    document.querySelector<HTMLButtonElement>("#resource-browser-cancel")!.click();

    browser.open({ resourceType: "nam", contextKey: "test-fallback", nodeId: "amp", currentId: fromTone3000.id, onSelect: () => {} });
    await vi.waitFor(() => expect(mocks.notify).toHaveBeenCalledOnce());
    expect(search.value).toBe("");
    expect(selectedRow()).toBe(fromTone3000.id);
  });

  it("stays on the Tone3000 tab when the pack opens", async () => {
    mocks.fetchModels.mockResolvedValue([{ id: 418176, name: "JCM800 2203 - G4", model_url: "https://x/g4.nam" }]);
    openOnModel();

    await vi.waitFor(() => expect(document.querySelector("#resource-browser-tone3000-list .resource-browser-model")).not.toBeNull());
    expect(activeTab()).toBe("tone3000");
    expect(mocks.notify).not.toHaveBeenCalled();
  });
});
