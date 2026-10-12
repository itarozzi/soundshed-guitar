/**
 * The global noise gate — the control-bar strip and the settings flyout behind
 * the chevron next to its dB readout.
 *
 * The strip carries the two controls worth a permanent place (on/off and
 * threshold). Everything else the gate has lives in the flyout, which is built
 * from the engine's own parameter definitions rather than a second list here:
 * a parameter added to NoiseGateEffect arrives in the effect catalogue with its
 * range, unit and Advanced flag, and appears in the flyout with no UI change.
 * That is also why the flyout is built on first open — the catalogue has landed
 * by the time anyone clicks, and a panel built at load would be empty.
 *
 * Values live on the global chain's `global_gate` node. A change is sent as a
 * `gate.<key>` global-chain param *and* written onto the node, because the node
 * is the copy the settings blob is serialized from; sending alone would lose an
 * edit made just before the state is saved.
 */

import { appendLog } from "./logging.js";
import { sendGlobalChainParam } from "./bridge.js";
import { uiState } from "./state.js";
import { EffectGuids } from "./effectGuids.js";
import {
  bindFlyoutParamRows,
  buildFlyoutParamRows,
  type FlyoutParamBinding,
  KNOB_SWEEP_PIXELS,
  knobStep,
  refreshFlyoutParamRows,
} from "./flyoutParamControls.js";
import { GenericKnob } from "./knob.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import type { GraphNode } from "./types.js";

/** Threshold's ends before the effect catalogue arrives with the real ones. */
const FALLBACK_THRESHOLD_RANGE = { min: -80, max: 0, step: 1 };

let flyoutKnobs = new Map<string, GenericKnob>();
let thresholdKnob: GenericKnob | null = null;
let thresholdRangeFromCatalog = false;
let flyoutBuilt = false;
let controlsInitialized = false;

const byId = <T extends HTMLElement>(id: string): T | null => document.getElementById(id) as T | null;

const gateToggleEl = (): HTMLInputElement | null => byId<HTMLInputElement>("gate-toggle");
const flyoutEl = (): HTMLDivElement | null => byId<HTMLDivElement>("gate-settings-flyout");
const flyoutTriggerEl = (): HTMLButtonElement | null => byId<HTMLButtonElement>("gate-settings-trigger");

function getGateNode(): GraphNode | undefined {
  const graph = uiState.globalSignalChain?.preChainGraph;
  if (!graph) {
    return undefined;
  }
  return graph.nodes.find((node) => node.id === "global_gate")
    ?? graph.nodes.find((node) => node.type === EffectGuids.kDynamicsGate);
}

function gateParamDefs(): ParameterDef[] {
  return EffectTypeRegistry.get(EffectGuids.kDynamicsGate)?.parameters ?? [];
}

function gateParamDef(key: string): ParameterDef | undefined {
  return gateParamDefs().find((def) => def.key === key);
}

/** The node's value for a parameter, or the definition's default when it has none. */
function gateParamValue(def: ParameterDef): number {
  const stored = getGateNode()?.params[def.key];
  return typeof stored === "number" && Number.isFinite(stored) ? stored : def.default;
}

// ── Sending ──────────────────────────────────────────────────────────────────

/**
 * Sends one gate parameter and records it on the node the settings blob is saved
 * from. Sent on every move rather than only on release: the global chain has
 * never coalesced these, and a gate you can only hear once you let go of the knob
 * is not a gate you can set by ear.
 */
function applyGateParam(key: string, value: number, commit: boolean): void {
  const node = getGateNode();
  if (node) {
    node.params[key] = value;
  }
  sendGlobalChainParam(`gate.${key}`, value);
  if (commit) {
    appendLog(`preChainGraph.global_gate.${key} → ${value}`);
  }
}

function applyGateEnabled(enabled: boolean): void {
  sendGlobalChainParam("gate.enabled", enabled);
  const node = getGateNode();
  if (node) {
    node.bypassed = !enabled;
  }
  appendLog(`preChainGraph.global_gate.enabled → ${enabled}`);
  reflectGateEnabled(enabled);
}

/** Mirrors the on/off state onto both views of it and the dimmed threshold knob. */
function reflectGateEnabled(enabled: boolean): void {
  const toggle = gateToggleEl();
  if (toggle) {
    toggle.checked = enabled;
  }
  const flyoutToggle = flyoutEl()?.querySelector<HTMLInputElement>(".gate-settings-enable-input");
  if (flyoutToggle) {
    flyoutToggle.checked = enabled;
  }
  byId("gate-threshold-control")?.classList.toggle("disabled", !enabled);
}

// ── Flyout ───────────────────────────────────────────────────────────────────

/** Where the flyout's rows read values from and send changes to (flyoutParamControls.ts). */
const gateBinding: FlyoutParamBinding = {
  idPrefix: "gate",
  value: (def) => gateParamValue(def),
  apply: (key, value, commit) => applyGateParam(key, value, commit),
};

/**
 * Builds the flyout from the catalogue. Returns false when the catalogue has not
 * arrived, so the next open tries again rather than caching an empty panel.
 */
function buildFlyout(): boolean {
  const flyout = flyoutEl();
  const defs = gateParamDefs();
  if (!flyout || defs.length === 0) {
    return false;
  }

  const main = defs.filter((def) => !def.advanced);
  const advanced = defs.filter((def) => Boolean(def.advanced));
  const enabled = !(getGateNode()?.bypassed ?? true);

  flyout.innerHTML = `
    <div class="flyout-settings-header">
      <span class="flyout-settings-title">Noise Gate</span>
      <label class="toggle-switch gate-settings-enable">
        <input type="checkbox" class="gate-settings-enable-input" ${enabled ? "checked" : ""} />
        <span class="toggle-slider"></span>
      </label>
    </div>
    <div class="flyout-settings-params">${buildFlyoutParamRows(main, gateBinding)}</div>
    ${advanced.length === 0 ? "" : `
      <div class="flyout-settings-section-title">Advanced</div>
      <div class="flyout-settings-params">${buildFlyoutParamRows(advanced, gateBinding)}</div>`}
    <div class="flyout-settings-footer">
      <button type="button" class="flyout-settings-reset">Reset to defaults</button>
    </div>`;

  bindFlyoutControls(flyout, defs);
  flyoutBuilt = true;
  return true;
}

function bindFlyoutControls(flyout: HTMLElement, defs: ParameterDef[]): void {
  flyoutKnobs = bindFlyoutParamRows(flyout, defs, gateBinding, (key, value) => {
    if (key === "threshold") {
      thresholdKnob?.setValue(value);
    }
  });

  flyout.querySelector<HTMLInputElement>(".gate-settings-enable-input")
    ?.addEventListener("change", (event) => {
      applyGateEnabled((event.target as HTMLInputElement).checked);
    });

  flyout.querySelector<HTMLButtonElement>(".flyout-settings-reset")
    ?.addEventListener("click", () => resetGateParams());
}

function resetGateParams(): void {
  gateParamDefs().forEach((def) => applyGateParam(def.key, def.default, true));
  refreshFlyoutValues();
  thresholdKnob?.setValue(gateParamDef("threshold")?.default ?? FALLBACK_THRESHOLD_RANGE.min);
}

/** Pushes the node's current values back into the built controls. */
function refreshFlyoutValues(): void {
  const flyout = flyoutEl();
  if (!flyout || !flyoutBuilt) {
    return;
  }
  refreshFlyoutParamRows(flyout, gateParamDefs(), flyoutKnobs, gateBinding);
}

function isFlyoutOpen(): boolean {
  return Boolean(flyoutEl() && !flyoutEl()!.hidden);
}

/**
 * Centres the flyout on its trigger, then pulls it back inside the window. Laid
 * out against its offset parent, the same way the input-calibration menu is.
 */
function positionFlyout(): void {
  const flyout = flyoutEl();
  const trigger = flyoutTriggerEl();
  if (!flyout || !trigger) {
    return;
  }

  const offsetParent = flyout.offsetParent;
  if (!(offsetParent instanceof HTMLElement)) {
    return;
  }

  const viewportMargin = 8;
  const parentRect = offsetParent.getBoundingClientRect();
  const triggerRect = trigger.getBoundingClientRect();
  const width = flyout.offsetWidth;

  const centeredLeft = triggerRect.left + (triggerRect.width / 2) - parentRect.left - (width / 2);
  const minLeft = viewportMargin - parentRect.left;
  const maxLeft = window.innerWidth - viewportMargin - parentRect.left - width;
  flyout.style.left = `${Math.round(Math.min(Math.max(centeredLeft, minLeft), Math.max(minLeft, maxLeft)))}px`;
}

export function closeGateSettingsFlyout(): void {
  const flyout = flyoutEl();
  if (flyout) {
    flyout.hidden = true;
  }
  const trigger = flyoutTriggerEl();
  trigger?.setAttribute("aria-expanded", "false");
  trigger?.closest(".control-bar")?.classList.remove("has-open-gate-settings");
}

function openGateSettingsFlyout(): void {
  const flyout = flyoutEl();
  if (!flyout) {
    return;
  }

  if (!flyoutBuilt && !buildFlyout()) {
    return;
  }

  refreshFlyoutValues();
  reflectGateEnabled(!(getGateNode()?.bypassed ?? true));
  flyout.hidden = false;
  positionFlyout();

  const trigger = flyoutTriggerEl();
  trigger?.setAttribute("aria-expanded", "true");
  trigger?.closest(".control-bar")?.classList.add("has-open-gate-settings");
}

function toggleGateSettingsFlyout(): void {
  if (isFlyoutOpen()) {
    closeGateSettingsFlyout();
  } else {
    openGateSettingsFlyout();
  }
}

// ── Control bar ──────────────────────────────────────────────────────────────

export function initializeGateControls(): void {
  if (controlsInitialized) {
    return;
  }
  controlsInitialized = true;

  const gateToggle = gateToggleEl();
  gateToggle?.addEventListener("change", () => applyGateEnabled(gateToggle.checked));

  const thresholdKnobEl = document.querySelector<HTMLElement>('.knob[data-param="gate_threshold"]');
  if (thresholdKnobEl) {
    const def = gateParamDef("threshold");
    const range = def ?? FALLBACK_THRESHOLD_RANGE;
    thresholdRangeFromCatalog = Boolean(def);
    thresholdKnob = new GenericKnob({
      knobElement: thresholdKnobEl,
      paramId: "gate_threshold",
      minValue: range.min,
      maxValue: range.max,
      defaultValue: def?.default ?? -60,
      stepValue: def ? knobStep(def) : FALLBACK_THRESHOLD_RANGE.step,
      sensitivity: Math.abs(range.max - range.min) / KNOB_SWEEP_PIXELS,
      displayFormat: (value) => `${value.toFixed(0)} dB`,
      valueDisplayId: "gate-threshold-value",
      sendParameter: false,
      onValueChange: (value) => {
        applyGateParam("threshold", value, false);
        flyoutKnobs.get("threshold")?.setValue(value);
      },
      onValueCommit: (value) => applyGateParam("threshold", value, true),
    });
  }

  flyoutTriggerEl()?.addEventListener("click", toggleGateSettingsFlyout);

  document.addEventListener("mousedown", (event) => {
    if (!isFlyoutOpen() || !(event.target instanceof Node)) {
      return;
    }
    if (flyoutEl()?.contains(event.target) || flyoutTriggerEl()?.contains(event.target)) {
      return;
    }
    closeGateSettingsFlyout();
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && isFlyoutOpen()) {
      closeGateSettingsFlyout();
      event.stopPropagation();
    }
  });

  window.addEventListener("resize", () => {
    if (isFlyoutOpen()) {
      positionFlyout();
    }
  });

  reflectGateEnabled(gateToggle?.checked ?? true);
}

export function syncGateControlsFromState(): void {
  const gateNode = getGateNode();

  if (gateNode) {
    reflectGateEnabled(!gateNode.bypassed);
  } else {
    const legacyEnabled = uiState.parameters.values?.find((param) => param.id === "gate_enabled")?.value;
    if (typeof legacyEnabled === "number") {
      reflectGateEnabled(legacyEnabled > 0.5);
    }
  }

  // The catalogue lands after the first render, so the threshold knob starts on
  // the fallback ends and takes the engine's the first time they are known.
  const thresholdDef = gateParamDef("threshold");
  if (thresholdKnob && thresholdDef && !thresholdRangeFromCatalog) {
    thresholdRangeFromCatalog = true;
    thresholdKnob.setRange(thresholdDef.min, thresholdDef.max, knobStep(thresholdDef));
  }

  const threshold = gateNode?.params.threshold
    ?? uiState.parameters.values?.find((param) => param.id === "gate_threshold")?.value;
  if (thresholdKnob && typeof threshold === "number") {
    thresholdKnob.setValue(threshold);
  }

  refreshFlyoutValues();
}
