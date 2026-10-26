/**
 * A long press on a touch screen opens whatever a right-click would.
 *
 * iOS never fires `contextmenu`, so on an iPhone the menus that hang off it (MIDI
 * Learn in midiLearnMenu.ts, the folder tree's Create Pack… in
 * presets/folderContextMenu.ts) had no way in. A touch held still for
 * {@link LONG_PRESS_MS} dispatches a `contextmenu` at the pressed element, so
 * every existing handler serves the long press as well, with no code of its own.
 *
 * Android and Windows raise a `contextmenu` of their own for a long press. Whichever
 * comes first wins: one from the platform cancels the pending press, and once ours
 * has opened a menu, the platform's is stopped before any handler sees it.
 */

import { POINTER_DRAG_THRESHOLD_PX, cancelPointerDrag, swallowNextClick } from "./pointerDrag.js";

/** How long a touch has to stay put to count as a long press. */
export const LONG_PRESS_MS = 500;

/** Movement past this gives the long press up: the finger is scrolling or dragging. */
export const LONG_PRESS_MOVE_TOLERANCE_PX = POINTER_DRAG_THRESHOLD_PX;

interface LongPress {
  pointerId: number;
  target: Element;
  startX: number;
  startY: number;
  timer: number | null;
  /** Our `contextmenu` has gone out for this press. */
  fired: boolean;
  /** And a handler took it, which means a menu is open. */
  opened: boolean;
}

let press: LongPress | null = null;
let bound = false;
/** The contextmenu events this module raised, told apart from the platform's own. */
const raisedHere = new WeakSet<Event>();

function clearPress(): void {
  if (press?.timer != null) {
    window.clearTimeout(press.timer);
  }
  press = null;
}

function fireLongPress(): void {
  const current = press;
  if (!current) return;
  current.timer = null;
  current.fired = true;

  // A re-render during the hold can replace the element; ask what is there now.
  const target = current.target.isConnected
    ? current.target
    : document.elementFromPoint(current.startX, current.startY);
  if (!target) return;

  const menuEvent = new MouseEvent("contextmenu", {
    bubbles: true,
    cancelable: true,
    composed: true,
    clientX: current.startX,
    clientY: current.startY,
    button: 2,
  });
  raisedHere.add(menuEvent);
  current.opened = !target.dispatchEvent(menuEvent);
  if (current.opened) {
    // The menu has the gesture now: the item under the finger must not start a drag,
    // nor take the click that lifting it may still produce.
    cancelPointerDrag();
    swallowNextClick();
  }
}

function onPointerDown(event: PointerEvent): void {
  clearPress();
  if (event.pointerType !== "touch" || !event.isPrimary || !(event.target instanceof Element)) {
    return;
  }
  press = {
    pointerId: event.pointerId,
    target: event.target,
    startX: event.clientX,
    startY: event.clientY,
    timer: window.setTimeout(fireLongPress, LONG_PRESS_MS),
    fired: false,
    opened: false,
  };
}

function onPointerMove(event: PointerEvent): void {
  if (!press || press.fired || press.pointerId !== event.pointerId) return;
  if (Math.hypot(event.clientX - press.startX, event.clientY - press.startY) > LONG_PRESS_MOVE_TOLERANCE_PX) {
    clearPress();
  }
}

function onPointerEnd(event: PointerEvent): void {
  if (press?.pointerId === event.pointerId) {
    clearPress();
  }
}

function onNativeContextMenu(event: MouseEvent): void {
  if (raisedHere.has(event) || !press) return;
  if (press.opened) {
    event.preventDefault();
    event.stopImmediatePropagation();
  } else if (!press.fired) {
    clearPress();
  }
}

/** Starts listening; later calls do nothing. */
export function initializeTouchLongPress(): void {
  if (bound) return;
  bound = true;
  // Capture phase, so a control that stops its own pointer events still gets a long press.
  document.addEventListener("pointerdown", onPointerDown, true);
  document.addEventListener("pointermove", onPointerMove, true);
  document.addEventListener("pointerup", onPointerEnd, true);
  document.addEventListener("pointercancel", onPointerEnd, true);
  document.addEventListener("contextmenu", onNativeContextMenu, true);
}

/** Test hook: forget the pending press and the listeners. */
export function resetTouchLongPressForTests(): void {
  clearPress();
  document.removeEventListener("pointerdown", onPointerDown, true);
  document.removeEventListener("pointermove", onPointerMove, true);
  document.removeEventListener("pointerup", onPointerEnd, true);
  document.removeEventListener("pointercancel", onPointerEnd, true);
  document.removeEventListener("contextmenu", onNativeContextMenu, true);
  bound = false;
}
