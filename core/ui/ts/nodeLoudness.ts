/**
 * The loudness in a node's DSP status readout: momentary, short-term and integrated, as BS.1770
 * and EBU R128 define them.
 *
 * The backend measures one node's output at a time (core/src/dsp/LoudnessTap.h), and only while a
 * watch is held, streaming it as "sldL" at 10 Hz. The audio thread does no more than copy that one
 * node's samples aside; the measuring happens off it. The DSP status readout holds the watch while
 * it is open, and this module keeps that watch, renews it and holds the newest reading.
 *
 * Moving the watch to another node restarts the backend's measurement, integrated value included,
 * so the integrated figure is always "since this readout opened on this node". Like the spectrum's,
 * the watch is a lease, so a UI that reloads stops the measurement within seconds.
 */

import { sendLoudnessWatch } from "./bridge.js";

/** Which node's output to measure, addressed as the diagnostics roster does. */
export interface NodeLoudnessSource {
  scope: "pre" | "post" | "preset";
  nodeId: string;
  /** "preset" scope only. Omitted means the active preset, as for a param edit. */
  presetId?: string;
}

export interface NodeLoudness {
  /** The last 400 ms, or null until 400 ms have been measured, or for silence. */
  momentaryLufs: number | null;
  /** The last 3 s, or null until 3 s have been measured, or for silence. */
  shortTermLufs: number | null;
  /** Gated over everything measured, or null while nothing has passed the gates. */
  integratedLufs: number | null;
  /** How long the integrated value has been measuring. */
  integratedSeconds: number;
}

/** Renewal interval. The backend's lease is 5 s (kLoudnessWatchLease). */
export const NODE_LOUDNESS_RENEW_MS = 2000;

/** A reading that stops arriving is cleared after this. The feed runs at 10 Hz. */
export const NODE_LOUDNESS_STALE_MS = 1000;

let watched: NodeLoudnessSource | null = null;
let latest: NodeLoudness | null = null;
let listener: (() => void) | null = null;
let renewTimer: ReturnType<typeof setInterval> | null = null;
let staleTimer: ReturnType<typeof setTimeout> | null = null;

function sameSource(a: NodeLoudnessSource | null, b: NodeLoudnessSource | null): boolean {
  if (!a || !b) {
    return a === b;
  }
  return a.scope === b.scope && a.nodeId === b.nodeId && (a.presetId ?? "") === (b.presetId ?? "");
}

function clearStaleTimer(): void {
  if (staleTimer !== null) {
    clearTimeout(staleTimer);
    staleTimer = null;
  }
}

/** Called whenever the reading changes, a frame or its going stale. One listener: the readout. */
export function setNodeLoudnessListener(next: (() => void) | null): void {
  listener = next;
}

/**
 * Points the backend at `source`, or lets go with null. Repeating the current source does
 * nothing, so this is cheap to call on every refresh of the readout.
 */
export function setNodeLoudnessWatch(source: NodeLoudnessSource | null): void {
  if (sameSource(watched, source)) {
    return;
  }

  watched = source ? { ...source } : null;
  latest = null;
  clearStaleTimer();
  sendLoudnessWatch(watched);

  if (!watched && renewTimer !== null) {
    clearInterval(renewTimer);
    renewTimer = null;
  } else if (watched && renewTimer === null) {
    renewTimer = setInterval(() => {
      if (watched) {
        sendLoudnessWatch(watched);
      }
    }, NODE_LOUDNESS_RENEW_MS);
  }

  listener?.();
}

/** The newest reading for the watched node, or null when there is none. */
export function getNodeLoudness(): NodeLoudness | null {
  return latest;
}

function lufsOrNull(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

/** Handles an "sldL" frame. Returns false for a frame nobody asked for. */
export function applyNodeLoudnessFrame(payload: Record<string, unknown>): boolean {
  const target = watched;
  if (!target || payload.scope !== target.scope || payload.id !== target.nodeId) {
    return false;
  }
  // The backend fills in the active preset when the UI named none, so a preset id is only
  // compared when both sides have one.
  if (target.presetId && typeof payload.presetId === "string" && payload.presetId !== target.presetId) {
    return false;
  }

  // [momentaryLufs, shortTermLufs, integratedLufs, integratedSeconds]
  const values = payload.l;
  if (!Array.isArray(values) || values.length < 4) {
    return false;
  }

  latest = {
    momentaryLufs: lufsOrNull(values[0]),
    shortTermLufs: lufsOrNull(values[1]),
    integratedLufs: lufsOrNull(values[2]),
    integratedSeconds: lufsOrNull(values[3]) ?? 0,
  };

  clearStaleTimer();
  staleTimer = setTimeout(() => {
    staleTimer = null;
    latest = null;
    listener?.();
  }, NODE_LOUDNESS_STALE_MS);
  listener?.();
  return true;
}
