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
 * is in flight at a time on each lane (one lane per plot), and whatever changes
 * while it is out are folded into a single follow-up carrying the newest
 * parameters. Plots on different lanes (the Global EQ beside a node's EQ) do not
 * hold each other back.
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
  /** Named figures the curve leaves out, for an effect that gives them (Pickup Match's
   * designed pickups). Absent otherwise. */
  summary?: Readonly<Record<string, number>>;
  /** Each band's own curve at the same frequencies, for an EQ asked with `bands`; their
   * sum is `magnitudesDb`. Absent otherwise. */
  bandsDb?: ReadonlyArray<readonly number[]>;
}

/** Called with the curve, or null when the engine has none for this effect. */
export type ResponseListener = (curve: ResponseCurve | null) => void;

interface CurveRequest {
  effectType: string;
  params: Record<string, number>;
  listener: ResponseListener;
  lane: string;
  bands: boolean;
}

interface Lane {
  inFlight: { id: string; request: CurveRequest; timer: ReturnType<typeof setTimeout> } | null;
  queued: CurveRequest | null;
}

/** The lane a request goes on unless it names one. */
const DEFAULT_LANE = "default";

let requestCounter = 0;
const lanes = new Map<string, Lane>();

function laneFor(name: string): Lane {
  let lane = lanes.get(name);
  if (!lane) {
    lane = { inFlight: null, queued: null };
    lanes.set(name, lane);
  }
  return lane;
}

function nextRequestId(kind: string): string {
  requestCounter += 1;
  return `${kind}-${requestCounter}`;
}

function sendCurveRequest(request: CurveRequest): void {
  const lane = laneFor(request.lane);
  const id = nextRequestId("response");
  const timer = setTimeout(() => {
    if (lane.inFlight?.id === id) {
      lane.inFlight = null;
      sendQueued(lane);
    }
  }, EFFECT_RESPONSE_TIMEOUT_MS);
  lane.inFlight = { id, request, timer };
  sendEffectResponseRequest(id, request.effectType, request.params, EFFECT_RESPONSE_POINTS, request.bands);
}

function sendQueued(lane: Lane): void {
  const next = lane.queued;
  lane.queued = null;
  if (next) {
    sendCurveRequest(next);
  }
}

export interface ResponseRequestOptions {
  /** Requests on one lane wait for each other; give each plot its own. */
  lane?: string;
  /** Ask for each band's curve too (`bandsDb`), for an EQ. */
  bands?: boolean;
}

/**
 * Asks for the response of `effectType` with `params`. The listener is called
 * once, with the curve. While another request on the same lane is out this one
 * waits, and a later call replaces it: only the newest parameters are worth drawing.
 */
export function requestEffectResponse(
  effectType: string,
  params: Record<string, number>,
  listener: ResponseListener,
  options: ResponseRequestOptions = {},
): void {
  const request: CurveRequest = {
    effectType,
    params: { ...params },
    listener,
    lane: options.lane ?? DEFAULT_LANE,
    bands: options.bands === true,
  };
  const lane = laneFor(request.lane);
  if (lane.inFlight) {
    lane.queued = request;
    return;
  }
  sendCurveRequest(request);
}

/** What a plot asks the engine to draw: the effect and its parameters. */
export interface ResponseQuery {
  effectType: string;
  params: Record<string, number>;
}

interface TrackedPlot {
  lane: string;
  key: string;
  curve: ResponseCurve | null;
  redraw: () => void;
}

let plotCounter = 0;
const trackedPlots = new WeakMap<object, TrackedPlot>();

/**
 * The engine's latest curve for the plot `owner` (a canvas, a panel), or null
 * until the first arrives. When `query` differs from the one last asked about,
 * the newer is asked for on the plot's own lane and `redraw` is called once it
 * arrives, so a plot draws what it has now and again when the engine answers.
 */
export function trackedEffectResponse(
  owner: object,
  query: ResponseQuery,
  redraw: () => void,
  options: { bands?: boolean } = {},
): ResponseCurve | null {
  let plot = trackedPlots.get(owner);
  if (!plot) {
    plotCounter += 1;
    plot = { lane: `plot-${plotCounter}`, key: "", curve: null, redraw };
    trackedPlots.set(owner, plot);
  }
  plot.redraw = redraw;
  const key = JSON.stringify([query.effectType, Object.entries(query.params).sort(([a], [b]) => a.localeCompare(b)), options.bands === true]);
  if (key !== plot.key) {
    plot.key = key;
    const tracked = plot;
    requestEffectResponse(query.effectType, query.params, (curve) => {
      tracked.curve = curve;
      tracked.redraw();
    }, { lane: plot.lane, bands: options.bands });
  }
  return plot.curve;
}

function numberArray(value: unknown): number[] | null {
  if (!Array.isArray(value) || !value.every((item) => typeof item === "number" && Number.isFinite(item))) {
    return null;
  }
  return value as number[];
}

/** The reply's finite numeric summary entries, as a spreadable `{ summary }`, or nothing. */
function parseSummary(value: unknown): { summary?: Record<string, number> } {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return {};
  }
  const entries = Object.entries(value as Record<string, unknown>)
    .filter((entry): entry is [string, number] => typeof entry[1] === "number" && Number.isFinite(entry[1]));
  return entries.length > 0 ? { summary: Object.fromEntries(entries) } : {};
}

/** Handles an "effectResponse" reply. Returns false for one nobody is waiting for. */
export function applyEffectResponse(payload: Record<string, unknown>): boolean {
  const lane = Array.from(lanes.values()).find((candidate) => candidate.inFlight?.id === payload.requestId);
  if (!lane?.inFlight) {
    return false;
  }
  const { request, timer } = lane.inFlight;
  clearTimeout(timer);
  lane.inFlight = null;

  const frequencies = numberArray(payload.frequencies);
  const magnitudesDb = numberArray(payload.magnitudesDb);
  const valid = payload.supported === true && frequencies !== null && magnitudesDb !== null
    && frequencies.length >= 2 && frequencies.length === magnitudesDb.length;
  request.listener(valid ? { frequencies, magnitudesDb, ...parseSummary(payload.summary), ...parseBands(payload.bandsDb, frequencies.length) } : null);
  sendQueued(lane);
  return true;
}

/** The reply's band curves, as a spreadable `{ bandsDb }`, when every one is complete. */
function parseBands(value: unknown, length: number): { bandsDb?: number[][] } {
  if (!Array.isArray(value)) {
    return {};
  }
  const bands = value.map(numberArray);
  return bands.every((band): band is number[] => band !== null && band.length === length) ? { bandsDb: bands } : {};
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
