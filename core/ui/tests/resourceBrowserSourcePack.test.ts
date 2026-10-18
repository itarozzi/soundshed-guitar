import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource } from "../ts/types.js";
import { tone3000ResourceMetadata, tone3000ToneFromResource } from "../ts/tone3000Shared.js";

const markup = readFileSync(join(__dirname, "..", "ui-components", "modals", "resource-browser-modal.html"), "utf8");
const source: LibraryResource = {
  id: "tone3000:101",
  name: "Source Pack - Clean",
  filePath: "C:\\Models\\clean.nam",
  metadata: { provider: "tone3000", toneId: "42", toneTitle: "Source Pack", modelId: "101", architectureVersion: "1" },
};
const models = [
  { id: 101, name: "Clean", model_url: "https://www.tone3000.com/api/v1/models/101/download/clean.nam" },
  { id: 102, name: "Crunch", model_url: "https://www.tone3000.com/api/v1/models/102/download/crunch.nam" },
];
const json = (body: unknown, status = 200): Response => new Response(JSON.stringify(body), { status });
const list = () => document.getElementById("resource-browser-tone3000-list")!;
const searchButton = () => document.getElementById("resource-browser-tone3000-search-btn")!;

describe("opening a resource's Tone3000 source pack", () => {
  const previousLibrary = uiState.resourceLibrary;
  const previousSettings = uiState.appSettings;
  const previousSession = uiState.tone3000Session;
  let browser: ResourceBrowserModal;
  let fetchMock: ReturnType<typeof vi.fn<typeof fetch>>;
  let sent: Array<Record<string, unknown>>;

  beforeEach(() => {
    document.body.innerHTML = markup;
    uiState.resourceLibrary = { nam: [source], ir: [] };
    uiState.appSettings = {
      "features.tone3000.enabled": true,
      "tone3000.useSoundshedToneSearchApi": true,
    };
    sent = [];
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
    vi.stubGlobal("CSS", { escape: (value: string) => value.replace(/["\\]/g, "\\$&") });
    if (!("scrollIntoView" in Element.prototype)) {
      Element.prototype.scrollIntoView = () => {};
    }
    fetchMock = vi.fn<typeof fetch>(async (input) => {
      const url = new URL(String(input));
      if (url.pathname.endsWith("/models")) {
        return json({ data: models, total_pages: 1 });
      }
      return json({ data: [], total_pages: 1 });
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
    uiState.tone3000Session = previousSession;
    document.body.innerHTML = "";
  });

  function open(currentId = source.id, resourceType: "nam" | "ir" = "nam"): void {
    browser.open({ resourceType, currentId, nodeId: "amp", onSelect: () => {} });
  }

  async function expectModels(): Promise<void> {
    await vi.waitFor(() => expect(list().querySelectorAll(".resource-browser-model")).toHaveLength(2));
  }

  it("opens the exact source pack expanded, without a search, preview or model change", async () => {
    open();
    expect(document.querySelector(".resource-browser-tab-btn.active")?.getAttribute("data-tab")).toBe("tone3000");
    await expectModels();
    expect(list().querySelector(".resource-browser-tone.is-expanded")?.getAttribute("data-tone-id")).toBe("42");
    expect(list().textContent).toContain("Source Pack");
    const urls = fetchMock.mock.calls.map(([input]) => new URL(String(input)));
    expect(urls.map((url) => url.pathname.split("/").pop())).toEqual(["models"]);
    expect(urls.every((url) => url.searchParams.get("architecture") === "1")).toBe(true);
    expect(urls[0].searchParams.get("tone_id")).toBe("42");
    expect(sent.filter((message) => ["updateNodeResource", "previewRemoteResource"].includes(String(message.type)))).toEqual([]);
  });

  it("supports legacy groupId and resources deduplicated to a local library id", async () => {
    uiState.resourceLibrary.nam = [{
      ...source, id: "local-copy", metadata: { provider: "tone3000", groupId: "42" },
    }];
    open("local-copy");
    await expectModels();
    expect(fetchMock.mock.calls.every(([input]) => !new URL(String(input)).searchParams.has("architecture"))).toBe(true);
  });

  it("opens IR packs without the remembered NAM architecture filter", async () => {
    uiState.resourceLibrary.ir = [{ ...source, id: "ir-copy" }];
    open("ir-copy", "ir");
    await expectModels();
    expect(document.querySelector<HTMLSelectElement>("#resource-browser-tone3000-architecture")!.value).toBe("all");
    expect(fetchMock.mock.calls.every(([input]) => !new URL(String(input)).searchParams.has("architecture"))).toBe(true);
  });

  it.each([
    undefined,
    { provider: "tone3000", modelId: "101" },
    { provider: "another-provider", toneId: "42" },
    { provider: "tone3000", toneId: "   " },
  ])("keeps normal browsing when source metadata is insufficient: %j", (metadata) => {
    uiState.resourceLibrary.nam = [{ ...source, metadata }];
    open();
    expect(document.querySelector("[data-tab-panel='library']")?.classList.contains("active")).toBe(true);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("does not open or fetch Tone3000 when the feature is disabled", () => {
    uiState.appSettings["features.tone3000.enabled"] = false;
    open();
    expect(document.querySelector("[data-tab-panel='library']")?.classList.contains("active")).toBe(true);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("shows how to connect when no Tone3000 session is available", async () => {
    uiState.appSettings["tone3000.useSoundshedToneSearchApi"] = false;
    uiState.tone3000Session = null;
    open();
    await vi.waitFor(() => expect(list().textContent).toContain("Add a Tone3000 API key in Settings"));
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("shows an empty pack without importing or previewing", async () => {
    fetchMock.mockResolvedValueOnce(json({ data: [], total_pages: 1 }));
    open();
    await vi.waitFor(() => expect(list().textContent).toContain("No models available"));
    expect(list().querySelector(".resource-browser-tone.is-expanded")?.getAttribute("data-tone-id")).toBe("42");
  });

  it("shows an unavailable pack error and still allows a normal search", async () => {
    fetchMock.mockResolvedValueOnce(json({}, 404));
    open();
    await vi.waitFor(() => expect(list().textContent).toContain("Could not load models: Model fetch failed: 404"));
    searchButton().click();
    await vi.waitFor(() => expect(list().textContent).toContain("No tones found"));
    expect(fetchMock.mock.calls.some(([input]) => String(input).includes("/tones/search?"))).toBe(true);
  });

  it("shows model fetch errors inside the source pack", async () => {
    fetchMock.mockResolvedValueOnce(json({}, 503));
    open();
    await vi.waitFor(() => expect(list().textContent).toContain("Could not load models: Model fetch failed: 503"));
    expect(list().textContent).toContain("Source Pack");
  });

  it("does not let a slow source pack replace a newer search", async () => {
    let resolveModels!: (response: Response) => void;
    fetchMock.mockImplementationOnce(() => new Promise((resolve) => { resolveModels = resolve; }));
    open();
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    searchButton().click();
    await vi.waitFor(() => expect(list().textContent).toContain("No tones found"));
    resolveModels(json({ data: models, total_pages: 1 }));
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(list().textContent).toContain("No tones found");
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it("does not let a slow search replace a newly opened source pack", async () => {
    let resolveSearch!: (response: Response) => void;
    fetchMock.mockImplementationOnce(() => new Promise((resolve) => { resolveSearch = resolve; }));
    open("local");
    document.querySelector<HTMLButtonElement>("[data-tab='tone3000']")!.click();
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    browser.close();
    open();
    await expectModels();
    resolveSearch(json({ data: [{ id: 99, title: "Old search" }], total_pages: 1 }));
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(list().textContent).toContain("Source Pack");
    expect(list().textContent).not.toContain("Old search");
  });

  it("does not leave a cancelled model expansion loading in a newer search", async () => {
    let resolveModels!: (response: Response) => void;
    fetchMock.mockImplementationOnce(() => new Promise((resolve) => { resolveModels = resolve; }));
    open();
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    fetchMock.mockResolvedValueOnce(json({ data: [{ id: 42, title: "Search result" }], total_pages: 1 }));
    searchButton().click();
    await vi.waitFor(() => expect(list().textContent).toContain("Search result"));
    resolveModels(json({ data: models, total_pages: 1 }));
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(list().textContent).not.toContain("Loading");
    expect(list().querySelector(".resource-browser-tone.is-expanded")).toBeNull();
  });

  it("drops a model response after closing and opening a different resource", async () => {
    let resolveModels!: (response: Response) => void;
    fetchMock.mockImplementationOnce(() => new Promise((resolve) => { resolveModels = resolve; }));
    open();
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    browser.close();
    open("local", "ir");
    const before = list().innerHTML;
    resolveModels(json({ data: models, total_pages: 1 }));
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(list().innerHTML).toBe(before);
  });

  it("preserves saved pack attribution when another model is imported from it", () => {
    const metadata = {
      ...source.metadata,
      toneTitle: "Source Pack",
      sourceUrl: "https://www.tone3000.com/tones/source-pack",
      licence: "cc0",
      creatorId: "7",
      creatorName: "Creator",
      creatorUrl: "https://www.tone3000.com/creator",
      authorUsername: "creator",
      gear: "amp",
      platform: "nam",
      imageUrl: "https://www.tone3000.com/pack.png",
    };
    const savedTone = tone3000ToneFromResource({ ...source, metadata });
    expect(savedTone).not.toBeNull();
    expect(tone3000ResourceMetadata(savedTone!, { id: "102", name: "Crunch", architectureVersion: "1" }))
      .toMatchObject({ ...metadata, modelId: "102" });
  });
});
