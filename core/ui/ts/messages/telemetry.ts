/**
 * The metering feeds: DSP performance, the signal diagnostics roster, and the
 * per-node analyzer frames.
 *
 * These arrive far faster than the UI can paint, so they are coalesced and
 * flushed at a fixed rate rather than rendered on arrival.
 */

import { requestSignalDiagnosticsRoster } from "../bridge.js";
import { recordDspLoadSample } from "../dspPerformance.js";
import { refreshSavePresetModalPeakInfoIfOpen } from "../presets.js";
import { handleUserInputCalibrationDiagnosticsUpdate } from "../settings.js";
import { updateSelectedNodeAnalyzerPanel, updateSelectedNodeDspStatus, updateSelectedNodePeakMeter } from "../signalPath.js";
import { uiState } from "../state.js";
import { SIGNAL_DIAGNOSTICS_NODE_TUPLE_LENGTH, SIGNAL_DIAGNOSTICS_TUPLE_LENGTH } from "../types.js";
import type { DSPPerformanceStats, InputAnalyzerTelemetry, SignalDiagnosticsAnalyzerFrame, SignalDiagnosticsFrame, SignalDiagnosticsRoster, SignalLevelDiagnostics, SignalLevelMetrics, SignalLevelNodeMetrics } from "../types.js";
import { updateDSPPerformancePlot, updateSignalDiagnosticsView } from "../views.js";
import type { IncomingPayload } from "./types.js";

/* ── Signal diagnostics reassembly ─────────────────────────────────────────
 * The backend streams levels as bare numeric tuples ("sld", 20 Hz) resolved against
 * a roster ("sldRoster") that is only re-sent when the node set changes, plus separate
 * analyzer payloads ("sldA"). Everything is rebuilt here into the SignalLevelDiagnostics
 * shape the views consume, so the wire format stays independent of the render code.
 */
let signalDiagnosticsRoster: SignalDiagnosticsRoster | null = null;

export const signalDiagnosticsAnalyzerByNodeId = new Map<string, InputAnalyzerTelemetry>();

/** Frames arrive at 20 Hz; one roster request per second is enough to recover. */
export const SIGNAL_DIAGNOSTICS_ROSTER_RETRY_MS = 1000;

let signalDiagnosticsRosterRequestedAt = 0;

export function requestSignalDiagnosticsRosterThrottled(): void {
  const now = Date.now();
  if (now - signalDiagnosticsRosterRequestedAt < SIGNAL_DIAGNOSTICS_ROSTER_RETRY_MS) {
    return;
  }
  signalDiagnosticsRosterRequestedAt = now;
  requestSignalDiagnosticsRoster();
}

export function signalDiagnosticsMetrics(values: number[], offset: number): SignalLevelMetrics {
  const peakDbfs = values[offset];
  return {
    peakDbfs,
    rmsDbfs: values[offset + 1],
    // Derived rather than transmitted: the backend computes it the same way.
    headroomDb: Math.max(0, -peakDbfs),
    clipCount: values[offset + 2],
    clipped: values[offset + 3] === 1,
  };
}

export function applySignalDiagnosticsRoster(roster: SignalDiagnosticsRoster): void {
  if (!roster || !Array.isArray(roster.nodes)) {
    return;
  }
  signalDiagnosticsRoster = roster;

  // Drop cached analyzer telemetry for nodes that are no longer present, so a removed
  // analyzer cannot keep painting stale spectrogram data.
  const liveNodeIds = new Set(roster.nodes.map((entry) => entry[2]));
  for (const nodeId of [...signalDiagnosticsAnalyzerByNodeId.keys()]) {
    if (!liveNodeIds.has(nodeId)) {
      signalDiagnosticsAnalyzerByNodeId.delete(nodeId);
    }
  }
}

export function applySignalDiagnosticsFrame(frame: SignalDiagnosticsFrame): boolean {
  const roster = signalDiagnosticsRoster;
  // A frame that predates the roster we hold describes a different node set — drop it
  // and wait for the matching roster rather than mismapping levels onto the wrong nodes.
  if (!roster || frame?.seq !== roster.seq || !Array.isArray(frame.d)) {
    // Nothing will arrive on its own: rosters are only sent when the node set changes,
    // so with none held (or a stale one) this drops every frame forever. Frames are the
    // signal that the backend is streaming and we cannot read it, so ask for a roster.
    if (Array.isArray(frame?.d)) {
      requestSignalDiagnosticsRosterThrottled();
    }
    return false;
  }
  if (frame.d.length !== roster.nodes.length * SIGNAL_DIAGNOSTICS_NODE_TUPLE_LENGTH) {
    return false;
  }

  const nodes: SignalLevelNodeMetrics[] = roster.nodes.map((entry, index) => {
    const [scope, presetId, nodeId, nodeType] = entry;
    const offset = index * SIGNAL_DIAGNOSTICS_NODE_TUPLE_LENGTH;
    const node: SignalLevelNodeMetrics = {
      scope,
      nodeId,
      nodeType,
      // Per-frame, not from the roster: it follows the signal, not the node set.
      channelCount: frame.d[offset + SIGNAL_DIAGNOSTICS_TUPLE_LENGTH],
      levels: signalDiagnosticsMetrics(frame.d, offset),
    };
    if (presetId) {
      node.presetId = presetId;
    }
    const analyzer = signalDiagnosticsAnalyzerByNodeId.get(nodeId);
    if (analyzer) {
      node.analyzer = analyzer;
    }
    return node;
  });

  const diagnostics: SignalLevelDiagnostics = {
    rawInput: signalDiagnosticsMetrics(frame.r, 0),
    input: signalDiagnosticsMetrics(frame.i, 0),
    output: signalDiagnosticsMetrics(frame.o, 0),
    nodes,
  };
  uiState.signalDiagnostics = diagnostics;
  return true;
}

export function applySignalDiagnosticsAnalyzer(frame: SignalDiagnosticsAnalyzerFrame): boolean {
  const roster = signalDiagnosticsRoster;
  if (!roster || frame?.seq !== roster.seq || !frame.id || !Array.isArray(frame.l)) {
    return false;
  }

  const [spectrogramMinDbfs, spectrogramMaxDbfs, spectrogramMinHz, spectrogramMaxHz] = roster.spectrogramRange;
  const [barkMinDbfs, barkMaxDbfs, barkMinHz, barkMaxHz] = roster.barkRange;
  const stereo = frame.l[9] === 1;

  const analyzer: InputAnalyzerTelemetry = {
    levels: {
      peakPercent: frame.l[0],
      rmsPercent: frame.l[1],
      rmsDbu: frame.l[2],
      rmsDbv: frame.l[3],
      rmsVolts: frame.l[4],
      momentaryLufs: frame.l[5],
      shortTermLufs: frame.l[6],
      integratedLufs: frame.l[7],
      activeChannelCount: frame.l[8],
      stereo,
      loudnessValid: frame.l[10] === 1,
      channelMode: stereo ? "stereo" : "mono",
    },
    spectrogram: {
      binsDb: frame.s ?? [],
      minDbfs: spectrogramMinDbfs,
      maxDbfs: spectrogramMaxDbfs,
      minFrequencyHz: spectrogramMinHz,
      maxFrequencyHz: spectrogramMaxHz,
      generatedAtMs: frame.t,
    },
    bark: {
      bandsDb: frame.b ?? [],
      minDbfs: barkMinDbfs,
      maxDbfs: barkMaxDbfs,
      minFrequencyHz: barkMinHz,
      maxFrequencyHz: barkMaxHz,
      generatedAtMs: frame.t,
    },
  };

  signalDiagnosticsAnalyzerByNodeId.set(frame.id, analyzer);

  // Analyzer payloads follow their frame, so patch the snapshot the frame just built
  // rather than waiting a further 50 ms for the next one.
  const node = uiState.signalDiagnostics?.nodes.find((candidate) => candidate.nodeId === frame.id);
  if (node) {
    node.analyzer = analyzer;
  }
  return true;
}

export const TELEMETRY_UI_FPS = 30;

export const TELEMETRY_UI_FRAME_MS = 1000 / TELEMETRY_UI_FPS;

let telemetryUiRafId: number | null = null;

let telemetryUiDelayTimer: number | null = null;

let telemetryUiLastFlushMs = 0;

let telemetryUiPendingDsp = false;

let telemetryUiPendingSignalDiagnostics = false;

export function flushTelemetryUiUpdates(): void {
  if (telemetryUiPendingDsp) {
    updateDSPPerformancePlot();
    updateSelectedNodeDspStatus();
    telemetryUiPendingDsp = false;
  }

  if (telemetryUiPendingSignalDiagnostics) {
    updateSignalDiagnosticsView();
    handleUserInputCalibrationDiagnosticsUpdate();
    updateSelectedNodePeakMeter();
    updateSelectedNodeDspStatus();
    updateSelectedNodeAnalyzerPanel();
    refreshSavePresetModalPeakInfoIfOpen();
    telemetryUiPendingSignalDiagnostics = false;
  }
}

export function scheduleTelemetryUiFlush(): void {
  if (telemetryUiRafId !== null || telemetryUiDelayTimer !== null) {
    return;
  }

  telemetryUiRafId = window.requestAnimationFrame((timestamp) => {
    telemetryUiRafId = null;

    const elapsedMs = timestamp - telemetryUiLastFlushMs;
    if (elapsedMs < TELEMETRY_UI_FRAME_MS) {
      const delayMs = Math.ceil(TELEMETRY_UI_FRAME_MS - elapsedMs);
      telemetryUiDelayTimer = window.setTimeout(() => {
        telemetryUiDelayTimer = null;
        scheduleTelemetryUiFlush();
      }, delayMs);
      return;
    }

    telemetryUiLastFlushMs = timestamp;
    flushTelemetryUiUpdates();
  });
}

export function queueTelemetryUiUpdate(kind: "dsp" | "signalDiagnostics"): void {

  if (kind === "dsp") {
    telemetryUiPendingDsp = true;
  } else {
    telemetryUiPendingSignalDiagnostics = true;
  }

  scheduleTelemetryUiFlush();
}

export function onDspPerformance(payload: IncomingPayload): void {
  const stats = payload as {
    stats?: DSPPerformanceStats;
    sampleRate?: number;
    blockSize?: number;
  };
  if (stats.stats) {
    const mergedStats: DSPPerformanceStats = {
      ...stats.stats,
      sampleRate: stats.sampleRate ?? stats.stats.sampleRate,
      blockSize: stats.blockSize ?? stats.stats.blockSize,
    };
    uiState.dspPerformance = mergedStats;
    recordDspLoadSample(mergedStats.dspLoadPercent);
    queueTelemetryUiUpdate("dsp");
  }
}

export function onSldRoster(payload: IncomingPayload): void {
  applySignalDiagnosticsRoster(payload as unknown as SignalDiagnosticsRoster);
}

export function onSld(payload: IncomingPayload): void {
  if (applySignalDiagnosticsFrame(payload as unknown as SignalDiagnosticsFrame)) {
    queueTelemetryUiUpdate("signalDiagnostics");
  }
}

export function onSldA(payload: IncomingPayload): void {
  if (applySignalDiagnosticsAnalyzer(payload as unknown as SignalDiagnosticsAnalyzerFrame)) {
    queueTelemetryUiUpdate("signalDiagnostics");
  }
}
