/**
 * The live spectrum drawn behind an EQ curve, so the user can see which
 * frequencies the source is using while they EQ it.
 *
 * The backend taps one node's input at a time (core/src/dsp/SpectrumTap.h) and
 * streams it as "sldS" while a watch is held. This module holds that watch for
 * the whole UI. Any number of EQ curves can ask for a spectrum; the most recent
 * request is the one the backend serves, and the others are told they have none
 * until it is theirs again. That is what happens when the Global EQ modal opens
 * over a node's EQ panel and then closes.
 *
 * The watch is a lease, renewed while held, so a UI that reloads or loses track
 * of a subscription stops the feed within seconds rather than leaving a node
 * tapped for good.
 */

import { sendSpectrumWatch } from "./bridge.js";

export type EqSpectrumScope = "pre" | "post" | "preset";

/** Which node's input to show — addressed as the diagnostics roster does. */
export interface EqSpectrumSource {
  scope: EqSpectrumScope;
  nodeId: string;
  /** "preset" scope only. Omitted means the active preset, as for a param edit. */
  presetId?: string;
}

export interface EqSpectrum {
  /** dB per bin, log-spaced from minFrequencyHz to maxFrequencyHz, both included. */
  binsDb: readonly number[];
  minFrequencyHz: number;
  maxFrequencyHz: number;
  floorDb: number;
  ceilingDb: number;
}

/** Called with a fresh spectrum, or null when there is none to show. */
export type EqSpectrumListener = (spectrum: EqSpectrum | null) => void;

/** Renewal interval. The backend's lease is 5 s (kSpectrumWatchLease). */
export const EQ_SPECTRUM_RENEW_MS = 2000;

/** A spectrum that stops arriving is cleared after this. The feed runs at 30 Hz. */
export const EQ_SPECTRUM_STALE_MS = 500;

/** How long a dropped watch is held before release, so a panel that rebuilds and
 * subscribes again straight away does not stop and restart the backend tap. */
export const EQ_SPECTRUM_RELEASE_DELAY_MS = 250;

interface Subscription {
  source: EqSpectrumSource;
  listener: EqSpectrumListener;
}

const subscriptions: Subscription[] = [];
/** What the backend has been asked to tap. */
let watched: EqSpectrumSource | null = null;
/** The newest frame for `watched`. */
let latest: EqSpectrum | null = null;
let renewTimer: ReturnType<typeof setInterval> | null = null;
let releaseTimer: ReturnType<typeof setTimeout> | null = null;
let staleTimer: ReturnType<typeof setTimeout> | null = null;
let notifyPending = false;

function sameSource(a: EqSpectrumSource, b: EqSpectrumSource): boolean {
  return a.scope === b.scope && a.nodeId === b.nodeId && (a.presetId ?? "") === (b.presetId ?? "");
}

function topSubscription(): Subscription | undefined {
  return subscriptions[subscriptions.length - 1];
}

function listenersForWatched(): EqSpectrumListener[] {
  const target = watched;
  return target ? subscriptions.filter((entry) => sameSource(entry.source, target)).map((entry) => entry.listener) : [];
}

function clearStaleTimer(): void {
  if (staleTimer !== null) {
    clearTimeout(staleTimer);
    staleTimer = null;
  }
}

function notifyWatched(): void {
  notifyPending = false;
  for (const listener of listenersForWatched()) {
    listener(latest);
  }
}

/** Frames arrive at 30 Hz; drawing is left to the next animation frame so a
 * burst never paints twice, and a hidden view does not paint at all. */
function scheduleNotify(): void {
  if (notifyPending) {
    return;
  }
  notifyPending = true;
  if (typeof window !== "undefined" && typeof window.requestAnimationFrame === "function") {
    window.requestAnimationFrame(notifyWatched);
  } else {
    notifyWatched();
  }
}

function release(): void {
  releaseTimer = null;
  if (topSubscription() || !watched) {
    return;
  }
  watched = null;
  latest = null;
  clearStaleTimer();
  if (renewTimer !== null) {
    clearInterval(renewTimer);
    renewTimer = null;
  }
  sendSpectrumWatch(null);
}

/** Points the backend at the newest subscription, or schedules letting go. */
function syncWatch(): void {
  const top = topSubscription();
  if (!top) {
    if (watched && releaseTimer === null) {
      releaseTimer = setTimeout(release, EQ_SPECTRUM_RELEASE_DELAY_MS);
    }
    return;
  }

  if (releaseTimer !== null) {
    clearTimeout(releaseTimer);
    releaseTimer = null;
  }
  if (watched && sameSource(watched, top.source)) {
    return;
  }

  watched = { ...top.source };
  latest = null;
  clearStaleTimer();
  sendSpectrumWatch(watched);
  if (renewTimer === null) {
    renewTimer = setInterval(() => {
      if (watched) {
        sendSpectrumWatch(watched);
      }
    }, EQ_SPECTRUM_RENEW_MS);
  }
}

/**
 * Asks for the spectrum of `source`. The listener is called on every frame
 * while this is the newest subscription, and with null when it has nothing to
 * show. Returns the unsubscribe function.
 */
export function subscribeEqSpectrum(source: EqSpectrumSource, listener: EqSpectrumListener): () => void {
  const subscription: Subscription = { source: { ...source }, listener };
  const previousTop = topSubscription();
  subscriptions.push(subscription);

  // The one it displaces keeps its subscription but loses the feed.
  if (previousTop && !sameSource(previousTop.source, subscription.source)) {
    previousTop.listener(null);
  }
  syncWatch();
  if (latest) {
    listener(latest);
  }

  let subscribed = true;
  return () => {
    if (!subscribed) {
      return;
    }
    subscribed = false;
    const index = subscriptions.indexOf(subscription);
    const wasTop = index === subscriptions.length - 1;
    if (index >= 0) {
      subscriptions.splice(index, 1);
    }
    if (!wasTop) {
      return;
    }
    syncWatch();
    // Whoever is on top now may already be the source being fed.
    if (latest) {
      scheduleNotify();
    }
  };
}

function finiteNumber(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

/** Handles an "sldS" frame. Returns false for a frame nobody asked for. */
export function applyEqSpectrumFrame(payload: Record<string, unknown>): boolean {
  const target = watched;
  if (!target || payload.scope !== target.scope || payload.id !== target.nodeId) {
    return false;
  }
  // The backend fills in the active preset when the UI named none, so a preset id
  // is only compared when both sides have one.
  if (target.presetId && typeof payload.presetId === "string" && payload.presetId !== target.presetId) {
    return false;
  }

  const bins = payload.s;
  const range = payload.r;
  if (!Array.isArray(bins) || bins.length < 2 || !Array.isArray(range) || range.length < 4) {
    return false;
  }
  const [minFrequencyHz, maxFrequencyHz, floorDb, ceilingDb] = range.map(finiteNumber);
  if (minFrequencyHz === null || maxFrequencyHz === null || floorDb === null || ceilingDb === null
    || minFrequencyHz <= 0 || maxFrequencyHz <= minFrequencyHz || ceilingDb <= floorDb) {
    return false;
  }

  latest = {
    binsDb: bins.map((value) => finiteNumber(value) ?? floorDb),
    minFrequencyHz,
    maxFrequencyHz,
    floorDb,
    ceilingDb,
  };

  clearStaleTimer();
  staleTimer = setTimeout(() => {
    staleTimer = null;
    latest = null;
    scheduleNotify();
  }, EQ_SPECTRUM_STALE_MS);
  scheduleNotify();
  return true;
}

/**
 * Holds a spectrum subscription for as long as `element` is on screen, so a
 * curve inside a closed modal or a scrolled-away panel costs nothing. Call
 * destroy() when the curve goes; after that the callback is never called.
 */
export class EqSpectrumWatcher {
  readonly element: Element;
  private readonly resolveSource: () => EqSpectrumSource | null;
  private readonly onSpectrum: EqSpectrumListener;
  private observer: IntersectionObserver | null = null;
  private unsubscribe: (() => void) | null = null;
  private current: EqSpectrum | null = null;
  private destroyed = false;

  /** `resolveSource` is asked each time the element comes into view, so it can
   * follow a node whose id is only known then. Returning null shows nothing. */
  constructor(element: Element, resolveSource: () => EqSpectrumSource | null, onSpectrum: EqSpectrumListener) {
    this.element = element;
    this.resolveSource = resolveSource;
    this.onSpectrum = onSpectrum;

    if (typeof IntersectionObserver === "undefined") {
      this.setWatching(true);
      return;
    }
    this.observer = new IntersectionObserver((entries) => {
      const entry = entries[entries.length - 1];
      this.setWatching(Boolean(entry?.isIntersecting));
    });
    this.observer.observe(this.element);
  }

  /** The spectrum last delivered, for a redraw that is not itself a frame. */
  get spectrum(): EqSpectrum | null {
    return this.current;
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.observer?.disconnect();
    this.observer = null;
    this.unsubscribe?.();
    this.unsubscribe = null;
    this.current = null;
  }

  private setWatching(watching: boolean): void {
    if (this.destroyed || watching === (this.unsubscribe !== null)) {
      return;
    }
    if (!watching) {
      const unsubscribe = this.unsubscribe;
      this.unsubscribe = null;
      unsubscribe?.();
      this.deliver(null);
      return;
    }
    const source = this.resolveSource();
    if (source) {
      this.unsubscribe = subscribeEqSpectrum(source, (spectrum) => this.deliver(spectrum));
    }
  }

  private deliver(spectrum: EqSpectrum | null): void {
    if (this.destroyed) {
      return;
    }
    this.current = spectrum;
    this.onSpectrum(spectrum);
  }
}
