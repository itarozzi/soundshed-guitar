/**
 * A long press on a touch screen raises the contextmenu a right-click would, which is the
 * only way into the MIDI Learn and folder menus on an iPhone.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { TOUCH_DRAG_HOLD_MS, beginPointerDrag, isPointerDragActive, resetPointerDragForTests } from "../ts/pointerDrag";
import { LONG_PRESS_MOVE_TOLERANCE_PX, LONG_PRESS_MS, initializeTouchLongPress, resetTouchLongPressForTests } from "../ts/touchLongPress";

/** jsdom has no PointerEvent; a MouseEvent carrying the pointer fields is enough. */
function pointer(type: string, x: number, y: number, pointerType = "touch"): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  Object.defineProperty(event, "isPrimary", { value: true });
  Object.defineProperty(event, "pointerType", { value: pointerType });
  return event as PointerEvent;
}

/** A contextmenu the platform raised itself, as Android and Windows do for a long press. */
function nativeContextMenu(): MouseEvent {
  return new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
}

let row: HTMLElement;
let menus: MouseEvent[];
/** Whether the contextmenu handler takes the event, as one that opens a menu does. */
let takeMenu: boolean;

function onContextMenu(event: MouseEvent): void {
  menus.push(event);
  if (takeMenu) event.preventDefault();
}

beforeEach(() => {
  vi.useFakeTimers();
  document.body.innerHTML = `<div id="row" class="row"><span id="label">Folder</span></div>`;
  row = document.getElementById("row") as HTMLElement;
  menus = [];
  takeMenu = true;
  document.addEventListener("contextmenu", onContextMenu);
  initializeTouchLongPress();
});

afterEach(() => {
  resetTouchLongPressForTests();
  resetPointerDragForTests();
  document.removeEventListener("contextmenu", onContextMenu);
  vi.useRealTimers();
  document.body.innerHTML = "";
});

describe("touch long press", () => {
  it("raises a contextmenu at the pressed element after the hold", () => {
    const label = document.getElementById("label") as HTMLElement;
    label.dispatchEvent(pointer("pointerdown", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS - 1);
    expect(menus).toHaveLength(0);

    vi.advanceTimersByTime(1);
    expect(menus).toHaveLength(1);
    expect(menus[0].target).toBe(label);
    expect(menus[0].clientX).toBe(40);
    expect(menus[0].clientY).toBe(20);
  });

  it("leaves a mouse to its own right-click", () => {
    row.dispatchEvent(pointer("pointerdown", 40, 20, "mouse"));
    vi.advanceTimersByTime(LONG_PRESS_MS * 2);
    expect(menus).toHaveLength(0);
  });

  it("tolerates a finger's jitter, and gives up past it", () => {
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    document.dispatchEvent(pointer("pointermove", 40 + LONG_PRESS_MOVE_TOLERANCE_PX, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS);
    expect(menus).toHaveLength(1);

    row.dispatchEvent(pointer("pointerdown", 40, 20));
    document.dispatchEvent(pointer("pointermove", 40, 20 + LONG_PRESS_MOVE_TOLERANCE_PX + 1));
    vi.advanceTimersByTime(LONG_PRESS_MS);
    expect(menus).toHaveLength(1);
  });

  it("does nothing for a press lifted or cancelled before the hold is up", () => {
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS / 2);
    document.dispatchEvent(pointer("pointerup", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS);

    row.dispatchEvent(pointer("pointerdown", 40, 20));
    document.dispatchEvent(pointer("pointercancel", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS);

    expect(menus).toHaveLength(0);
  });

  it("swallows the click that lifting the finger produces once a menu is open", () => {
    const onClick = vi.fn();
    row.addEventListener("click", onClick);
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS);
    document.dispatchEvent(pointer("pointerup", 40, 20));
    row.click();
    expect(onClick).not.toHaveBeenCalled();

    row.dispatchEvent(pointer("pointerdown", 40, 20));
    document.dispatchEvent(pointer("pointerup", 40, 20));
    row.click();
    expect(onClick).toHaveBeenCalledTimes(1);
  });

  it("keeps the click when nothing took the contextmenu", () => {
    takeMenu = false;
    const onClick = vi.fn();
    row.addEventListener("click", onClick);
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS);
    document.dispatchEvent(pointer("pointerup", 40, 20));
    row.click();
    expect(menus).toHaveLength(1);
    expect(onClick).toHaveBeenCalledTimes(1);
  });

  it("cancels a drag the press had armed, so the row does not move under the menu", () => {
    const press = pointer("pointerdown", 40, 20);
    row.dispatchEvent(press);
    const onDrop = vi.fn();
    beginPointerDrag(press, {
      source: row,
      sourceSelector: ".row",
      previewClass: "preview",
      touchHoldMs: TOUCH_DRAG_HOLD_MS,
      resolveTarget: () => ({ element: row }),
      setTargetHighlight: () => undefined,
      onDrop,
    });
    vi.advanceTimersByTime(LONG_PRESS_MS);
    document.dispatchEvent(pointer("pointermove", 40, 80));
    document.dispatchEvent(pointer("pointerup", 40, 80));

    expect(menus).toHaveLength(1);
    expect(isPointerDragActive()).toBe(false);
    expect(onDrop).not.toHaveBeenCalled();
  });

  it("stops the platform's own long-press contextmenu once ours has opened a menu", () => {
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS);
    const native = nativeContextMenu();
    row.dispatchEvent(native);

    expect(menus).toHaveLength(1);
    expect(native.defaultPrevented).toBe(true);
  });

  it("stands down when the platform raises its contextmenu first", () => {
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    vi.advanceTimersByTime(LONG_PRESS_MS / 2);
    const native = nativeContextMenu();
    row.dispatchEvent(native);
    vi.advanceTimersByTime(LONG_PRESS_MS);

    expect(menus).toEqual([native]);
  });

  it("raises the menu on what is under the finger when a re-render replaced the row", () => {
    row.dispatchEvent(pointer("pointerdown", 40, 20));
    document.body.innerHTML = `<div id="fresh">Folder</div>`;
    const fresh = document.getElementById("fresh") as HTMLElement;
    document.elementFromPoint = () => fresh;
    vi.advanceTimersByTime(LONG_PRESS_MS);

    expect(menus).toHaveLength(1);
    expect(menus[0].target).toBe(fresh);
  });
});
