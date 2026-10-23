import { afterAll, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { UiSettings } from "../ts/types.js";

const library = vi.hoisted(() => ({
  items: [] as Array<{ type: string; displayName: string; category: string; thumbnailDataUrl?: string; parameters: never[]; requiresResource: boolean }>,
}));
const settings = vi.hoisted(() => ({ current: { zoom: 1 } as UiSettings }));

vi.mock("../ts/fxLibrary/catalog.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  CATEGORY_METADATA: {},
  getFxLibraryItems: () => library.items,
}));
vi.mock("../ts/windowSettings.js", () => ({
  getCurrentUiSettings: () => settings.current,
  updateUiSettings: (patch: Partial<UiSettings>) => {
    settings.current = { ...settings.current, ...patch };
  },
  adoptEngineUiSettings: (patch: Partial<UiSettings>) => {
    settings.current = { ...settings.current, ...patch };
  },
}));
vi.mock("../ts/iconAssets.js", () => ({ getBadgeIcon: () => "", getFxCategoryIcon: () => "", getFxEffectIcon: () => "" }));
vi.mock("../ts/layoutRenderer.js", () => ({ getCustomLayout: () => undefined }));
vi.mock("../ts/pointerDrag.js", () => ({ getUiZoom: () => 1 }));
vi.mock("../ts/signalPath/state.js", () => ({
  get signalPathNodesElement() {
    return document.getElementById("signal-path-nodes");
  },
}));

const { reanchorAddEffectDropdown, showAddEffectDropdown } = await import("../ts/signalPath/addEffectDropdown.js");

/**
 * jsdom lays nothing out. A button's box comes from its data-left/data-top, and, as in a
 * browser, an element that is not in the document has no boxes at all.
 */
function boxOf(element: HTMLElement): DOMRect {
  const left = element.isConnected ? Number(element.dataset.left ?? 0) : 0;
  const top = element.isConnected ? Number(element.dataset.top ?? 0) : 0;
  const size = element.isConnected ? 20 : 0;
  return { left, top, right: left + size, bottom: top + size, width: size, height: size, x: left, y: top, toJSON: () => ({}) };
}

// Defined on HTMLElement.prototype, so they shadow jsdom's Element.prototype versions
// until deleted again.
beforeAll(() => {
  Object.defineProperty(HTMLElement.prototype, "getBoundingClientRect", {
    configurable: true,
    value(this: HTMLElement) {
      return boxOf(this);
    },
  });
  Object.defineProperty(HTMLElement.prototype, "getClientRects", {
    configurable: true,
    value(this: HTMLElement) {
      return (this.isConnected ? [boxOf(this)] : []) as unknown as DOMRectList;
    },
  });
});

afterAll(() => {
  Reflect.deleteProperty(HTMLElement.prototype, "getBoundingClientRect");
  Reflect.deleteProperty(HTMLElement.prototype, "getClientRects");
});

function addButton(from: string, to: string, left: number, top: number): string {
  return `<button class="signal-add-btn" data-edge-from="${from}" data-edge-to="${to}"
    data-edge-from-port="0" data-edge-to-port="0" data-left="${left}" data-top="${top}">+</button>`;
}

/** Stands in for a render of the chain: every + button is replaced. */
function renderChain(buttons: string): void {
  document.getElementById("signal-path-nodes")!.innerHTML = buttons;
  reanchorAddEffectDropdown();
}

function openFrom(from: string, to: string, onChoose: (payload: unknown) => void = () => {}): HTMLElement {
  const button = document.querySelector<HTMLElement>(`.signal-add-btn[data-edge-from="${from}"][data-edge-to="${to}"]`)!;
  showAddEffectDropdown(button, onChoose);
  return document.querySelector<HTMLElement>(".fx-flyout")!;
}

const item = (flyout: HTMLElement, type: string): HTMLElement =>
  flyout.querySelector<HTMLElement>(`.fx-item[data-effect-type="${type}"]`)!;
const openTab = (flyout: HTMLElement): string | undefined =>
  flyout.querySelector<HTMLElement>(".fx-category.active")?.dataset.category;
const listed = (flyout: HTMLElement): string[] =>
  Array.from(flyout.querySelectorAll<HTMLElement>(".fx-item")).map((el) => el.dataset.effectType ?? "");
const showTab = (flyout: HTMLElement, tab: string): void =>
  flyout.querySelector<HTMLElement>(`.fx-category[data-category="${tab}"]`)!.click();

const keydown = (target: EventTarget, key: string): void => {
  target.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
};

const nextTask = (): Promise<void> => new Promise((resolve) => setTimeout(resolve, 0));

function effect(type: string, displayName: string, category: string) {
  return { type, displayName, category, parameters: [] as never[], requiresResource: false };
}

beforeEach(() => {
  settings.current = { zoom: 1 };
  library.items = [effect("test-wah", "Wah", "filter"), effect("test-octave", "Octave", "pitch"), effect("test-whammy", "Whammy", "pitch")];
  document.querySelectorAll(".fx-flyout").forEach((dropdown) => dropdown.remove());
  document.body.innerHTML = `<div id="signal-path-nodes">${addButton("__input__", "amp", 300, 200)}${addButton("amp", "__output__", 500, 200)}</div>`;
});

describe("add effect dropdown", () => {
  it("shows a Custom Effect's own category and thumbnail as text and inline images only", () => {
    // A Custom Effect's category and thumbnail are its module's word (its descriptor).
    library.items.push({
      ...effect("wasm", "Custom", `<iframe src="https://example.invalid"></iframe>`),
      thumbnailDataUrl: "https://tracker.example/pixel.png",
    });

    const dropdown = openFrom("amp", "__output__");
    dropdown.querySelector<HTMLInputElement>(".fx-selector-search input")!.value = "custom";
    dropdown.querySelector(".fx-selector-search input")!.dispatchEvent(new Event("input"));

    expect(dropdown.querySelector("iframe")).toBeNull();
    expect(dropdown.textContent).toContain(`<iframe src="https://example.invalid"></iframe>`);
    expect(dropdown.querySelector('img[src^="https:"]')).toBeNull();
  });

  it("opens below the + button it was opened from", () => {
    const dropdown = openFrom("amp", "__output__");
    expect(dropdown.style.left).toBe("500px");
    expect(dropdown.style.top).toBe("225px");
  });

  it("follows the replacement button when the chain re-renders under it, not the top-left corner", () => {
    const dropdown = openFrom("amp", "__output__");

    // A wah went in before the amp, so the same insertion point is now further right.
    renderChain(`${addButton("__input__", "test-wah", 300, 200)}${addButton("test-wah", "amp", 450, 200)}${addButton("amp", "__output__", 650, 200)}`);

    expect(dropdown.isConnected).toBe(true);
    expect(dropdown.style.left).toBe("650px");
    expect(dropdown.style.top).toBe("225px");

    window.dispatchEvent(new Event("scroll"));
    expect(dropdown.style.left).toBe("650px");
  });

  it("closes when the chain no longer has the insertion point it was opened for", () => {
    const dropdown = openFrom("__input__", "amp");

    renderChain(`${addButton("__input__", "test-wah", 300, 200)}${addButton("test-wah", "amp", 450, 200)}`);

    expect(dropdown.isConnected).toBe(false);
    expect(document.querySelector(".fx-flyout")).toBeNull();
  });

  it("stays where it is while its own list scrolls", () => {
    const dropdown = openFrom("amp", "__output__");
    document.querySelector<HTMLElement>('.signal-add-btn[data-edge-from="amp"]')!.dataset.left = "700";

    dropdown.querySelector(".fx-selector-effects-list")!.dispatchEvent(new Event("scroll"));
    expect(dropdown.style.left).toBe("500px");

    document.dispatchEvent(new Event("scroll"));
    expect(dropdown.style.left).toBe("700px");
  });

  it("hands the chosen effect to the caller and closes", () => {
    const onChoose = vi.fn();
    const dropdown = openFrom("amp", "__output__", onChoose);

    showTab(dropdown, "filter");
    item(dropdown, "test-wah").click();

    expect(onChoose).toHaveBeenCalledTimes(1);
    expect(onChoose.mock.calls[0][0]).toMatchObject({ effectType: "test-wah" });
    expect(dropdown.isConnected).toBe(false);
  });

  it("opening another replaces the first, and a click outside closes it", async () => {
    const first = openFrom("__input__", "amp");
    const second = openFrom("amp", "__output__");

    expect(first.isConnected).toBe(false);
    expect(document.querySelectorAll(".fx-flyout")).toHaveLength(1);

    await nextTask();
    document.body.click();
    expect(second.isConnected).toBe(false);
  });

  it("closes on Escape", () => {
    const dropdown = openFrom("amp", "__output__");
    keydown(dropdown.querySelector(".fx-selector-search input")!, "Escape");
    expect(dropdown.isConnected).toBe(false);
  });
});

describe("the chooser's library", () => {
  it("keeps the tab it was asked for while the catalog is still empty", async () => {
    const { FxBrowser } = await import("../ts/fxLibrary/fxBrowser.js");
    library.items = [];
    const host = document.createElement("div");
    host.innerHTML = `<div class="rail"></div><div class="list"></div>`;
    const browser = new FxBrowser({
      categories: host.querySelector<HTMLElement>(".rail")!,
      list: host.querySelector<HTMLElement>(".list")!,
      search: null,
      tab: "pitch",
      getPick: () => null,
    });
    browser.render();
    expect(browser.getTab()).toBe("pitch");

    library.items = [effect("test-octave", "Octave", "pitch")];
    browser.render();
    expect(browser.getTab()).toBe("pitch");
    browser.destroy();
  });

  it("lists the FX library's categories after Favourites and Recent, and opens on a category while both are empty", () => {
    const dropdown = openFrom("amp", "__output__");
    const tabs = Array.from(dropdown.querySelectorAll<HTMLElement>(".fx-category")).map((tab) => tab.dataset.category);

    expect(tabs.slice(0, 2)).toEqual(["favorites", "recent"]);
    expect([...tabs.slice(2)].sort()).toEqual(["filter", "pitch"]);
    expect(["filter", "pitch"]).toContain(openTab(dropdown));
  });

  it("stars an effect without choosing it or closing, and opens on Favourites next time", async () => {
    const onChoose = vi.fn();
    const dropdown = openFrom("amp", "__output__", onChoose);
    showTab(dropdown, "pitch");
    await nextTask();

    item(dropdown, "test-whammy").querySelector<HTMLElement>(".fx-item-fav")!.click();

    expect(onChoose).not.toHaveBeenCalled();
    expect(dropdown.isConnected).toBe(true);
    expect(item(dropdown, "test-whammy").classList.contains("is-favorite")).toBe(true);
    expect(item(dropdown, "test-whammy").querySelector(".fx-item-fav")!.getAttribute("aria-pressed")).toBe("true");

    showTab(dropdown, "favorites");
    expect(listed(dropdown)).toEqual(["test-whammy"]);

    const next = openFrom("amp", "__output__");
    expect(openTab(next)).toBe("favorites");
    expect(listed(next)).toEqual(["test-whammy"]);

    item(next, "test-whammy").querySelector<HTMLElement>(".fx-item-fav")!.click();
    expect(listed(next)).toEqual([]);
    expect(next.textContent).toContain("Star an effect");
  });

  it("lists what was added most recent first, and opens on Recent when there are no favourites", () => {
    let dropdown = openFrom("amp", "__output__");
    showTab(dropdown, "pitch");
    item(dropdown, "test-octave").click();

    dropdown = openFrom("amp", "__output__");
    expect(openTab(dropdown)).toBe("recent");
    showTab(dropdown, "filter");
    item(dropdown, "test-wah").click();

    dropdown = openFrom("amp", "__output__");
    expect(openTab(dropdown)).toBe("recent");
    expect(listed(dropdown)).toEqual(["test-wah", "test-octave"]);
  });

  it("does not list a favourite whose effect has gone", () => {
    settings.current = { zoom: 1, fxFavorites: ["gone||", "test-wah||"] };
    const dropdown = openFrom("amp", "__output__");
    expect(openTab(dropdown)).toBe("favorites");
    expect(listed(dropdown)).toEqual(["test-wah"]);
  });

  it("searches every category, and Enter adds the first match", () => {
    const onChoose = vi.fn();
    const dropdown = openFrom("amp", "__output__", onChoose);
    const search = dropdown.querySelector<HTMLInputElement>(".fx-selector-search input")!;

    search.value = "wh";
    search.dispatchEvent(new Event("input"));
    expect(listed(dropdown)).toEqual(["test-whammy"]);

    keydown(search, "Enter");
    expect(onChoose.mock.calls[0]?.[0]).toMatchObject({ effectType: "test-whammy" });
    expect(dropdown.isConnected).toBe(false);
  });

  it("moves from the search into the list with Down, and chooses with Enter", () => {
    const onChoose = vi.fn();
    const dropdown = openFrom("amp", "__output__", onChoose);
    showTab(dropdown, "pitch");
    const search = dropdown.querySelector<HTMLInputElement>(".fx-selector-search input")!;

    keydown(search, "ArrowDown");
    expect(document.activeElement).toBe(item(dropdown, "test-octave"));
    keydown(item(dropdown, "test-octave"), "ArrowDown");
    expect(document.activeElement).toBe(item(dropdown, "test-whammy"));
    keydown(item(dropdown, "test-whammy"), "Enter");

    expect(onChoose.mock.calls[0]?.[0]).toMatchObject({ effectType: "test-whammy" });
  });
});
