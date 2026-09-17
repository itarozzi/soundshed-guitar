import { afterAll, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/fxSelector.js", () => ({
  CATEGORY_METADATA: {},
  getFxLibraryItems: () => [
    { type: "wah", displayName: "Wah", category: "filter" },
    { type: "octave", displayName: "Octave", category: "pitch" },
  ],
  getOrderedFxCategories: () => ["filter", "pitch"],
}));
vi.mock("../ts/iconAssets.js", () => ({ getBadgeIcon: () => "" }));
vi.mock("../ts/layoutRenderer.js", () => ({ getCustomLayout: () => undefined }));
vi.mock("../ts/pointerDrag.js", () => ({ getUiZoom: () => 1 }));
vi.mock("../ts/signalPath/nodeTypes.js", () => ({ getNodeIcon: () => "" }));
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

function openFrom(from: string, to: string, onChoose: (item: HTMLElement) => void = () => {}): HTMLElement {
  const button = document.querySelector<HTMLElement>(`.signal-add-btn[data-edge-from="${from}"][data-edge-to="${to}"]`)!;
  showAddEffectDropdown(button, onChoose);
  return document.querySelector<HTMLElement>(".effect-selection-dropdown")!;
}

const nextTask = (): Promise<void> => new Promise((resolve) => setTimeout(resolve, 0));

beforeEach(() => {
  document.querySelectorAll(".effect-selection-dropdown").forEach((dropdown) => dropdown.remove());
  document.body.innerHTML = `<div id="signal-path-nodes">${addButton("__input__", "amp", 300, 200)}${addButton("amp", "__output__", 500, 200)}</div>`;
});

describe("add effect dropdown", () => {
  it("opens below the + button it was opened from", () => {
    const dropdown = openFrom("amp", "__output__");
    expect(dropdown.style.left).toBe("500px");
    expect(dropdown.style.top).toBe("225px");
  });

  it("follows the replacement button when the chain re-renders under it, not the top-left corner", () => {
    const dropdown = openFrom("amp", "__output__");

    // A wah went in before the amp, so the same insertion point is now further right.
    renderChain(`${addButton("__input__", "wah", 300, 200)}${addButton("wah", "amp", 450, 200)}${addButton("amp", "__output__", 650, 200)}`);

    expect(dropdown.isConnected).toBe(true);
    expect(dropdown.style.left).toBe("650px");
    expect(dropdown.style.top).toBe("225px");

    window.dispatchEvent(new Event("scroll"));
    expect(dropdown.style.left).toBe("650px");
  });

  it("closes when the chain no longer has the insertion point it was opened for", () => {
    const dropdown = openFrom("__input__", "amp");

    renderChain(`${addButton("__input__", "wah", 300, 200)}${addButton("wah", "amp", 450, 200)}`);

    expect(dropdown.isConnected).toBe(false);
    expect(document.querySelector(".effect-selection-dropdown")).toBeNull();
  });

  it("stays where it is while its own list scrolls", () => {
    const dropdown = openFrom("amp", "__output__");
    document.querySelector<HTMLElement>('.signal-add-btn[data-edge-from="amp"]')!.dataset.left = "700";

    dropdown.dispatchEvent(new Event("scroll"));
    expect(dropdown.style.left).toBe("500px");

    document.dispatchEvent(new Event("scroll"));
    expect(dropdown.style.left).toBe("700px");
  });

  it("hands the chosen row to the caller and closes", () => {
    const onChoose = vi.fn();
    const dropdown = openFrom("amp", "__output__", onChoose);

    dropdown.querySelector<HTMLElement>('.effect-dropdown-item[data-effect-type="wah"]')!.click();

    expect(onChoose).toHaveBeenCalledTimes(1);
    expect((onChoose.mock.calls[0][0] as HTMLElement).dataset.effectType).toBe("wah");
    expect(dropdown.isConnected).toBe(false);
  });

  it("opening another replaces the first, and a click outside closes it", async () => {
    const first = openFrom("__input__", "amp");
    const second = openFrom("amp", "__output__");

    expect(first.isConnected).toBe(false);
    expect(document.querySelectorAll(".effect-selection-dropdown")).toHaveLength(1);

    await nextTask();
    document.body.click();
    expect(second.isConnected).toBe(false);
  });
});
