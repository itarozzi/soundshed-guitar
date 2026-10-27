/**
 * Lining up the IR Cabinet's two IRs: asking the engine where IR B lines up with
 * IR A (core/src/controller/PluginControllerEffectAnalysis.cpp, dsp/IrAlignment.h),
 * and for the two summed as the cab plays them (dsp/IrPairResponse.h).
 *
 * The engine reads both IR files and answers once: the start of each at 48 kHz,
 * the offset and polarity that line them up, and how alike the two are at every
 * offset in reach. The match at an offset is read from that answer here, so a drag
 * never waits on the engine for it. The summed response comes from the engine on
 * every change (getIrAlignmentResponse), worked out from the cab's own filter
 * designs and gains, coalesced so a drag sends no more than it can answer. With
 * only one IR loaded it answers with that one alone, and the section draws its
 * response and nothing to align.
 */

import { postMessage, sendAnalyzeIrAlignment } from "./bridge.js";
import type { ResourceRef } from "./types.js";

/** An analysis takes well under a second; one not answered by then has failed. */
export const IR_ALIGNMENT_TIMEOUT_MS = 20000;

export interface IrAlignmentAnalysis {
  /** Which slots had an IR: both, or only A or only B. Only a pair has the
   * alignment fields below; for one IR they are zero and the curve one entry. */
  slots: "both" | "a" | "b";
  /** The rate of the waveforms and of matchByOffset's steps. */
  sampleRate: number;
  /** The start of each IR, mono; empty for a slot with no IR. */
  waveformA: Float32Array;
  waveformB: Float32Array;
  /** The gain the cab's Normalize applies to each. */
  normalizeGainA: number;
  normalizeGainB: number;
  /** The IR B offset, in ms, that lines B up with A. */
  alignedOffsetMs: number;
  /** Whether B lines up with A inverted. */
  invertB: boolean;
  /** How alike the two are once lined up, 0..1. */
  match: number;
  onsetAMs: number;
  onsetBMs: number;
  /** The signed match with B moved by each whole sample, -maxOffsetSamples first. */
  maxOffsetSamples: number;
  matchByOffset: number[];
}

export type IrAlignmentResult = { ok: true; analysis: IrAlignmentAnalysis } | { ok: false; error: string };

let requestCounter = 0;
let pending: {
  id: string;
  listener: (result: IrAlignmentResult) => void;
  timer: ReturnType<typeof setTimeout>;
} | null = null;

/** Asks where `irB` lines up with `irA`, each a ref as the node holds it, or for
 * the start of the one IR when the other slot is null. A new request supersedes
 * one still out; its answer is dropped. */
export function requestIrAlignment(
  irA: ResourceRef | null,
  irB: ResourceRef | null,
  listener: (result: IrAlignmentResult) => void,
): void {
  if (pending) {
    clearTimeout(pending.timer);
  }
  requestCounter += 1;
  const id = `ir-alignment-${requestCounter}`;
  const timer = setTimeout(() => {
    if (pending?.id === id) {
      pending = null;
      listener({ ok: false, error: "The engine did not answer" });
    }
  }, IR_ALIGNMENT_TIMEOUT_MS);
  pending = { id, listener, timer };
  sendAnalyzeIrAlignment(id, irA, irB);
}

/** Little-endian float32 samples, base64'd. */
export function decodeFloat32Base64(text: string): Float32Array {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length - (binary.length % 4));
  for (let i = 0; i < bytes.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return new Float32Array(bytes.buffer);
}

function finiteNumber(value: unknown, fallback = Number.NaN): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

/** Reads an "irAlignment" reply; null when it is not a usable analysis. */
export function parseIrAlignment(payload: Record<string, unknown>): IrAlignmentAnalysis | null {
  const sampleRate = finiteNumber(payload.sampleRate);
  const hasA = typeof payload.waveformA === "string";
  const hasB = typeof payload.waveformB === "string";
  if (!(sampleRate > 0) || (!hasA && !hasB)) {
    return null;
  }
  if (!hasA || !hasB) {
    return {
      slots: hasA ? "a" : "b",
      sampleRate,
      waveformA: hasA ? decodeFloat32Base64(payload.waveformA as string) : new Float32Array(0),
      waveformB: hasB ? decodeFloat32Base64(payload.waveformB as string) : new Float32Array(0),
      normalizeGainA: finiteNumber(payload.normalizeGainA, 1),
      normalizeGainB: finiteNumber(payload.normalizeGainB, 1),
      alignedOffsetMs: 0,
      invertB: false,
      match: 0,
      onsetAMs: 0,
      onsetBMs: 0,
      maxOffsetSamples: 0,
      matchByOffset: [0],
    };
  }
  const matchByOffset = payload.matchByOffset;
  const maxOffsetSamples = finiteNumber(payload.maxOffsetSamples, 0);
  const alignedOffsetMs = finiteNumber(payload.alignedOffsetMs);
  if (!Array.isArray(matchByOffset) || matchByOffset.length !== 2 * maxOffsetSamples + 1
    || !matchByOffset.every((value) => typeof value === "number" && Number.isFinite(value))
    || !Number.isFinite(alignedOffsetMs)) {
    return null;
  }
  return {
    slots: "both",
    sampleRate,
    waveformA: decodeFloat32Base64(payload.waveformA as string),
    waveformB: decodeFloat32Base64(payload.waveformB as string),
    normalizeGainA: finiteNumber(payload.normalizeGainA, 1),
    normalizeGainB: finiteNumber(payload.normalizeGainB, 1),
    alignedOffsetMs,
    invertB: payload.invertB === true,
    match: finiteNumber(payload.match, 0),
    onsetAMs: finiteNumber(payload.onsetAMs, 0),
    onsetBMs: finiteNumber(payload.onsetBMs, 0),
    maxOffsetSamples,
    matchByOffset: matchByOffset as number[],
  };
}

/** Handles an "irAlignment" reply. Returns false for one nobody is waiting for. */
export function applyIrAlignment(payload: Record<string, unknown>): boolean {
  if (!pending || payload.requestId !== pending.id) {
    return false;
  }
  const { listener, timer } = pending;
  clearTimeout(timer);
  pending = null;

  if (typeof payload.error === "string") {
    listener({ ok: false, error: payload.error });
    return true;
  }
  const analysis = parseIrAlignment(payload);
  listener(analysis ? { ok: true, analysis } : { ok: false, error: "No analysis came back" });
  return true;
}

// ── What the section draws ────────────────────────────────────────────────

/** The signed match with B moved by `offsetMs`, between whole samples by straight
 * line; held at the ends beyond the analysis's reach. */
export function matchAtOffset(analysis: IrAlignmentAnalysis, offsetMs: number): number {
  const curve = analysis.matchByOffset;
  const position = Math.max(0, Math.min(curve.length - 1,
    offsetMs * analysis.sampleRate / 1000 + analysis.maxOffsetSamples));
  const below = Math.floor(position);
  const above = Math.min(curve.length - 1, below + 1);
  return curve[below] + (position - below) * (curve[above] - curve[below]);
}

/** Polarity of each slot as the node's params set it: +1 or -1. */
export function slotPolarities(params: Record<string, number>): { a: number; b: number } {
  return {
    a: (params.slotAPolarity ?? 0) >= 0.5 ? -1 : 1,
    b: (params.slotBPolarity ?? 0) >= 0.5 ? -1 : 1,
  };
}

// ── The summed response, from the engine ──────────────────────────────────

/** Points per curve: log-spaced 20 Hz to 20 kHz. */
export const IR_ALIGNMENT_RESPONSE_POINTS = 160;
/** A curve the engine has not answered by then is given up on, so it cannot hold back the next. */
export const IR_ALIGNMENT_RESPONSE_TIMEOUT_MS = 3000;

/** What the curve is asked for: the two IRs as the node holds them, its parameters, and IR B's offset. */
export interface AlignmentResponseQuery {
  irA: ResourceRef | null;
  irB: ResourceRef | null;
  params: Record<string, number>;
  offsetMs: number;
}

export interface AlignmentResponse {
  /** Which slots it was worked out for. */
  slots: "both" | "a" | "b";
  /** Hz, rising. */
  frequencies: number[];
  /** The two summed with B moved by the offset, shaped as the cab shapes them, in dB. */
  summedDb: number[];
  /** The two added as if they could not interfere (their powers summed): where the summed
   * curve falls below this, they are cancelling. Null for one IR alone. */
  unrelatedDb: number[] | null;
}

export type AlignmentResponseResult = { ok: true; response: AlignmentResponse } | { ok: false; error: string };

interface ResponseRequest {
  query: AlignmentResponseQuery;
  listener: (result: AlignmentResponseResult) => void;
}

/** One request out at a time; whatever is asked meanwhile is folded into one follow-up with
 * the newest query, so a drag sends no more than the engine can answer. */
let responseInFlight: { id: string; request: ResponseRequest; timer: ReturnType<typeof setTimeout> } | null = null;
let responseQueued: ResponseRequest | null = null;

function sendResponseRequest(request: ResponseRequest): void {
  requestCounter += 1;
  const id = `ir-alignment-response-${requestCounter}`;
  const timer = setTimeout(() => {
    if (responseInFlight?.id === id) {
      responseInFlight = null;
      request.listener({ ok: false, error: "The engine did not answer" });
      sendQueuedResponseRequest();
    }
  }, IR_ALIGNMENT_RESPONSE_TIMEOUT_MS);
  responseInFlight = { id, request, timer };
  const { irA, irB, params, offsetMs } = request.query;
  postMessage({
    type: "getIrAlignmentResponse",
    requestId: id,
    ...(irA ? { irA } : {}),
    ...(irB ? { irB } : {}),
    params,
    offsetMs,
    points: IR_ALIGNMENT_RESPONSE_POINTS,
  });
}

function sendQueuedResponseRequest(): void {
  const next = responseQueued;
  responseQueued = null;
  if (next) {
    sendResponseRequest(next);
  }
}

/** Asks the engine for the two IRs summed as the cab would play them
 * (getIrAlignmentResponse). While a request is out this one waits, and a later call
 * replaces it: only the newest settings are worth drawing. */
export function requestIrAlignmentResponse(
  query: AlignmentResponseQuery,
  listener: (result: AlignmentResponseResult) => void,
): void {
  const request: ResponseRequest = { query: { ...query, params: { ...query.params } }, listener };
  if (responseInFlight) {
    responseQueued = request;
    return;
  }
  sendResponseRequest(request);
}

function finiteNumbers(value: unknown, length?: number): number[] | null {
  if (!Array.isArray(value) || !value.every((item) => typeof item === "number" && Number.isFinite(item))) {
    return null;
  }
  return length === undefined || value.length === length ? (value as number[]) : null;
}

/** Reads an "irAlignmentResponse" reply; null when it is not a usable curve. */
export function parseIrAlignmentResponse(payload: Record<string, unknown>): AlignmentResponse | null {
  const frequencies = finiteNumbers(payload.frequencies);
  if (!frequencies || frequencies.length < 2) {
    return null;
  }
  const summedDb = finiteNumbers(payload.summedDb, frequencies.length);
  const slots = payload.slots === "a" || payload.slots === "b" ? payload.slots : "both";
  const unrelatedDb = finiteNumbers(payload.unrelatedDb, frequencies.length);
  if (!summedDb || (slots === "both" && !unrelatedDb)) {
    return null;
  }
  return { slots, frequencies, summedDb, unrelatedDb: slots === "both" ? unrelatedDb : null };
}

/** Handles an "irAlignmentResponse" reply. Returns false for one nobody is waiting for. */
export function applyIrAlignmentResponse(payload: Record<string, unknown>): boolean {
  if (!responseInFlight || payload.requestId !== responseInFlight.id) {
    return false;
  }
  const { request, timer } = responseInFlight;
  clearTimeout(timer);
  responseInFlight = null;

  if (typeof payload.error === "string") {
    request.listener({ ok: false, error: payload.error });
  } else {
    const response = parseIrAlignmentResponse(payload);
    request.listener(response ? { ok: true, response } : { ok: false, error: "No response came back" });
  }
  sendQueuedResponseRequest();
  return true;
}

interface TrackedResponse {
  key: string;
  result: AlignmentResponseResult | null;
  redraw: () => void;
}

const trackedResponses = new WeakMap<object, TrackedResponse>();

function refKey(ref: ResourceRef | null): string {
  return ref ? [ref.resourceId ?? ref.id ?? "", ref.filePath ?? "", ref.embeddedId ?? ""].join("|") : "-";
}

/**
 * The engine's latest curve for the plot `owner` (a canvas), or null until the first
 * arrives. When `query` differs from the one last asked about, the newer is asked for
 * and `redraw` is called once the engine answers, so a plot draws what it has now and
 * again with the answer.
 */
export function trackedIrAlignmentResponse(
  owner: object,
  query: AlignmentResponseQuery,
  redraw: () => void,
): AlignmentResponseResult | null {
  let tracked = trackedResponses.get(owner);
  if (!tracked) {
    tracked = { key: "", result: null, redraw };
    trackedResponses.set(owner, tracked);
  }
  tracked.redraw = redraw;
  const key = JSON.stringify([
    refKey(query.irA),
    refKey(query.irB),
    Object.entries(query.params).sort(([a], [b]) => a.localeCompare(b)),
    query.offsetMs,
  ]);
  if (key !== tracked.key) {
    tracked.key = key;
    const plot = tracked;
    requestIrAlignmentResponse(query, (result) => {
      // A failure keeps the last curve on screen: the next change asks again.
      plot.result = result.ok || !plot.result?.ok ? result : plot.result;
      plot.redraw();
    });
  }
  return tracked.result;
}
