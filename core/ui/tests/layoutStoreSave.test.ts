import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/bridge.js", () => ({ postMessage: vi.fn(), setAppSetting: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));

import { postMessage } from "../ts/bridge.js";
import { showNotification } from "../ts/notifications.js";
import { LayoutStore } from "../ts/layoutDesigner/layoutStore.js";
import type { LayoutStoreHost } from "../ts/layoutDesigner/layoutStore.js";
import type { EffectLayout } from "../ts/layoutTypes.js";
import { uiState } from "../ts/state.js";

function makeLayout(layoutId?: string): EffectLayout {
  return {
    effectType: "delay_digital",
    layoutId,
    version: 1,
    name: "Echo Face",
    dimensions: { width: 400, height: 280 },
    backgrounds: [],
    controls: [],
    overlays: [],
    textLabels: [],
  };
}

function makeHost(layout: EffectLayout) {
  const host = {
    layout: layout as EffectLayout | null,
    saving: false,
    closed: [] as boolean[],
    getLayout: () => host.layout,
    setLayout: (next: EffectLayout | null) => { host.layout = next; },
    getEffectType: () => "delay_digital",
    getBlendId: () => "",
    getFallbackLayoutName: () => "Digital Delay",
    isFactoryLayout: () => false,
    setFactoryLayout: vi.fn(),
    isNewLayout: () => true,
    setNewLayout: vi.fn(),
    getParamDefs: () => [],
    getKnownControlKeys: () => new Set<string>(),
    applyLayoutName: vi.fn(),
    updateDeleteButtonVisibility: vi.fn(),
    notifySaved: vi.fn(),
    pushUndoState: vi.fn(),
    refreshEditorChrome: vi.fn(),
    setSaving: (saving: boolean) => { host.saving = saving; },
    renderCanvas: vi.fn(),
    selectElement: vi.fn(),
    takeImportFile: () => null,
    closeModal: (didSave?: boolean) => { host.closed.push(Boolean(didSave)); },
  };
  return host satisfies LayoutStoreHost;
}

function lastSaveMessage(): { type: string; layoutId?: string } | undefined {
  const calls = vi.mocked(postMessage).mock.calls.map(([message]) => message as { type: string; layoutId?: string });
  return calls.filter((message) => message.type === "saveEffectLayout").at(-1);
}

describe("LayoutStore.save", () => {
  beforeEach(() => {
    vi.mocked(postMessage).mockClear();
    vi.mocked(showNotification).mockClear();
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
    // jsdom has no 2D canvas; the thumbnail is skipped.
    HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
  });

  it("stays open until the engine confirms, and keeps the work when it refuses", async () => {
    const host = makeHost(makeLayout("layout-1"));
    const store = new LayoutStore(host);

    await store.save();
    expect(lastSaveMessage()?.layoutId).toBe("layout-1");
    expect(host.saving).toBe(true);
    expect(host.closed).toEqual([]);

    expect(store.handleSaveResult({ layoutId: "layout-1", ok: false, message: "disk full" })).toBe(true);
    expect(host.closed).toEqual([]);
    expect(host.saving).toBe(false);
    expect(vi.mocked(showNotification).mock.calls.at(-1)?.[0]).toContain("disk full");
    expect(uiState.layoutLibrary?.byEffectType.delay_digital).toBeUndefined();

    await store.save();
    expect(store.handleSaveResult({ layoutId: "layout-1", ok: true })).toBe(true);
    expect(host.closed).toEqual([true]);
    expect(uiState.layoutLibrary?.byEffectType.delay_digital?.[0]?.layoutId).toBe("layout-1");
    expect(vi.mocked(showNotification).mock.calls.filter(([text]) => text === "Layout saved")).toHaveLength(1);
  });

  it("ignores an answer for a save it is not waiting on", async () => {
    const host = makeHost(makeLayout("layout-1"));
    const store = new LayoutStore(host);
    expect(store.handleSaveResult({ layoutId: "layout-1", ok: true })).toBe(false);

    await store.save();
    expect(store.handleSaveResult({ layoutId: "layout-2", ok: true })).toBe(false);
    expect(host.closed).toEqual([]);
  });

  it("gives a layout without an id one before sending it, so the engine and the cache agree", async () => {
    const host = makeHost(makeLayout(undefined));
    const store = new LayoutStore(host);

    await store.save();
    const sent = lastSaveMessage();
    expect(sent?.layoutId).toMatch(/^layout-/);
    expect(host.layout?.layoutId).toBe(sent?.layoutId);

    store.handleSaveResult({ layoutId: sent?.layoutId, ok: true });
    expect(uiState.layoutLibrary?.byEffectType.delay_digital?.[0]?.layoutId).toBe(sent?.layoutId);
  });

  it("sends only one save while one is in flight", async () => {
    const host = makeHost(makeLayout("layout-1"));
    const store = new LayoutStore(host);
    await store.save();
    await store.save();
    expect(vi.mocked(postMessage).mock.calls.filter(([message]) => (message as { type: string }).type === "saveEffectLayout")).toHaveLength(1);

    store.cancelPendingSave();
    expect(host.saving).toBe(false);
    expect(store.handleSaveResult({ layoutId: "layout-1", ok: true })).toBe(false);
  });

  it("sends one save for a double click, though the thumbnail is still rendering", async () => {
    const host = makeHost(makeLayout("layout-1"));
    const store = new LayoutStore(host);
    await Promise.all([store.save(), store.save()]);
    expect(vi.mocked(postMessage).mock.calls.filter(([message]) => (message as { type: string }).type === "saveEffectLayout")).toHaveLength(1);
  });
});
