import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { resetPointerDragForTests } from "../ts/pointerDrag";
import {
  beginPresetDrag,
  registerPresetDropZone,
  resetPresetDropZonesForTests,
  type PresetDropZoneData,
} from "../ts/presets/drag";

/** jsdom does not implement PointerEvent, and only the geometry matters here. */
function pointerEvent(type: string, x: number, y: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, clientX: x, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  Object.defineProperty(event, "isPrimary", { value: true });
  return event as PointerEvent;
}

const PADS_MARKUP = `
  <div class="pad" data-slot-index="0"><span class="pad-name">Clean</span></div>
  <div class="pad" data-slot-index="1"><button type="button" class="pad-save">Save</button></div>
`;

let underPointer: Element | null;
let drops: { presetId: string; zone: PresetDropZoneData }[];

function element(selector: string): HTMLElement {
  return document.querySelector(selector) as HTMLElement;
}

const nextTask = (): Promise<void> => new Promise((resolve) => setTimeout(resolve, 0));

/** Press on the preset row, then move far enough for the press to become a drag. */
function dragOnto(target: Element | null): void {
  beginPresetDrag(pointerEvent("pointerdown", 10, 10), element("#source"), ".preset-item", "preset-a");
  underPointer = target;
  document.dispatchEvent(pointerEvent("pointermove", 200, 200));
}

/** Release, then let the drop — which waits for the release to finish — run. */
async function release(): Promise<void> {
  document.dispatchEvent(pointerEvent("pointerup", 200, 200));
  await nextTask();
}

beforeEach(() => {
  document.body.innerHTML = `
    <article id="source" class="preset-item" data-id="preset-a">Preset A</article>
    <div id="pads">${PADS_MARKUP}</div>
    <div class="pad" id="stray" data-slot-index="9"></div>
  `;
  underPointer = null;
  drops = [];
  // jsdom does no layout, so what is under the pointer is whatever the test says.
  document.elementFromPoint = vi.fn(() => underPointer);
  registerPresetDropZone(element("#pads"), ".pad", (presetId, zone) => drops.push({ presetId, zone }));
});

afterEach(() => {
  resetPointerDragForTests();
  resetPresetDropZonesForTests();
  Reflect.deleteProperty(document, "elementFromPoint");
  document.body.innerHTML = "";
});

describe("preset drop zones", () => {
  it("hands the dragged preset to the zone under the pointer", async () => {
    dragOnto(element(".pad-save"));
    const pad = element('.pad[data-slot-index="1"]');
    expect(pad.classList.contains("drag-over")).toBe(true);

    await release();

    expect(drops).toEqual([{ presetId: "preset-a", zone: { slotIndex: "1" } }]);
    expect(pad.classList.contains("drag-over")).toBe(false);
  });

  it("drops only once the release is over, so the browser's click still finds the source", async () => {
    dragOnto(element(".pad-name"));
    document.dispatchEvent(pointerEvent("pointerup", 200, 200));
    expect(drops).toEqual([]);

    await nextTask();

    expect(drops).toHaveLength(1);
  });

  it("keeps the zone's data when a re-render replaces it mid-drag", async () => {
    dragOnto(element(".pad-name"));
    // What a backend broadcast does to the pad view.
    element("#pads").innerHTML = PADS_MARKUP;

    await release();

    expect(drops).toEqual([{ presetId: "preset-a", zone: { slotIndex: "0" } }]);
  });

  it("ignores a matching element outside the zone's root", async () => {
    dragOnto(element("#stray"));
    expect(element("#stray").classList.contains("drag-over")).toBe(false);

    await release();

    expect(drops).toEqual([]);
  });

  it("drops nothing when released away from every zone", async () => {
    dragOnto(null);
    await release();

    expect(drops).toEqual([]);
  });
});
