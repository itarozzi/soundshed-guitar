// @vitest-environment node
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";
import { afterEach, describe, expect, it, vi } from "vitest";

// Exercise the feed's private request handlers without booting the native audio bridge.
const source = ts.createSourceFile("panel.ts", readFileSync(new URL("../ts/toneSharingPanel/feed.ts", import.meta.url), "utf8"), ts.ScriptTarget.Latest, true);
const names = new Set(["loadStandardBrowsePage", "resetBrowseCollections", "scheduleCommunitySearch"]);
// `export` is stripped because the extracted text is run as a plain script, not a module.
const handlers = source.statements.filter(node => ts.isFunctionDeclaration(node) && names.has(node.name?.text ?? "")).map(node => node.getText(source).replace(/^export /, "")).join("\n");
function harness(apiFetch = vi.fn().mockResolvedValue({ items: [] })) {
  const context = {
    apiFetch, communitySearchTimer: undefined,
    browseState: { mode: "items", activeSharedTarget: null, featuredHiddenPresetCount: 0, searchQuery: "", tagFilter: "", requestVersion: 0 },
    browseCollections: { page: 1, pageSize: 36, items: [], packs: [], hasMore: false, loadingMore: false },
    renderStandardBrowseCollection: vi.fn(), updateBrowseFooter: vi.fn(),
    element: () => ({ innerHTML: "" }), requestBrowseReload: vi.fn(), setTimeout, clearTimeout,
  };
  const script = ts.transpileModule(handlers, { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;
  const actions = runInNewContext(script + '\n({loadStandardBrowsePage, scheduleCommunitySearch, resetBrowseCollections})', context);
  return { context, actions };
}
afterEach(() => vi.useRealTimers());
describe("community search requests", () => {
  it("sends encoded queries on every page and trusts API matches", async () => {
    const api = vi.fn().mockResolvedValue({ items: [{ id: "older", title: "Different title" }] });
    const { context, actions } = harness(api);
    context.browseState.searchQuery = "blues & clean";
    await actions.loadStandardBrowsePage(1);
    await actions.loadStandardBrowsePage(2, true);
    expect(api.mock.calls.map(call => call[0])).toEqual([
      "/items?page=1&pageSize=36&q=blues%20%26%20clean",
      "/items?page=2&pageSize=36&q=blues%20%26%20clean",
    ]);
    expect(context.browseCollections.items).toHaveLength(2);
    context.browseState.searchQuery = "";
    actions.resetBrowseCollections();
    await actions.loadStandardBrowsePage(1);
    expect(api).toHaveBeenLastCalledWith("/items?page=1&pageSize=36&q=");
    expect(context.browseCollections.items).toHaveLength(1);
  });
  it("keeps tag filters across pages, combines search, and can remove the tag", async () => {
    const api = vi.fn().mockResolvedValue({ items: [] });
    const { context, actions } = harness(api);
    context.browseState.tagFilter = "high-gain";
    context.browseState.searchQuery = "vintage";
    await actions.loadStandardBrowsePage(1);
    await actions.loadStandardBrowsePage(2, true);
    expect(api).toHaveBeenLastCalledWith("/items?page=2&pageSize=36&q=vintage&tag=high-gain");
    context.browseState.tagFilter = "";
    actions.resetBrowseCollections();
    await actions.loadStandardBrowsePage(1);
    expect(api).toHaveBeenLastCalledWith("/items?page=1&pageSize=36&q=vintage");
  });
  it("discards an old page response as soon as a new search starts", async () => {
    vi.useFakeTimers();
    let resolve!: (value: unknown) => void;
    const { context, actions } = harness(vi.fn(() => new Promise(r => { resolve = r; })));
    const pending = actions.loadStandardBrowsePage(2, true);
    context.browseState.searchQuery = "new";
    actions.scheduleCommunitySearch();
    resolve({ items: [{ id: "stale" }] });
    await pending;
    expect(context.browseCollections.items).toEqual([]);
    expect(context.renderStandardBrowseCollection).not.toHaveBeenCalled();
    expect(context.browseCollections.page).toBe(1);
  });
  it("debounces typing and cancels pending search when browsing resets", async () => {
    vi.useFakeTimers();
    const { context, actions } = harness();
    actions.scheduleCommunitySearch();
    await vi.advanceTimersByTimeAsync(150);
    actions.scheduleCommunitySearch();
    await vi.advanceTimersByTimeAsync(249);
    expect(context.requestBrowseReload).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(context.requestBrowseReload).toHaveBeenCalledTimes(1);
    actions.scheduleCommunitySearch();
    actions.resetBrowseCollections();
    await vi.advanceTimersByTimeAsync(300);
    expect(context.requestBrowseReload).toHaveBeenCalledTimes(1);
  });
});
