/**
 * The global chain's Pickup Match: the "global_pickup" node straight after the input, bypassed
 * until switched on. It converts the player's pickup ahead of everything else, whatever preset
 * plays. This module draws its tab in the input calibration flyout (settings/inputCalibration.ts
 * hosts it) and the control bar's indicator, which shows while it is on.
 *
 * The tab is built from the same pieces as Pickup Match's params panel, so the two cannot drift:
 * - flyout rows for its knobs (flyoutParamControls.ts, shared with the gate flyout);
 * - the pickup pickers (enumPicker.ts);
 * - the conversion curve and the custom-design blocks (pickupResponse.ts).
 *
 * Values live on the node. A change is sent as a `pickup.<param>` global-chain param and
 * written onto the node, the copy the settings blob is saved from, as the gate does.
 */

import { sendGlobalChainParam } from "./bridge.js";
import { EffectGuids } from "./effectGuids.js";
import {
  bindFlyoutParamRows,
  buildFlyoutParamRows,
  type FlyoutParamBinding,
  refreshFlyoutParamRows,
} from "./flyoutParamControls.js";
import type { GenericKnob } from "./knob.js";
import { appendLog } from "./logging.js";
import { pickerIcon } from "./pickerIcons.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import {
  bindEnumPickerTriggers,
  isPickerParam,
  pickerChoiceIndex,
  pickerChoices,
} from "./signalPath/paramsPanel/enumPicker.js";
import {
  buildPickupDesignSectionHtml,
  buildPickupResponseSectionHtml,
  PICKUP_DESIGN_PARAM_KEYS,
  updatePickupResponseVisualization,
} from "./signalPath/paramsPanel/pickupResponse.js";
import { uiState } from "./state.js";
import type { GraphNode } from "./types.js";

export const GLOBAL_PICKUP_NODE_ID = "global_pickup";

/** The tab's container while it is drawn, so a change can redraw its curve and design blocks. */
let tabRoot: HTMLElement | null = null;
let tabKnobs = new Map<string, GenericKnob>();
/** What the drawn tab's layout depends on; anything else only moves values. */
let drawnLayout = "";

function layoutKey(): string {
  const node = getGlobalPickupNode();
  const params = node?.params ?? {};
  return [isGlobalPickupMatchOn(), params.source, params.target, params.sourceDesignType, params.targetDesignType,
    pickupParamDefs().length].join("|");
}

export function getGlobalPickupNode(): GraphNode | undefined {
  const graph = uiState.globalSignalChain?.preChainGraph;
  return graph?.nodes.find((node) => node.id === GLOBAL_PICKUP_NODE_ID)
    ?? graph?.nodes.find((node) => node.type === EffectGuids.kPickupMatch);
}

function pickupParamDefs(): ParameterDef[] {
  return EffectTypeRegistry.get(EffectGuids.kPickupMatch)?.parameters ?? [];
}

function paramValue(def: ParameterDef): number {
  const stored = getGlobalPickupNode()?.params[def.key];
  return typeof stored === "number" && Number.isFinite(stored) ? stored : def.default;
}

export function isGlobalPickupMatchOn(): boolean {
  const node = getGlobalPickupNode();
  return Boolean(node && !node.bypassed);
}

/** Sends one parameter, records it on the node, and brings the curve and indicator along. */
function applyParam(key: string, value: number, commit: boolean): void {
  const node = getGlobalPickupNode();
  if (node) {
    node.params[key] = value;
  }
  sendGlobalChainParam(`pickup.${key}`, value);
  if (commit) {
    appendLog(`preChainGraph.global_pickup.${key} → ${value}`);
  }
  if (node && tabRoot?.isConnected) {
    updatePickupResponseVisualization(node, tabRoot);
  }
  updatePickupMatchIndicator();
}

function applyEnabled(enabled: boolean): void {
  sendGlobalChainParam("pickup.enabled", enabled);
  const node = getGlobalPickupNode();
  if (node) {
    node.bypassed = !enabled;
  }
  appendLog(`preChainGraph.global_pickup.enabled → ${enabled}`);
  tabRoot?.querySelector<HTMLElement>(".pickup-match-tab")?.classList.toggle("is-off", !enabled);
  updatePickupMatchIndicator();
}

const binding: FlyoutParamBinding = {
  idPrefix: GLOBAL_PICKUP_NODE_ID,
  value: (def) => paramValue(def),
  apply: (key, value, commit) => applyParam(key, value, commit),
};

/** Wraps rows the way the flyout lays them out. */
function rows(defs: readonly ParameterDef[]): string {
  return `<div class="flyout-settings-params">${buildFlyoutParamRows(defs, binding)}</div>`;
}

/**
 * Draws the tab into `container`. Drawn afresh each time it is shown, so it always reflects
 * the node and the catalogue as they are now; the catalogue lands after the first render.
 */
export function renderPickupMatchTab(container: HTMLElement): void {
  tabRoot = container;
  const node = getGlobalPickupNode();
  const defs = pickupParamDefs();

  if (!node || defs.length === 0) {
    container.innerHTML = `<p class="pickup-match-tab-hint">Pickup Match is not available yet.</p>`;
    return;
  }

  const enabled = isGlobalPickupMatchOn();
  // The design parameters live in the design blocks, as they do in the params panel.
  const shown = defs.filter((def) => !PICKUP_DESIGN_PARAM_KEYS.includes(def.key));
  const main = shown.filter((def) => !def.advanced);
  const advanced = shown.filter((def) => Boolean(def.advanced));

  container.innerHTML = `
    <div class="pickup-match-tab${enabled ? "" : " is-off"}">
      <div class="flyout-settings-header">
        <span class="flyout-settings-title">Pickup Match</span>
        <label class="toggle-switch" title="Pickup Match on or off">
          <input type="checkbox" class="pickup-match-enable-input" ${enabled ? "checked" : ""} />
          <span class="toggle-slider"></span>
        </label>
      </div>
      <p class="pickup-match-tab-hint">Converts your pickup first, before every preset: set Your Pickup to the
        guitar you play and Sound Like to the pickup you want to hear.</p>
      ${buildPickupResponseSectionHtml(node)}
      ${rows(main)}
      ${buildPickupDesignSectionHtml(node, defs, rows)}
      ${advanced.length === 0 ? "" : `<div class="flyout-settings-section-title">Advanced</div>${rows(advanced)}`}
    </div>`;

  tabKnobs = bindFlyoutParamRows(container, defs.filter((def) => !isPickerParam(def)), binding);
  drawnLayout = layoutKey();
  bindEnumPickerTriggers(container, node, defs, (def, value) => applyParam(def.key, value, true));
  container.querySelector<HTMLInputElement>(".pickup-match-enable-input")
    ?.addEventListener("change", (event) => applyEnabled((event.target as HTMLInputElement).checked));
  updatePickupResponseVisualization(node, container);
}

/** "Single Coil (Bridge) → Humbucker (Bridge)", from the node's two choices. */
function conversionLabel(): { text: string; icon?: string } {
  const defs = pickupParamDefs();
  const choice = (key: string) => {
    const def = defs.find((candidate) => candidate.key === key);
    return def?.labels ? pickerChoices(def)[pickerChoiceIndex(def, paramValue(def))] : undefined;
  };
  const source = choice("source");
  const target = choice("target");
  return {
    text: source && target ? `${source.label} → ${target.label}` : "",
    icon: target?.info.icon,
  };
}

/** Shows the indicator while Pickup Match is on, with the target's picture. */
export function updatePickupMatchIndicator(): void {
  const indicator = document.getElementById("pickup-match-indicator");
  if (!indicator) {
    return;
  }
  const on = isGlobalPickupMatchOn();
  indicator.hidden = !on;
  if (!on) {
    return;
  }
  const { text, icon } = conversionLabel();
  indicator.innerHTML = pickerIcon(icon);
  const title = text ? `Pickup Match on: ${text}` : "Pickup Match on";
  indicator.title = title;
  indicator.setAttribute("aria-label", `${title}. Open its settings`);
}

/**
 * The global chain arrived or changed: bring the indicator and an open tab up to date. The tab
 * is redrawn only when its layout would change (on/off, a pickup choice); otherwise its controls
 * just take the new values, so a knob being dragged is not pulled out from under the pointer.
 */
export function syncPickupMatchFromState(): void {
  updatePickupMatchIndicator();
  const node = getGlobalPickupNode();
  if (!tabRoot?.isConnected || tabRoot.hidden || !node) {
    return;
  }
  if (layoutKey() !== drawnLayout) {
    renderPickupMatchTab(tabRoot);
    return;
  }
  refreshFlyoutParamRows(tabRoot, pickupParamDefs().filter((def) => !isPickerParam(def)), tabKnobs, binding);
  updatePickupResponseVisualization(node, tabRoot);
}
