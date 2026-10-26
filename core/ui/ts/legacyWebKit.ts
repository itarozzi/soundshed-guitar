/**
 * Stand-ins for the web platform features the UI uses that iOS 12's WebKit (Safari 12.1, the
 * engine of the iOS 12 "legacy" build, docs/ios-build.md) does not have. Each is installed only
 * where the feature is missing, so every other WebView (WebView2, current WKWebView, Android,
 * WebKitGTK) runs its own and this module does nothing there.
 *
 * Imported first by main.ts, so that it runs before any module that uses them: on iOS 12 the
 * first missing one used to throw during bootstrap and leave the splash screen showing
 * "Startup failed".
 */

type Global = Record<string, unknown>;

/**
 * matchMedia(...).addEventListener("change"): Safari 14. Safari 12 has only addListener. Its
 * MediaQueryList is also NoInterfaceObject (there is no global `MediaQueryList`, so testing for
 * one finds nothing to patch), hence the prototype comes from a real matchMedia() result.
 */
function installMediaQueryListEvents(): void {
  if (typeof window.matchMedia !== "function") return;
  const proto = Object.getPrototypeOf(window.matchMedia("all")) as Global | null;
  if (!proto || typeof proto.addEventListener === "function") return;

  type Listener = EventListenerOrEventListenerObject;
  type Legacy = { addListener(fn: unknown): void; removeListener(fn: unknown): void };
  const wrapped = new WeakMap<object, (event: unknown) => void>();
  const asFunction = (listener: Listener): ((event: unknown) => void) => {
    if (typeof listener === "function") return listener as (event: unknown) => void;
    let fn = wrapped.get(listener);
    if (!fn) {
      fn = (event: unknown) => listener.handleEvent(event as Event);
      wrapped.set(listener, fn);
    }
    return fn;
  };

  proto.addEventListener = function (this: Legacy, type: string, listener: Listener | null) {
    if (type === "change" && listener) this.addListener(asFunction(listener));
  };
  proto.removeEventListener = function (this: Legacy, type: string, listener: Listener | null) {
    if (type === "change" && listener) this.removeListener(asFunction(listener));
  };
}

/**
 * ResizeObserver: Safari 13.1. Every use here re-lays out from the elements themselves and
 * ignores the entries, so a stand-in that reports on window resizes (and once on observe) is
 * enough: an element that changes size by itself is not noticed, which costs a stale layout,
 * not a broken page.
 */
function installResizeObserver(global: Global): void {
  if (typeof global.ResizeObserver === "function") return;

  type Callback = (entries: Array<{ target: Element; contentRect: DOMRect }>, observer: unknown) => void;
  class ResizeObserverFallback {
    private readonly targets = new Set<Element>();
    private readonly onResize = (): void => this.notify(Array.from(this.targets));

    constructor(private readonly callback: Callback) {}

    observe(target: Element): void {
      if (this.targets.size === 0) window.addEventListener("resize", this.onResize);
      this.targets.add(target);
      requestAnimationFrame(() => this.notify([target]));
    }

    unobserve(target: Element): void {
      this.targets.delete(target);
      if (this.targets.size === 0) window.removeEventListener("resize", this.onResize);
    }

    disconnect(): void {
      this.targets.clear();
      window.removeEventListener("resize", this.onResize);
    }

    private notify(targets: Element[]): void {
      const live = targets.filter((target) => this.targets.has(target));
      if (live.length === 0) return;
      this.callback(live.map((target) => ({ target, contentRect: target.getBoundingClientRect() })), this);
    }
  }

  global.ResizeObserver = ResizeObserverFallback;
}

/** structuredClone: Safari 15.4. Every use here copies plain JSON-shaped data. */
function installStructuredClone(global: Global): void {
  if (typeof global.structuredClone === "function") return;
  global.structuredClone = (value: unknown): unknown =>
    value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

/** crypto.randomUUID: Safari 15.4. A version 4 UUID from getRandomValues, which Safari 11 has. */
function installRandomUuid(): void {
  if (typeof crypto === "undefined" || typeof (crypto as unknown as Global).randomUUID === "function") return;
  (crypto as unknown as Global).randomUUID = (): string => {
    const bytes = crypto.getRandomValues(new Uint8Array(16));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  };
}

/** replaceChildren: Safari 14. */
function installReplaceChildren(): void {
  for (const ctor of [Element, Document, DocumentFragment]) {
    const proto = ctor.prototype as unknown as Global;
    if (typeof proto.replaceChildren === "function") continue;
    proto.replaceChildren = function (this: ParentNode & Node, ...nodes: Array<Node | string>) {
      while (this.lastChild) this.removeChild(this.lastChild);
      this.append(...nodes);
    };
  }
}

/**
 * Pointer Events: Safari 13. Without them every control driven by pointerdown/move/up (knobs,
 * sliders, drag and drop, many taps) is dead on iOS 12, though nothing throws. This makes them
 * from touch events, which is all an iOS 12 device has, following the spec's rules for touch:
 *
 * - a touch pointer is implicitly captured by the element it went down on, so moves and the up
 *   go there, or to the element that called setPointerCapture;
 * - a default prevented on pointermove, or any move while captured, prevents the touchmove, so
 *   a drag does not scroll the page (Safari 12 ignores `touch-action: none`);
 * - a default prevented on pointerdown does not prevent the click, so it is not passed on.
 */
function installPointerEventsFromTouch(global: Global): void {
  if (typeof global.PointerEvent === "function" || !("ontouchstart" in window)) return;

  interface PointerInit extends MouseEventInit {
    pointerId?: number;
    pointerType?: string;
    isPrimary?: boolean;
    width?: number;
    height?: number;
    pressure?: number;
  }

  class PointerEventFromTouch extends MouseEvent {
    readonly pointerId: number;
    readonly pointerType: string;
    readonly isPrimary: boolean;
    readonly width: number;
    readonly height: number;
    readonly pressure: number;
    readonly tiltX = 0;
    readonly tiltY = 0;

    constructor(type: string, init: PointerInit = {}) {
      super(type, init);
      this.pointerId = init.pointerId ?? 0;
      this.pointerType = init.pointerType ?? "";
      this.isPrimary = init.isPrimary ?? false;
      this.width = init.width ?? 1;
      this.height = init.height ?? 1;
      this.pressure = init.pressure ?? 0;
    }
  }

  global.PointerEvent = PointerEventFromTouch;

  const started = new Map<number, Element>();
  const captured = new Map<number, Element>();
  let primaryId: number | null = null;

  const elementProto = Element.prototype as unknown as Global;
  elementProto.setPointerCapture = function (this: Element, pointerId: number) {
    captured.set(pointerId, this);
  };
  elementProto.releasePointerCapture = function (this: Element, pointerId: number) {
    if (captured.get(pointerId) !== this) return;
    captured.delete(pointerId);
    this.dispatchEvent(new PointerEventFromTouch("lostpointercapture", { bubbles: true, pointerId, pointerType: "touch" }));
  };
  elementProto.hasPointerCapture = function (this: Element, pointerId: number) {
    return captured.get(pointerId) === this;
  };

  const pointerIdOf = (touch: Touch): number => touch.identifier + 2; // 1 is the mouse's

  // True unless a listener prevented the default.
  const fire = (type: string, touch: Touch, target: Element): boolean => {
    const pointerId = pointerIdOf(touch);
    const down = type !== "pointerup" && type !== "pointercancel";
    return target.dispatchEvent(
      new PointerEventFromTouch(type, {
        bubbles: true,
        cancelable: type !== "pointercancel",
        composed: true,
        view: window,
        clientX: touch.clientX,
        clientY: touch.clientY,
        screenX: touch.screenX,
        screenY: touch.screenY,
        button: type === "pointermove" ? -1 : 0,
        buttons: down ? 1 : 0,
        pointerId,
        pointerType: "touch",
        isPrimary: pointerId === primaryId,
        width: Math.max(1, (touch.radiusX || 0.5) * 2),
        height: Math.max(1, (touch.radiusY || 0.5) * 2),
        pressure: down ? touch.force || 0.5 : 0,
      }),
    );
  };

  const targetOf = (touch: Touch): Element | undefined => {
    const id = pointerIdOf(touch);
    return captured.get(id) ?? started.get(id);
  };

  const finish = (type: "pointerup" | "pointercancel", event: TouchEvent): void => {
    for (let i = 0; i < event.changedTouches.length; i++) {
      const touch = event.changedTouches[i];
      const id = pointerIdOf(touch);
      const target = targetOf(touch);
      if (target) fire(type, touch, target);
      const holder = captured.get(id);
      if (holder) {
        captured.delete(id);
        holder.dispatchEvent(new PointerEventFromTouch("lostpointercapture", { bubbles: true, pointerId: id, pointerType: "touch" }));
      }
      started.delete(id);
      if (primaryId === id) primaryId = null;
    }
  };

  document.addEventListener(
    "touchstart",
    (event) => {
      for (let i = 0; i < event.changedTouches.length; i++) {
        const touch = event.changedTouches[i];
        const id = pointerIdOf(touch);
        const target = touch.target instanceof Element ? touch.target : document.body;
        if (primaryId === null) primaryId = id;
        started.set(id, target);
        fire("pointerdown", touch, target);
      }
    },
    { capture: true, passive: true },
  );

  // Not passive: it has to be able to stop a drag scrolling the page.
  document.addEventListener(
    "touchmove",
    (event) => {
      let prevent = false;
      for (let i = 0; i < event.changedTouches.length; i++) {
        const touch = event.changedTouches[i];
        const target = targetOf(touch);
        if (!target) continue;
        if (!fire("pointermove", touch, target) || captured.has(pointerIdOf(touch))) prevent = true;
      }
      if (prevent && event.cancelable) event.preventDefault();
    },
    { capture: true, passive: false },
  );

  document.addEventListener("touchend", (event) => finish("pointerup", event), { capture: true, passive: true });
  document.addEventListener("touchcancel", (event) => finish("pointercancel", event), { capture: true, passive: true });
}

const global = window as unknown as Global;
installMediaQueryListEvents();
installResizeObserver(global);
installStructuredClone(global);
installRandomUuid();
installReplaceChildren();
installPointerEventsFromTouch(global);

export {};
