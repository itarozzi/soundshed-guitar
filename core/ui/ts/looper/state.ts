/**
 * The looper as the engine last described it (looperState, looperLayers, looperWaveforms), and
 * what the strip shows open. One owner: the handlers write it through these functions, the strip
 * reads it. Nothing here touches the DOM.
 */

export type LooperStateName =
  | "empty"
  | "countIn"
  | "armed"
  | "recording"
  | "playing"
  | "overdubArmed"
  | "overdubbing"
  | "stopped";

export interface LooperSettings {
  enabled: boolean;
  maxSeconds: number;
  memoryBudgetMb: number;
  level: number;
  start: "press" | "armed";
  length: "free" | "bars";
  bars: number;
  countInBars: number;
  clickWhileRecording: boolean;
  afterRecord: "play" | "overdub";
  armThresholdDb: number;
  alignOffsetMs: number;
}

export interface LooperStatus {
  available: boolean;
  enabled: boolean;
  state: LooperStateName;
  playing: boolean;
  positionSec: number;
  lengthSec: number;
  canUndo: boolean;
  canRedo: boolean;
  layerCount: number;
  memoryMb: number;
  tempoLocked: boolean;
  settings: LooperSettings;
  bars?: number;
  barIndex?: number;
  tempoBpm?: number;
  countInBeatsLeft?: number;
  armWaitingSec?: number;
  /** How much of the take being recorded there is so far. */
  recordedSec?: number;
  notice?: string;
}

export interface LooperLayer {
  id: string;
  title: string;
  order: number;
  recording: boolean;
  firstLayer: boolean;
  startSec: number;
  lengthSec: number;
  level: number;
  balance: number;
  muted: boolean;
  solo: boolean;
  presetName: string;
  toneChanged: boolean;
}

export interface LooperPeaks {
  left: number[];
  right: number[];
}

export interface LooperLivePeaks {
  layerId: string;
  bucketSamples: number;
  recordedSamples: number;
  peaks: number[];
}

const STATE_NAMES: readonly LooperStateName[] = [
  "empty", "countIn", "armed", "recording", "playing", "overdubArmed", "overdubbing", "stopped",
];

export const DEFAULT_LOOPER_SETTINGS: LooperSettings = {
  enabled: false,
  maxSeconds: 60,
  memoryBudgetMb: 512,
  level: 1,
  start: "press",
  length: "free",
  bars: 4,
  countInBars: 1,
  clickWhileRecording: true,
  afterRecord: "play",
  armThresholdDb: -50,
  alignOffsetMs: 0,
};

let status: LooperStatus | null = null;
let layers: LooperLayer[] = [];
const layerPeaks = new Map<string, LooperPeaks>();
let livePeaks: LooperLivePeaks | null = null;
let stripOpen = false;
let layersOpen = false;

const num = (value: unknown, fallback: number): number =>
  typeof value === "number" && Number.isFinite(value) ? value : fallback;
const bool = (value: unknown, fallback = false): boolean => (typeof value === "boolean" ? value : fallback);
const str = (value: unknown, fallback = ""): string => (typeof value === "string" ? value : fallback);
const numbers = (value: unknown): number[] => (Array.isArray(value) ? value.map((v) => num(v, 0)) : []);
const optional = (value: unknown): number | undefined =>
  typeof value === "number" && Number.isFinite(value) ? value : undefined;

export function parseLooperSettings(value: unknown): LooperSettings {
  const source = value && typeof value === "object" ? (value as Record<string, unknown>) : {};
  const d = DEFAULT_LOOPER_SETTINGS;
  return {
    enabled: bool(source.enabled, d.enabled),
    maxSeconds: num(source.maxSeconds, d.maxSeconds),
    memoryBudgetMb: num(source.memoryBudgetMb, d.memoryBudgetMb),
    level: num(source.level, d.level),
    start: source.start === "armed" ? "armed" : "press",
    length: source.length === "bars" ? "bars" : "free",
    bars: num(source.bars, d.bars),
    countInBars: num(source.countInBars, d.countInBars),
    clickWhileRecording: bool(source.clickWhileRecording, d.clickWhileRecording),
    afterRecord: source.afterRecord === "overdub" ? "overdub" : "play",
    armThresholdDb: num(source.armThresholdDb, d.armThresholdDb),
    alignOffsetMs: num(source.alignOffsetMs, d.alignOffsetMs),
  };
}

export function parseLooperStatus(payload: Record<string, unknown>): LooperStatus {
  const state = STATE_NAMES.includes(payload.state as LooperStateName) ? (payload.state as LooperStateName) : "empty";
  return {
    available: bool(payload.available),
    enabled: bool(payload.enabled),
    state,
    playing: bool(payload.playing),
    positionSec: num(payload.positionSec, 0),
    lengthSec: num(payload.lengthSec, 0),
    canUndo: bool(payload.canUndo),
    canRedo: bool(payload.canRedo),
    layerCount: num(payload.layerCount, 0),
    memoryMb: num(payload.memoryMb, 0),
    tempoLocked: bool(payload.tempoLocked),
    settings: parseLooperSettings(payload.settings),
    bars: optional(payload.bars),
    barIndex: optional(payload.barIndex),
    tempoBpm: optional(payload.tempoBpm),
    countInBeatsLeft: optional(payload.countInBeatsLeft),
    armWaitingSec: optional(payload.armWaitingSec),
    recordedSec: optional(payload.recordedSec),
    notice: typeof payload.notice === "string" ? payload.notice : undefined,
  };
}

export function parseLooperLayers(value: unknown): LooperLayer[] {
  if (!Array.isArray(value)) {
    return [];
  }

  return value.map((entry) => {
    const layer = entry && typeof entry === "object" ? (entry as Record<string, unknown>) : {};
    const span = layer.span && typeof layer.span === "object" ? (layer.span as Record<string, unknown>) : {};
    const tone = layer.tone && typeof layer.tone === "object" ? (layer.tone as Record<string, unknown>) : {};
    return {
      id: str(layer.id),
      title: str(layer.title),
      order: num(layer.order, 0),
      recording: bool(layer.recording),
      firstLayer: bool(layer.firstLayer),
      startSec: num(span.startSec, 0),
      lengthSec: num(span.lengthSec, 0),
      level: num(layer.level, 1),
      balance: num(layer.balance, 0),
      muted: bool(layer.muted),
      solo: bool(layer.solo),
      presetName: str(tone.presetName),
      toneChanged: bool(layer.toneChanged),
    };
  }).filter((layer) => layer.id !== "");
}

export function setLooperStatus(next: LooperStatus): void {
  status = next;

  if (next.state === "empty" && next.lengthSec === 0) {
    layerPeaks.clear();
    livePeaks = null;
  }
}

export function getLooperStatus(): LooperStatus | null {
  return status;
}

export function setLooperLayers(next: LooperLayer[]): void {
  layers = next;
  const ids = new Set(next.map((layer) => layer.id));

  for (const id of [...layerPeaks.keys()]) {
    if (!ids.has(id)) {
      layerPeaks.delete(id);
    }
  }

  if (livePeaks && !next.some((layer) => layer.id === livePeaks?.layerId && layer.recording)) {
    livePeaks = null;
  }
}

export function getLooperLayers(): readonly LooperLayer[] {
  return layers;
}

export function setLooperWaveforms(payload: Record<string, unknown>): void {
  const entries = payload.layers && typeof payload.layers === "object" ? (payload.layers as Record<string, unknown>) : {};

  for (const [id, value] of Object.entries(entries)) {
    const peaks = value && typeof value === "object" ? (value as Record<string, unknown>) : {};
    layerPeaks.set(id, { left: numbers(peaks.peaksL), right: numbers(peaks.peaksR) });
  }

  const live = payload.live && typeof payload.live === "object" ? (payload.live as Record<string, unknown>) : null;

  if (live) {
    livePeaks = {
      layerId: str(live.layerId),
      bucketSamples: num(live.bucketSamples, 1),
      recordedSamples: num(live.recordedSamples, 0),
      peaks: numbers(live.peaks),
    };
  }
}

export function getLayerPeaks(id: string): LooperPeaks | undefined {
  return layerPeaks.get(id);
}

export function getLivePeaks(): LooperLivePeaks | null {
  return livePeaks;
}

export function isLooperStripOpen(): boolean {
  return stripOpen;
}

export function setLooperStripOpen(open: boolean): void {
  stripOpen = open;
}

export function isLooperLayersOpen(): boolean {
  return layersOpen;
}

export function setLooperLayersOpen(open: boolean): void {
  layersOpen = open;
}

/** What the one state button does next, and what it says. */
export function mainButtonLabel(state: LooperStateName): string {
  switch (state) {
    case "empty":
      return "Rec";
    case "countIn":
    case "armed":
      return "Rec now";
    case "recording":
      return "Play";
    case "playing":
      return "Dub";
    case "stopped":
      return "Play";
    case "overdubArmed":
      return "Dub now";
    case "overdubbing":
      return "Play";
  }
}
