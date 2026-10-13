/**
 * The noise gate's Learn, shared by the global gate's flyout and a gate node's panel.
 *
 * Learn listens to the gate's own detector for about two seconds with the strings muted and
 * suggests a threshold just above the noise it heard (GateLearnService, GateNoiseLearner). The
 * engine only measures: the suggestion comes back here and is applied by whichever view owns
 * the gate, the same way a turned knob would be, so it reaches the preset, undo and the host.
 *
 * Both views draw the same meter: the noise the gate heard as a shaded run from the left, the
 * band between the close level and Threshold (the hysteresis), Threshold itself, and while
 * listening the live level and a progress line. A view is markup from `gateLearnMeterHtml`
 * bound with `bindGateLearnView`; status messages update every bound view of that gate.
 */

import { postMessage } from "./bridge.js";
import type { IncomingPayload } from "./messages/types.js";
import { escapeHtml } from "./utils.js";

export type GateLearnScope = "global" | "preset";

export interface GateLearnTarget {
  scope: GateLearnScope;
  nodeId: string;
  /** The mixer id of the preset, for a node in one. */
  presetId?: string;
}

export type GateLearnState = "idle" | "listening" | "done" | "failed" | "cancelled";

export interface GateLearnStatus {
  state: GateLearnState;
  /** 0..1 through the listen. */
  progress: number;
  /** The gate's detector level while listening (dBFS). */
  levelDb?: number;
  /** The noise heard, once a listen finishes (dBFS). */
  noiseFloorDb?: number;
  /** The threshold the listen suggests. */
  thresholdDb?: number;
  /** Why a listen failed: "playing", "noSignal" or "notFound". */
  reason?: string;
}

/** What a bound view needs to draw the gate's current settings and apply a result. */
export interface GateLearnViewBinding {
  target: GateLearnTarget;
  threshold: () => number;
  hysteresis: () => number;
  /** The ends of Threshold's range, which the meter spans. */
  range: () => { min: number; max: number };
  /** Before a listen starts: a bypassed gate hears nothing, so the view switches it on. */
  ensureEnabled: () => void;
}

/** Applies a finished listen's threshold to the gate it measured. */
export type GateLearnApplier = (target: GateLearnTarget, thresholdDb: number) => void;

const statuses = new Map<string, GateLearnStatus>();
const views = new Map<string, { root: HTMLElement; binding: GateLearnViewBinding }[]>();
const appliers = new Map<GateLearnScope, GateLearnApplier>();

export function gateLearnKey(target: GateLearnTarget): string {
  return target.scope === "global" ? "global" : `preset:${target.presetId ?? ""}:${target.nodeId}`;
}

/** Registers how a scope's gate takes a learned threshold. One per scope; the last wins. */
export function registerGateLearnApplier(scope: GateLearnScope, applier: GateLearnApplier): void {
  appliers.set(scope, applier);
}

export function gateLearnStatus(target: GateLearnTarget): GateLearnStatus {
  return statuses.get(gateLearnKey(target)) ?? { state: "idle", progress: 0 };
}

export function startGateLearn(target: GateLearnTarget): void {
  statuses.set(gateLearnKey(target), { ...gateLearnStatus(target), state: "listening", progress: 0, levelDb: undefined });
  refreshGateLearnViews(target);
  postMessage({ type: "gateLearn", action: "start", ...target });
}

export function cancelGateLearn(): void {
  postMessage({ type: "gateLearn", action: "cancel" });
}

function finiteOrUndefined(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

/** "gateLearnStatus" from the engine. */
export function onGateLearnStatus(payload: IncomingPayload): void {
  const scope = payload.scope === "preset" ? "preset" : "global";
  const target: GateLearnTarget = {
    scope,
    nodeId: typeof payload.nodeId === "string" ? payload.nodeId : "",
    ...(typeof payload.presetId === "string" && scope === "preset" ? { presetId: payload.presetId } : {}),
  };
  const state = (["listening", "done", "failed", "cancelled"] as const)
    .find((candidate) => candidate === payload.state) ?? "idle";
  const previous = gateLearnStatus(target);
  const status: GateLearnStatus = {
    state,
    progress: finiteOrUndefined(payload.progress) ?? (state === "done" ? 1 : previous.progress),
    levelDb: finiteOrUndefined(payload.levelDb),
    // A cancelled or failed listen keeps showing the last noise floor that was learned.
    noiseFloorDb: finiteOrUndefined(payload.noiseFloorDb) ?? previous.noiseFloorDb,
    thresholdDb: finiteOrUndefined(payload.thresholdDb),
    reason: typeof payload.reason === "string" ? payload.reason : undefined,
  };
  if (state === "failed" && status.reason === "playing") {
    // What it heard was playing, not noise: do not leave it drawn as the noise floor.
    status.noiseFloorDb = previous.noiseFloorDb;
  }
  statuses.set(gateLearnKey(target), status);

  if (state === "done" && status.thresholdDb !== undefined) {
    appliers.get(scope)?.(target, status.thresholdDb);
  }
  refreshGateLearnViews(target);
}

// ── The view ─────────────────────────────────────────────────────────────────

/** The meter, status line and button. Bind it with bindGateLearnView once it is in the DOM. */
export function gateLearnMeterHtml(target: GateLearnTarget): string {
  return `
    <div class="gate-learn" data-gate-learn-key="${escapeHtml(gateLearnKey(target))}">
      <div class="gate-learn-meter" role="img" aria-label="Noise floor and threshold">
        <div class="gate-learn-noise"></div>
        <div class="gate-learn-hysteresis"></div>
        <div class="gate-learn-threshold"></div>
        <div class="gate-learn-level"></div>
        <div class="gate-learn-progress"></div>
      </div>
      <div class="gate-learn-scale" aria-hidden="true"><span class="gate-learn-scale-min"></span><span class="gate-learn-scale-max"></span></div>
      <div class="gate-learn-row">
        <button type="button" class="gate-learn-button" title="Mute the strings, then listen for two seconds and set Threshold just above the noise">Learn</button>
        <span class="gate-learn-status" aria-live="polite"></span>
      </div>
    </div>`;
}

/** The bound view of the gate inside `container`, if there is one. */
export function findGateLearnView(container: ParentNode, target: GateLearnTarget): HTMLElement | null {
  const key = gateLearnKey(target);
  return Array.from(container.querySelectorAll<HTMLElement>(".gate-learn"))
    .find((element) => element.dataset.gateLearnKey === key) ?? null;
}

export function bindGateLearnView(container: ParentNode, binding: GateLearnViewBinding): void {
  const root = findGateLearnView(container, binding.target);
  if (!root) {
    return;
  }

  const key = gateLearnKey(binding.target);
  const bound = (views.get(key) ?? []).filter((view) => view.root.isConnected && view.root !== root);
  bound.push({ root, binding });
  views.set(key, bound);

  root.querySelector<HTMLButtonElement>(".gate-learn-button")?.addEventListener("click", () => {
    if (gateLearnStatus(binding.target).state === "listening") {
      cancelGateLearn();
      return;
    }
    binding.ensureEnabled();
    startGateLearn(binding.target);
  });

  renderGateLearnView(root, binding);
}

/** Redraws every bound view of the gate: after a status, or after its settings change. */
export function refreshGateLearnViews(target: GateLearnTarget): void {
  const key = gateLearnKey(target);
  const live = (views.get(key) ?? []).filter((view) => view.root.isConnected);
  views.set(key, live);
  live.forEach((view) => renderGateLearnView(view.root, view.binding));
}

function formatDb(value: number): string {
  return `${Math.round(value)} dB`.replace("-", "−");
}

/** `floorDb` is the bottom of the meter: quieter noise (or silence) reads as under it. */
function statusText(status: GateLearnStatus, floorDb: number): string {
  switch (status.state) {
    case "listening":
      return "Listening… keep the strings quiet";
    case "done": {
      if (status.noiseFloorDb === undefined || status.thresholdDb === undefined) {
        return "Threshold set";
      }
      const noise = status.noiseFloorDb < floorDb ? `under ${formatDb(floorDb)}` : formatDb(status.noiseFloorDb);
      return `Noise ${noise} · threshold set to ${formatDb(status.thresholdDb)}`;
    }
    case "failed":
      if (status.reason === "playing") {
        return "Heard playing. Mute the strings and try again.";
      }
      if (status.reason === "noSignal") {
        return "No audio reached the gate. Is the audio device running?";
      }
      return "This gate is not in the running chain.";
    case "cancelled":
      return "Learn cancelled.";
    default:
      return "Mute the strings, then Learn.";
  }
}

function renderGateLearnView(root: HTMLElement, binding: GateLearnViewBinding): void {
  const status = gateLearnStatus(binding.target);
  const { min, max } = binding.range();
  const span = Math.max(1, max - min);
  const percent = (db: number) => `${(Math.min(Math.max((db - min) / span, 0), 1) * 100).toFixed(2)}%`;
  const threshold = binding.threshold();
  const closeLevel = threshold - Math.max(0, binding.hysteresis());
  const listening = status.state === "listening";

  root.classList.toggle("is-listening", listening);
  root.classList.toggle("has-noise-floor", status.noiseFloorDb !== undefined);
  root.classList.toggle("is-failed", status.state === "failed");

  const meter = root.querySelector<HTMLElement>(".gate-learn-meter");
  if (meter) {
    meter.style.setProperty("--gate-learn-threshold", percent(threshold));
    meter.style.setProperty("--gate-learn-close", percent(closeLevel));
    meter.style.setProperty("--gate-learn-noise", status.noiseFloorDb !== undefined ? percent(status.noiseFloorDb) : "0%");
    meter.style.setProperty("--gate-learn-level", status.levelDb !== undefined ? percent(status.levelDb) : "0%");
    meter.style.setProperty("--gate-learn-progress", `${(status.progress * 100).toFixed(1)}%`);
    const noise = status.noiseFloorDb !== undefined ? `, noise at ${formatDb(status.noiseFloorDb)}` : "";
    meter.setAttribute("aria-label", `Threshold ${formatDb(threshold)}${noise}`);
  }

  const scaleMin = root.querySelector(".gate-learn-scale-min");
  const scaleMax = root.querySelector(".gate-learn-scale-max");
  if (scaleMin) scaleMin.textContent = formatDb(min);
  if (scaleMax) scaleMax.textContent = formatDb(max);

  const button = root.querySelector<HTMLButtonElement>(".gate-learn-button");
  if (button) {
    button.textContent = listening ? "Cancel" : "Learn";
    button.classList.toggle("is-active", listening);
  }

  const text = root.querySelector(".gate-learn-status");
  if (text) {
    text.textContent = statusText(status, min);
  }
}
