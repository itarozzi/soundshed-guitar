/**
 * Reading the engine's `dspPerformance` feed.
 *
 * The feed is a UI-only diagnostic: it drives the performance panel's plot and the
 * per-node time and latency readouts that show where a chain's budget is going. Nothing
 * is persisted from it and nothing outside the app sees it.
 *
 * Both per-node maps are keyed `<scope>::<nodeId>`, because a node id only identifies a
 * node within one executor — every executor has an `__input__`, and the same preset in
 * two mixer slots repeats every id it has. `nodeDspPerformanceKey()` is the one place
 * that shape is written down; the signal-path panel and the diagnostics table both go
 * through it rather than assembling the key themselves.
 */

import { uiState } from "./state.js";
import type { SignalLevelNodeMetrics } from "./types.js";

/** Points retained for the performance plot. At kDspPerformanceStatsRateHz that is the
 *  last ~20 seconds of load. */
const DSP_LOAD_HISTORY_POINTS = 100;

/** Appends one frame's load to the plot history, dropping the oldest point past the
 *  window. Only the load is kept: the plot reads nothing else from a past frame, and
 *  retaining whole frames pinned a node map per point. */
export function recordDspLoadSample(loadPercent: number): void {
  uiState.dspLoadHistoryPercent.push(loadPercent);
  if (uiState.dspLoadHistoryPercent.length > DSP_LOAD_HISTORY_POINTS) {
    uiState.dspLoadHistoryPercent.shift();
  }
}

/** How far back the output readout's load peak looks. */
export const DSP_PEAK_WINDOW_MS = 10_000;

/** Each frame's `peakDspLoadPercent` (the worst block since the frame before) and when it came. */
const peakSamples: { at: number; percent: number }[] = [];

/** Keeps one frame's peak, dropping those older than the window. */
export function recordDspPeakSample(percent: number | undefined, now = performance.now()): void {
  if (typeof percent === "number" && Number.isFinite(percent)) {
    peakSamples.push({ at: now, percent });
  }
  while (peakSamples.length && now - peakSamples[0].at > DSP_PEAK_WINDOW_MS) {
    peakSamples.shift();
  }
}

/** The worst block in the last DSP_PEAK_WINDOW_MS, or null before any frame carried one. */
export function recentPeakDspLoadPercent(now = performance.now()): number | null {
  let peak: number | null = null;
  for (const sample of peakSamples) {
    if (now - sample.at <= DSP_PEAK_WINDOW_MS && (peak === null || sample.percent > peak)) {
      peak = sample.percent;
    }
  }
  return peak;
}

/** The key a node's entries live under in both `dspPerformance` node maps. Private: the
 *  two lookups below are the whole of what callers need. */
function nodeDspPerformanceKey(node: SignalLevelNodeMetrics): string {
  if (node.scope === "preset") {
    return `${node.presetId ?? uiState.activePresetId ?? ""}::${node.nodeId}`;
  }
  return `${node.scope}::${node.nodeId}`;
}

/** Last block's processing time for one node, or null if it did not run. */
export function nodeDspProcessingTimeUs(node: SignalLevelNodeMetrics): number | null {
  const timeUs = uiState.dspPerformance?.nodeProcessingTimesUs?.[nodeDspPerformanceKey(node)];
  return typeof timeUs === "number" && Number.isFinite(timeUs) ? timeUs : null;
}

/** One node's algorithmic latency, or null if the engine has not reported it. */
export function nodeDspLatencySamples(node: SignalLevelNodeMetrics): number | null {
  const samples = uiState.dspPerformance?.nodeLatencySamples?.[nodeDspPerformanceKey(node)];
  return typeof samples === "number" && Number.isFinite(samples) ? samples : null;
}

/** A node's share of the block, as a percentage, or null if either figure is missing. */
export function nodeDspProcessingSharePercent(timeUs: number | null | undefined): number | null {
  if (typeof timeUs !== "number" || !Number.isFinite(timeUs)) {
    return null;
  }

  const totalUs = uiState.dspPerformance?.totalProcessingTimeUs ?? 0;
  return totalUs > 0 ? (timeUs / totalUs) * 100.0 : null;
}
