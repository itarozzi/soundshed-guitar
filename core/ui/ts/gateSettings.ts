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
import { GenericKnob } from "./knob.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import { escapeHtml } from "./utils.js";
import type { GraphNode } from "./types.js";

/** Threshold's ends before the effect catalogue arrives with the real ones. */
const FALLBACK_THRESHOLD_RANGE = { min: -80, max: 0, step: 1 };

/** Pixels of drag for a full sweep, whatever the parameter's span. */
const KNOB_SWEEP_PIXELS = 200;

const flyoutKnobs = new Map<string, GenericKnob>();
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

/**
 * A two-label 0..1 parameter is a switch however the engine spells it — the gate's
 * Stereo Link is `{0,1}` with `{"Independent","Linked"}` rather than unit "toggle".
 */
function isSwitchParam(def: ParameterDef): boolean {
  return def.unit === "toggle" || (def.labels?.length === 2 && def.min === 0 && def.max === 1);
}

/**
 * Decimals from the span, not the unit: 0.1–50 ms wants one place where 0–500 ms
 * reads better with none, and both are "ms".
 */
function valueDecimals(def: ParameterDef): number {
  const span = Math.abs(def.max - def.min);
  if (span <= 2) return 2;
  if (span <= 60) return 1;
  return 0;
}

function knobStep(def: ParameterDef): number {
  return def.step && def.step > 0 ? def.step : Math.pow(10, -valueDecimals(def));
}

function formatGateValue(value: number, def: ParameterDef): string {
  if (isSwitchParam(def)) {
    return def.labels?.[value >= 0.5 ? 1 : 0] ?? (value >= 0.5 ? "On" : "Off");
  }
  if (def.labels?.length) {
    const index = Math.round(value);
    return def.labels[index] ?? `${index}`;
  }
  const text = value.toFixed(valueDecimals(def));
  return def.unit ? `${text} ${def.unit}` : text;
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

function buildParamRow(def: ParameterDef): string {
  const label = escapeHtml(def.name || def.key);
  const value = gateParamValue(def);
  const display = escapeHtml(formatGateValue(value, def));
  const key = escapeHtml(def.key);

  if (isSwitchParam(def)) {
    return `
      <div class="gate-param-row is-switch">
        <span class="gate-param-label">${label}</span>
        <label class="toggle-switch">
          <input type="checkbox" class="gate-param-toggle" data-param-key="${key}" ${value >= 0.5 ? "checked" : ""} />
          <span class="toggle-slider"></span>
        </label>
        <span class="gate-param-value" data-param-key="${key}">${display}</span>
      </div>`;
  }

  return `
    <div class="gate-param-row">
      <span class="gate-param-label">${label}</span>
      <div class="knob gate-param-knob" data-param-key="${key}" data-value="${value}">
        <div class="knob-indicator"></div>
      </div>
      <span class="gate-param-value" data-param-key="${key}">${display}</span>
    </div>`;
}

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
    <div class="gate-settings-header">
      <span class="gate-settings-title">Noise Gate</span>
      <label class="toggle-switch gate-settings-enable">
        <input type="checkbox" class="gate-settings-enable-input" ${enabled ? "checked" : ""} />
        <span class="toggle-slider"></span>
      </label>
    </div>
    <div class="gate-settings-params">${main.map(buildParamRow).join("")}</div>
    ${advanced.length === 0 ? "" : `
      <div class="gate-settings-section-title">Advanced</div>
      <div class="gate-settings-params">${advanced.map(buildParamRow).join("")}</div>`}
    <div class="gate-settings-footer">
      <button type="button" class="gate-settings-reset">Reset to defaults</button>
    </div>`;

  bindFlyoutControls(flyout, defs);
  flyoutBuilt = true;
  return true;
}

function bindFlyoutControls(flyout: HTMLElement, defs: ParameterDef[]): void {
  flyoutKnobs.clear();

  defs.forEach((def) => {
    const valueEl = flyout.querySelector<HTMLElement>(`.gate-param-value[data-param-key="${def.key}"]`);

    if (isSwitchParam(def)) {
      const input = flyout.querySelector<HTMLInputElement>(`.gate-param-toggle[data-param-key="${def.key}"]`);
      input?.addEventListener("change", () => {
        const value = input.checked ? 1 : 0;
        applyGateParam(def.key, value, true);
        if (valueEl) {
          valueEl.textContent = formatGateValue(value, def);
        }
      });
      return;
    }

    const knobEl = flyout.querySelector<HTMLElement>(`.gate-param-knob[data-param-key="${def.key}"]`);
    if (!knobEl) {
      return;
    }

    const span = Math.abs(def.max - def.min);
    flyoutKnobs.set(def.key, new GenericKnob({
      knobElement: knobEl,
      paramId: `gate_${def.key}`,
      minValue: def.min,
      maxValue: def.max,
      defaultValue: def.default,
      stepValue: knobStep(def),
      sensitivity: span > 0 ? span / KNOB_SWEEP_PIXELS : 0.5,
      displayFormat: (value) => formatGateValue(value, def),
      valueDisplay: valueEl,
      sendParameter: false,
      onValueChange: (value) => {
        applyGateParam(def.key, value, false);
        if (def.key === "threshold") {
          thresholdKnob?.setValue(value);
        }
      },
      onValueCommit: (value) => applyGateParam(def.key, value, true),
    }));
  });

  flyout.querySelector<HTMLInputElement>(".gate-settings-enable-input")
    ?.addEventListener("change", (event) => {
      applyGateEnabled((event.target as HTMLInputElement).checked);
    });

  flyout.querySelector<HTMLButtonElement>(".gate-settings-reset")
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

  gateParamDefs().forEach((def) => {
    const value = gateParamValue(def);
    const knob = flyoutKnobs.get(def.key);
    if (knob) {
      knob.setValue(value);
      return;
    }

    const input = flyout.querySelector<HTMLInputElement>(`.gate-param-toggle[data-param-key="${def.key}"]`);
    if (input) {
      input.checked = value >= 0.5;
    }
    const valueEl = flyout.querySelector<HTMLElement>(`.gate-param-value[data-param-key="${def.key}"]`);
    if (valueEl) {
      valueEl.textContent = formatGateValue(value, def);
    }
  });
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
