/**
 * The loudness in the DSP status readouts: momentary, short-term and integrated, as BS.1770 and
 * EBU R128 define them.
 *
 * The backend measures only while a watch is held, and streams the readings as "sldL" at 10 Hz.
 * There are two watches, which run independently:
 *  - a node's output (core/src/dsp/LoudnessTap.h), for an effect's readout, one node at a time;
 *  - the app's output, after the master volume, for the output meter's readout (frames with
 *    `slot: 1`).
 * The audio thread does no more than copy samples aside for either; the measuring happens off it.
 * Each readout holds its watch while it is open and shows a loudness figure, and this module keeps
 * the watches, renews them and holds the newest readings.
 *
 * Starting a watch, or moving the node one to another node, restarts the backend's measurement,
 * integrated value included, so integrated is always "since this readout opened". Like the
 * spectrum's, a watch is a lease, so a UI that reloads stops the measurement within seconds.
 */

import { sendLoudnessWatch, sendOutputLoudnessWatch } from "./bridge.js";

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

function lufsOrNull(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

/** One watch: what it asks the backend for, its renewal, its newest reading and its listener. */
class LoudnessWatch<Source> {
  private watched: Source | null = null;
  private latest: NodeLoudness | null = null;
  private listener: (() => void) | null = null;
  private renewTimer: ReturnType<typeof setInterval> | null = null;
  private staleTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(
    private readonly send: (source: Source | null) => void,
    private readonly same: (a: Source, b: Source) => boolean,
  ) {}

  get source(): Source | null {
    return this.watched;
  }

  get reading(): NodeLoudness | null {
    return this.latest;
  }

  setListener(listener: (() => void) | null): void {
    this.listener = listener;
  }

  set(source: Source | null): void {
    const current = this.watched;
    if (current === null ? source === null : source !== null && this.same(current, source)) {
      return;
    }

    this.watched = source;
    this.latest = null;
    this.clearStaleTimer();
    this.send(source);

    if (source === null && this.renewTimer !== null) {
      clearInterval(this.renewTimer);
      this.renewTimer = null;
    } else if (source !== null && this.renewTimer === null) {
      this.renewTimer = setInterval(() => {
        if (this.watched !== null) {
          this.send(this.watched);
        }
      }, NODE_LOUDNESS_RENEW_MS);
    }

    this.listener?.();
  }

  /** Takes a frame's values, already matched to this watch. False when they are malformed. */
  apply(values: unknown): boolean {
    // [momentaryLufs, shortTermLufs, integratedLufs, integratedSeconds]
    if (!Array.isArray(values) || values.length < 4) {
      return false;
    }

    this.latest = {
      momentaryLufs: lufsOrNull(values[0]),
      shortTermLufs: lufsOrNull(values[1]),
      integratedLufs: lufsOrNull(values[2]),
      integratedSeconds: lufsOrNull(values[3]) ?? 0,
    };

    this.clearStaleTimer();
    this.staleTimer = setTimeout(() => {
      this.staleTimer = null;
      this.latest = null;
      this.listener?.();
    }, NODE_LOUDNESS_STALE_MS);
    this.listener?.();
    return true;
  }

  private clearStaleTimer(): void {
    if (this.staleTimer !== null) {
      clearTimeout(this.staleTimer);
      this.staleTimer = null;
    }
  }
}

const nodeWatch = new LoudnessWatch<NodeLoudnessSource>(
  sendLoudnessWatch,
  (a, b) => a.scope === b.scope && a.nodeId === b.nodeId && (a.presetId ?? "") === (b.presetId ?? ""),
);

const outputWatch = new LoudnessWatch<true>(
  (on) => sendOutputLoudnessWatch(on !== null),
  () => true,
);

/** Called whenever the node reading changes, a frame or its going stale. One listener: the readout. */
export function setNodeLoudnessListener(next: (() => void) | null): void {
  nodeWatch.setListener(next);
}

/**
 * Points the backend at `source`, or lets go with null. Repeating the current source does
 * nothing, so this is cheap to call on every refresh of the readout.
 */
export function setNodeLoudnessWatch(source: NodeLoudnessSource | null): void {
  nodeWatch.set(source ? { ...source } : null);
}

/** The newest reading for the watched node, or null when there is none. */
export function getNodeLoudness(): NodeLoudness | null {
  return nodeWatch.reading;
}

/** Called whenever the output reading changes. One listener: the output meter's readout. */
export function setOutputLoudnessListener(next: (() => void) | null): void {
  outputWatch.setListener(next);
}

/** Holds or lets go of the output watch. Cheap to repeat. */
export function setOutputLoudnessWatch(on: boolean): void {
  outputWatch.set(on ? true : null);
}

/** The newest reading for the app's output, or null when there is none. */
export function getOutputLoudness(): NodeLoudness | null {
  return outputWatch.reading;
}

/** Handles an "sldL" frame. Returns false for a frame nobody asked for. */
export function applyNodeLoudnessFrame(payload: Record<string, unknown>): boolean {
  if (payload.slot === 1) {
    return outputWatch.source !== null && payload.scope === "output" && outputWatch.apply(payload.l);
  }

  const target = nodeWatch.source;
  if (!target || payload.scope !== target.scope || payload.id !== target.nodeId) {
    return false;
  }
  // The backend fills in the active preset when the UI named none, so a preset id is only
  // compared when both sides have one.
  if (target.presetId && typeof payload.presetId === "string" && payload.presetId !== target.presetId) {
    return false;
  }
  return nodeWatch.apply(payload.l);
}
