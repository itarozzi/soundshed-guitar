// @vitest-environment node
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";
import { describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

// Preview, below, is run from its module with the network and the page stubbed.
const previewStubs = vi.hoisted(() => ({
  postMessage: vi.fn(),
  toneSharingFetch: vi.fn<(path: string) => Promise<unknown>>(),
}));
vi.mock("../ts/bridge.js", () => ({ postMessage: previewStubs.postMessage }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));
vi.mock("../ts/toneSharingPanel/actionButtons.js", () => ({ syncFeedPreviewButton: vi.fn() }));
vi.mock("../ts/toneSharingPanel/api.js", () => ({
  apiFetch: async () => ({ item: { id: "x" } }),
  toneSharingFetch: previewStubs.toneSharingFetch,
}));
vi.mock("../ts/toneSharingPanel/dom.js", () => ({ element: () => null }));
vi.mock("../ts/toneSharingPanel/format.js", () => ({ resolveCreatorProfileHandle: () => null }));
vi.mock("../ts/toneSharingPanel/hostActions.js", () => ({
  getToneSharingHostActions: () => ({ prepareArchiveRemoteResourcesForPreview: async () => undefined }),
}));
vi.mock("../ts/toneSharingPanel/install.js", () => ({
  readDownload: async (download: { title: string; itemId?: string }) => ({
    title: download.title,
    itemId: download.itemId,
    base64: "UEs=",
    presets: 1,
    resources: 0,
    blends: 0,
    remoteRefs: [],
  }),
}));
const { uiState } = await import("../ts/state.js");
const { previewState } = await import("../ts/toneSharingPanel/state.js");
const { clearPreviewPreset, previewPreset } = await import("../ts/toneSharingPanel/preview.js");

// A slow reply for one Tone Sharing tab (the Featured tab's /home most often) used to land after
// the user had switched tab and replace that tab's feed under its name. Run loadBrowse and loadMine
// on their own, as communitySearch.test.ts does the feed's handlers, with the feed stubbed.
const source = ts.createSourceFile("browse.ts", readFileSync(new URL("../ts/toneSharingPanel/browse.ts", import.meta.url), "utf8"), ts.ScriptTarget.Latest, true);
const names = new Set(["loadBrowse", "loadMine"]);
const handlers = source.statements
  .filter((node) => ts.isFunctionDeclaration(node) && names.has(node.name?.text ?? ""))
  .map((node) => node.getText(source).replace(/^export /, ""))
  .join("\n");

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void } {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((r) => { resolve = r; });
  return { promise, resolve };
}

function harness(apiFetch: (path: string) => Promise<unknown>) {
  const feed = { innerHTML: "" };
  const browseState = { mode: "review", activeSharedTarget: null as null | { kind: string; id: string }, requestVersion: 0 };
  const rendered: Array<{ titles: string[] }> = [];
  const context = {
    apiFetch,
    browseState,
    toneSharingState: { user: { id: "me" }, myItems: [] },
    element: () => feed,
    clearPackDetail: vi.fn(),
    resetBrowseCollections: () => { browseState.requestVersion++; },
    updateActiveSharedFilter: vi.fn(),
    updateCommunityPresetSearchUi: vi.fn(),
    updateBrowseFooter: vi.fn(),
    buildSingleRow: (title: string) => ({ title }),
    renderFeedRows: vi.fn(async (rows: Array<{ title: string }>) => { rendered.push({ titles: rows.map((row) => row.title) }); }),
    loadStandardBrowsePage: vi.fn(async () => { rendered.push({ titles: ["items page"] }); }),
    renderInstalledPacks: vi.fn(),
    viewPack: vi.fn(),
    setUploadStatus: vi.fn(),
    isToneSharingAdmin: () => true,
    encodeURIComponent,
  };
  const script = ts.transpileModule(handlers, { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;
  const actions = runInNewContext(`${script}\n({ loadBrowse, loadMine })`, context) as { loadBrowse: () => Promise<void>; loadMine: () => Promise<void> };
  return { browseState, rendered, actions };
}

describe("Tone Sharing browse", () => {
  // The review queue's two lists stand in for any slow tab.
  const slowReview = () => {
    const pending = deferred<unknown>();
    const api = (path: string) => (path.endsWith("/pending/list")
      ? pending.promise
      : Promise.resolve(path.startsWith("/items") ? { items: [] } : { packs: [] }));
    return { pending, api };
  };

  it("drops a slow tab's reply that lands after the switch to another tab", async () => {
    const { pending, api } = slowReview();
    const { browseState, rendered, actions } = harness(api);

    const review = actions.loadBrowse();
    browseState.mode = "items";
    await actions.loadBrowse();
    pending.resolve({ items: [], packs: [] });
    await review;

    expect(rendered).toEqual([{ titles: ["items page"] }]);
  });

  it("drops a slow tab's reply that lands after the switch to My Content", async () => {
    const { pending, api } = slowReview();
    const { browseState, rendered, actions } = harness(api);

    const review = actions.loadBrowse();
    browseState.mode = "mine";
    await actions.loadMine();
    pending.resolve({ items: [], packs: [] });
    await review;

    expect(rendered).toEqual([{ titles: ["My Presets on Tone Sharing", "My Packs on Tone Sharing"] }]);
  });
});

describe("Tone Sharing preview", () => {
  const download = (itemId: string) => ({
    ok: true,
    status: 200,
    headers: { get: () => "" },
    arrayBuffer: async () => new TextEncoder().encode(itemId).buffer,
  });
  const sent = (type: string) => previewStubs.postMessage.mock.calls
    .map(([message]) => message as { type: string; presetId?: string })
    .filter((message) => message.type === type);

  it("drops a preview still loading when Clear comes first", async () => {
    // Preview A, preview B, Clear while B loads: B's reply used to apply the preview and show
    // the indicator again, after Clear had restored the user's preset and forgotten it.
    previewStubs.postMessage.mockClear();
    uiState.activePresetId = "mine";
    uiState.presetCache = new Map([["mine", { id: "mine", name: "Mine" } as Preset]]);
    const slowB = deferred<unknown>();
    previewStubs.toneSharingFetch.mockImplementation((path) => (path.startsWith("/items/b/") ? slowB.promise : Promise.resolve(download("a"))));

    await previewPreset("a", "A");
    expect(previewState.itemId).toBe("a");
    expect(previewState.priorPresetId).toBe("mine");

    const b = previewPreset("b", "B");
    await clearPreviewPreset();
    expect(sent("loadPreset").map((message) => message.presetId)).toEqual(["mine"]);

    slowB.resolve(download("b"));
    await b;

    expect(sent("previewPresetArchive").map((message) => message.presetId)).toEqual(["preview-a"]);
    expect(previewState.itemId).toBeNull();
    expect(previewState.priorPresetId).toBeNull();
  });
});
