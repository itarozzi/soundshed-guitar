/**
 * How loud a preset plays the demo DI: asking the engine to measure it (measurePresetLevel,
 * core/src/controller/PresetLevelMeter.h) and waiting for the answer.
 *
 * The engine renders the preset offline on a graph of its own, never the one playing, so asking
 * does not interrupt the player. It answers with BS.1770 integrated loudness, sample peak and RMS,
 * as a lone rig is heard in the mix (the equal-power pan law puts a centred rig 3 dB down; the
 * answer's slotGainDb says so). The tone builder calls this to trim what it builds to -18 LUFS;
 * nothing in the UI shows it.
 *
 * Several requests may be out at once: each has its own requestId, answers are matched to it,
 * and one that does not come back in time rejects. The engine renders one at a time and turns
 * away more than a few waiting, so a caller measuring many presets should await each in turn.
 */

import { sendMeasurePresetLevel } from "./bridge.js";

/** Long enough for the whole 110 s demo DI through a heavy chain on a slow machine. */
export const PRESET_LEVEL_TIMEOUT_MS = 180000;

export interface PresetLevel {
  requestId: string;
  presetId: string;
  /** The scene measured: the one asked for, or the preset's first. */
  sceneId: string;
  clipId: string;
  /** Null when the output is silent or too quiet or short to gate. */
  integratedLufs: number | null;
  /** Sample peak over the whole render, tail included. Null for silence. */
  peakDbfs: number | null;
  rmsDbfs: number | null;
  /** How much of the clip was measured. */
  durationSeconds: number;
  sampleRate: number;
  latencySamples: number;
  /** What the mix does to a lone, centred rig (about -3 dB), already in the levels above. */
  slotGainDb: number;
  /** How long the render ran past the clip for the tail. */
  tailSeconds: number;
}

/** A resource the preset names that has no file, so it was not measured. */
export interface MissingPresetResource {
  nodeId: string;
  nodeType: string;
  resourceType: string;
  resourceId: string;
  name: string;
  filePath: string;
}

export class PresetLevelError extends Error {
  readonly requestId: string;
  readonly missingResources: MissingPresetResource[];

  constructor(message: string, requestId: string, missingResources: MissingPresetResource[] = []) {
    super(message);
    this.name = "PresetLevelError";
    this.requestId = requestId;
    this.missingResources = missingResources;
  }
}

export interface PresetLevelOptions {
  /** A scene to measure; the preset's first when left out. */
  sceneId?: string;
  /** A demo clip id from ui/demo/clips.json; the demo DI when left out. */
  clipId?: string;
  /** Measure only the start of the clip. */
  maxSeconds?: number;
  timeoutMs?: number;
}

/** A preset by id (stored, factory, or the active one unsaved), or a whole preset as saved. */
export type PresetLevelSubject = string | { id?: string } & Record<string, unknown>;

interface Pending {
  resolve: (level: PresetLevel) => void;
  reject: (error: PresetLevelError) => void;
  timer: ReturnType<typeof setTimeout>;
}

const pending = new Map<string, Pending>();
let requestCounter = 0;

/** Measures `preset` (see PresetLevelSubject) over a demo clip. Rejects with a PresetLevelError
 * when the engine cannot (an unknown preset, scene or clip, or a resource with no file, listed in
 * missingResources) or does not answer within the timeout. */
export function requestPresetLevel(preset: PresetLevelSubject, options: PresetLevelOptions = {}): Promise<PresetLevel> {
  requestCounter += 1;
  const requestId = `preset-level-${Date.now().toString(36)}-${requestCounter}`;
  const request: Record<string, unknown> = typeof preset === "string" ? { presetId: preset } : { preset };
  if (typeof preset !== "string" && typeof preset.id === "string") {
    request.presetId = preset.id;
  }
  if (options.sceneId) {
    request.sceneId = options.sceneId;
  }
  if (options.clipId) {
    request.clipId = options.clipId;
  }
  if (typeof options.maxSeconds === "number" && options.maxSeconds > 0) {
    request.maxSeconds = options.maxSeconds;
  }

  return new Promise<PresetLevel>((resolve, reject) => {
    const timer = setTimeout(() => {
      if (pending.delete(requestId)) {
        reject(new PresetLevelError("The engine did not answer the level measurement in time", requestId));
      }
    }, options.timeoutMs ?? PRESET_LEVEL_TIMEOUT_MS);
    pending.set(requestId, { resolve, reject, timer });
    sendMeasurePresetLevel(requestId, request);
  });
}

function finiteOrNull(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function finiteOr(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function text(value: unknown): string {
  return typeof value === "string" ? value : "";
}

/** Reads a "presetLevelMeasured" answer. */
export function parsePresetLevel(payload: Record<string, unknown>): PresetLevel {
  return {
    requestId: text(payload.requestId),
    presetId: text(payload.presetId),
    sceneId: text(payload.sceneId),
    clipId: text(payload.clipId),
    integratedLufs: finiteOrNull(payload.integratedLufs),
    peakDbfs: finiteOrNull(payload.peakDbfs),
    rmsDbfs: finiteOrNull(payload.rmsDbfs),
    durationSeconds: finiteOr(payload.durationSeconds, 0),
    sampleRate: finiteOr(payload.sampleRate, 0),
    latencySamples: finiteOr(payload.latencySamples, 0),
    slotGainDb: finiteOr(payload.slotGainDb, 0),
    tailSeconds: finiteOr(payload.tailSeconds, 0),
  };
}

function settle(payload: Record<string, unknown>): Pending | null {
  const requestId = text(payload.requestId);
  const entry = pending.get(requestId);
  if (!entry) {
    return null; // timed out already, or not ours
  }
  pending.delete(requestId);
  clearTimeout(entry.timer);
  return entry;
}

/** Handles "presetLevelMeasured": resolves the request it answers, if still waiting. */
export function applyPresetLevelMeasured(payload: Record<string, unknown>): void {
  settle(payload)?.resolve(parsePresetLevel(payload));
}

/** Handles "presetLevelMeasureFailed": rejects the request it answers, if still waiting. */
export function applyPresetLevelMeasureFailed(payload: Record<string, unknown>): void {
  const entry = settle(payload);
  if (!entry) {
    return;
  }
  const missing = Array.isArray(payload.missingResources)
    ? payload.missingResources
        .filter((item): item is Record<string, unknown> => typeof item === "object" && item !== null)
        .map((item) => ({
          nodeId: text(item.nodeId),
          nodeType: text(item.nodeType),
          resourceType: text(item.resourceType),
          resourceId: text(item.resourceId),
          name: text(item.name),
          filePath: text(item.filePath),
        }))
    : [];
  entry.reject(new PresetLevelError(text(payload.message) || "The level measurement failed", text(payload.requestId), missing));
}
