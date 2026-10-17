// @vitest-environment node
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";
import { describe, expect, it, vi } from "vitest";

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
  const browseState = { mode: "featured", activeSharedTarget: null as null | { kind: string; id: string }, featuredHiddenPresetCount: 0, requestVersion: 0 };
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
    updateFeaturedMoreLink: vi.fn(),
    updateBrowseFooter: vi.fn(),
    applyFeaturedLayout: (rows: unknown[]) => ({ rows, hiddenPresetCount: 0 }),
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
  it("drops a featured reply that lands after the switch to another tab", async () => {
    const home = deferred<{ rows: Array<{ title: string }> }>();
    const { browseState, rendered, actions } = harness((path) => (path === "/home" ? home.promise : Promise.resolve({})));

    const featured = actions.loadBrowse();
    browseState.mode = "items";
    await actions.loadBrowse();
    home.resolve({ rows: [{ title: "Featured row" }] });
    await featured;

    expect(rendered).toEqual([{ titles: ["items page"] }]);
  });

  it("drops a featured reply that lands after the switch to My Content", async () => {
    const home = deferred<{ rows: Array<{ title: string }> }>();
    const { browseState, rendered, actions } = harness((path) =>
      path === "/home" ? home.promise : Promise.resolve(path.startsWith("/items") ? { items: [] } : { packs: [] }));

    const featured = actions.loadBrowse();
    browseState.mode = "mine";
    await actions.loadMine();
    home.resolve({ rows: [{ title: "Featured row" }] });
    await featured;

    expect(rendered).toEqual([{ titles: ["My Presets on Tone Sharing", "My Packs on Tone Sharing"] }]);
  });
});
