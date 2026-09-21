/**
 * Asking the engine how an effect sounds: its response curve for a set of
 * parameters, exporting it as an IR, and matching the Simple Cabinet to a
 * library IR (core/src/controller/PluginControllerEffectAnalysis.cpp).
 *
 * The engine answers from the voicing it actually plays, so a curve drawn from
 * it cannot drift from what is heard the way a TypeScript copy of the design
 * would. This module only keeps track of requests and replies; panels draw.
 *
 * A curve is asked for on every knob move, so curve requests are coalesced: one
 * is in flight at a time, and whatever changes while it is out are folded into a
 * single follow-up carrying the newest parameters.
 */

import { sendEffectResponseRequest, sendExportEffectAsIr, sendMatchSimpleCabToIr } from "./bridge.js";

/** Points per curve: log-spaced 20 Hz to 20 kHz, plenty for a panel-wide plot. */
export const EFFECT_RESPONSE_POINTS = 160;

/** A request the engine never answers (an older engine, a dropped message) is given up
 * on after this, so it cannot hold back every curve after it. */
export const EFFECT_RESPONSE_TIMEOUT_MS = 3000;

export interface ResponseCurve {
  /** Hz, rising. */
  frequencies: readonly number[];
  /** dB at each frequency. */
  magnitudesDb: readonly number[];
}

/** Called with the curve, or null when the engine has none for this effect. */
export type ResponseListener = (curve: ResponseCurve | null) => void;

interface CurveRequest {
  effectType: string;
  params: Record<string, number>;
  listener: ResponseListener;
}

let requestCounter = 0;
let inFlight: { id: string; request: CurveRequest; timer: ReturnType<typeof setTimeout> } | null = null;
let queued: CurveRequest | null = null;

function nextRequestId(kind: string): string {
  requestCounter += 1;
  return `${kind}-${requestCounter}`;
}

function sendCurveRequest(request: CurveRequest): void {
  const id = nextRequestId("response");
  const timer = setTimeout(() => {
    if (inFlight?.id === id) {
      inFlight = null;
      sendQueued();
    }
  }, EFFECT_RESPONSE_TIMEOUT_MS);
  inFlight = { id, request, timer };
  sendEffectResponseRequest(id, request.effectType, request.params, EFFECT_RESPONSE_POINTS);
}

function sendQueued(): void {
  const next = queued;
  queued = null;
  if (next) {
    sendCurveRequest(next);
  }
}

/**
 * Asks for the response of `effectType` with `params`. The listener is called
 * once, with the curve. While another request is out this one waits, and a later
 * call replaces it: only the newest parameters are worth drawing.
 */
export function requestEffectResponse(
  effectType: string,
  params: Record<string, number>,
  listener: ResponseListener,
): void {
  const request: CurveRequest = { effectType, params: { ...params }, listener };
  if (inFlight) {
    queued = request;
    return;
  }
  sendCurveRequest(request);
}

function numberArray(value: unknown): number[] | null {
  if (!Array.isArray(value) || !value.every((item) => typeof item === "number" && Number.isFinite(item))) {
    return null;
  }
  return value as number[];
}

/** Handles an "effectResponse" reply. Returns false for one nobody is waiting for. */
export function applyEffectResponse(payload: Record<string, unknown>): boolean {
  if (!inFlight || payload.requestId !== inFlight.id) {
    return false;
  }
  const { request, timer } = inFlight;
  clearTimeout(timer);
  inFlight = null;

  const frequencies = numberArray(payload.frequencies);
  const magnitudesDb = numberArray(payload.magnitudesDb);
  const valid = payload.supported === true && frequencies !== null && magnitudesDb !== null
    && frequencies.length >= 2 && frequencies.length === magnitudesDb.length;
  request.listener(valid ? { frequencies, magnitudesDb } : null);
  sendQueued();
  return true;
}

/** The curve's level at `freq`, interpolated linearly in log frequency and held
 * flat beyond its ends. */
export function responseDbAt(curve: ResponseCurve, freq: number): number {
  const { frequencies, magnitudesDb } = curve;
  const last = frequencies.length - 1;
  if (freq <= frequencies[0]) {
    return magnitudesDb[0];
  }
  if (freq >= frequencies[last]) {
    return magnitudesDb[last];
  }
  let high = 1;
  while (frequencies[high] < freq) {
    high += 1;
  }
  const low = high - 1;
  const t = Math.log(freq / frequencies[low]) / Math.log(frequencies[high] / frequencies[low]);
  return magnitudesDb[low] + t * (magnitudesDb[high] - magnitudesDb[low]);
}

// ── Simple Cabinet IR match ──────────────────────────────────────────────

/** A match or an export that has not been answered by then is reported as failed. Both
 * normally take well under a second. */
export const EFFECT_TOOL_TIMEOUT_MS = 20000;

export type SimpleCabMatchResult =
  | { ok: true; params: Record<string, number>; rmsErrorDb: number }
  | { ok: false; error: string };

let pendingMatch: {
  id: string;
  listener: (result: SimpleCabMatchResult) => void;
  timer: ReturnType<typeof setTimeout>;
} | null = null;

/** Asks for the Simple Cabinet settings closest to library IR `resourceId`. A new
 * request supersedes one still out; its answer is dropped. */
export function requestSimpleCabIrMatch(resourceId: string, listener: (result: SimpleCabMatchResult) => void): void {
  if (pendingMatch) {
    clearTimeout(pendingMatch.timer);
  }
  const id = nextRequestId("cab-match");
  const timer = setTimeout(() => {
    if (pendingMatch?.id === id) {
      pendingMatch = null;
      listener({ ok: false, error: "The engine did not answer" });
    }
  }, EFFECT_TOOL_TIMEOUT_MS);
  pendingMatch = { id, listener, timer };
  sendMatchSimpleCabToIr(id, resourceId);
}

/** Handles a "simpleCabIrMatch" reply. Returns false for one nobody is waiting for. */
export function applySimpleCabIrMatch(payload: Record<string, unknown>): boolean {
  if (!pendingMatch || payload.requestId !== pendingMatch.id) {
    return false;
  }
  const { listener, timer } = pendingMatch;
  clearTimeout(timer);
  pendingMatch = null;

  const params = payload.params;
  if (typeof payload.error === "string" || !params || typeof params !== "object") {
    listener({ ok: false, error: typeof payload.error === "string" ? payload.error : "No match came back" });
    return true;
  }
  const numeric = Object.fromEntries(
    Object.entries(params as Record<string, unknown>)
      .filter((entry): entry is [string, number] => typeof entry[1] === "number" && Number.isFinite(entry[1])),
  );
  const rmsErrorDb = typeof payload.rmsErrorDb === "number" ? payload.rmsErrorDb : Number.NaN;
  listener({ ok: true, params: numeric, rmsErrorDb });
  return true;
}

// ── Export as IR ─────────────────────────────────────────────────────────

export type IrExportResult = { ok: true; name: string } | { ok: false; error: string };

/**
 * Renders `effectType` with `params` into the IR library under `name` (the engine
 * makes the name unique). The engine answers with the ordinary resource import
 * messages, which the resource handlers re-dispatch as document events; the one
 * carrying this request's id is the answer.
 */
export function exportEffectAsIr(
  effectType: string,
  params: Record<string, number>,
  name: string,
  listener: (result: IrExportResult) => void,
): void {
  const id = nextRequestId("ir-export");
  let finished = false;
  const finish = (result: IrExportResult): void => {
    if (finished) {
      return;
    }
    finished = true;
    clearTimeout(timer);
    document.removeEventListener("resource-browser:resource-imported", onImported);
    document.removeEventListener("resource-browser:resource-import-failed", onFailed);
    listener(result);
  };
  const timer = setTimeout(() => finish({ ok: false, error: "The engine did not answer" }), EFFECT_TOOL_TIMEOUT_MS);
  const onImported = (event: Event): void => {
    const detail = (event as CustomEvent<{ requestId?: string; name?: string }>).detail;
    if (detail?.requestId === id) {
      finish({ ok: true, name: detail.name ?? name });
    }
  };
  const onFailed = (event: Event): void => {
    const detail = (event as CustomEvent<{ requestId?: string; message?: string }>).detail;
    if (detail?.requestId === id) {
      finish({ ok: false, error: detail.message ?? "Export failed" });
    }
  };
  document.addEventListener("resource-browser:resource-imported", onImported);
  document.addEventListener("resource-browser:resource-import-failed", onFailed);
  sendExportEffectAsIr(id, effectType, { ...params }, name);
}
