/**
 * The global chain's Pickup Match: the "global_pickup" node straight after the input, bypassed
 * until switched on. It converts the player's pickup ahead of everything else, whatever preset
 * plays. This module draws its tab in the input calibration flyout (settings/inputCalibration.ts
 * hosts it) and the control bar's indicator, which shows while it is on.
 *
 * It is experimental, behind the Pickup Match feature flag (off by default). With the flag off
 * neither the tab nor the indicator shows, and a Pickup Match left on is switched off, so nothing
 * the player cannot see changes their sound.
 *
 * The tab is built from the same pieces as Pickup Match's params panel, so the two cannot drift:
 * - flyout rows for its knobs (flyoutParamControls.ts, shared with the gate flyout);
 * - the pickup pickers (enumPicker.ts);
 * - the conversion curve and the custom-design blocks (pickupResponse.ts).
 *
 * Values live on the node. A change is sent as a `pickup.<param>` global-chain param and
 * written onto the node, the copy the settings blob is saved from, as the gate does.
 *
 * My Guitars lets a player save the settings for each guitar they own under its name and switch
 * between them. They are Pickup Match's ordinary user effect presets (effect-presets.json, by
 * effect type), so one saved here is offered by a Pickup Match node's Presets button too, and
 * the other way round. Which one was chosen is not stored: the dropdown shows the guitar whose
 * every setting the node holds now, so after a knob is turned it reads "Unsaved settings" until
 * the guitar is saved again.
 */

import { haveAppSettingsArrived } from "./appSettingsStore.js";
import { postMessage, sendGlobalChainParam } from "./bridge.js";
import { showConfirm } from "./dialogs.js";
import { EffectGuids } from "./effectGuids.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";
import {
  bindFlyoutParamRows,
  buildFlyoutParamRows,
  type FlyoutParamBinding,
  refreshFlyoutParamRows,
} from "./flyoutParamControls.js";
import type { GenericKnob } from "./knob.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { pickerIcon } from "./pickerIcons.js";
import { EffectTypeRegistry, type ParameterDef } from "./presetV2.js";
import {
  effectPresetStorageKey,
  getUserEffectPresets,
  sortFactoryEffectPresets,
} from "./signalPath/effectPresets.js";
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
import type { GraphNode, StoredEffectPreset } from "./types.js";
import { escapeHtml } from "./utils.js";

export const GLOBAL_PICKUP_NODE_ID = "global_pickup";

/** The tab's container while it is drawn, so a change can redraw its curve and design blocks. */
let tabRoot: HTMLElement | null = null;
let tabKnobs = new Map<string, GenericKnob>();
/** What the drawn tab's layout depends on; anything else only moves values. */
let drawnLayout = "";
/** What the drawn guitar list shows, so a knob turn that changes nothing there leaves it be. */
let drawnGuitarList = "";

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
  refreshGuitarList();
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

/** The user's saved guitars: Pickup Match's user effect presets. */
function savedGuitars(): StoredEffectPreset[] {
  const node = getGlobalPickupNode();
  return node ? getUserEffectPresets(node) : [];
}

/** A saved value for one of the effect's parameters, if the entry holds a usable one. */
function savedValue(entry: StoredEffectPreset, def: ParameterDef): number | undefined {
  const value = entry.parameters?.[def.key];
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

/**
 * True when the node holds every setting the entry saved. Parameters the entry predates are
 * not compared, as loading it leaves them alone.
 */
function guitarMatchesNode(entry: StoredEffectPreset): boolean {
  let compared = 0;
  for (const def of pickupParamDefs()) {
    const saved = savedValue(entry, def);
    if (saved === undefined) {
      continue;
    }
    if (Math.abs(saved - paramValue(def)) > 1e-6) {
      return false;
    }
    compared++;
  }
  return compared > 0;
}

/** The saved guitar the node is set to now, if any. */
function currentGuitar(): StoredEffectPreset | undefined {
  return savedGuitars().find(guitarMatchesNode);
}

/** "Single Coil (Bridge) → Humbucker (Bridge)", from a source and target value lookup. */
function conversionChoices(value: (def: ParameterDef) => number) {
  const defs = pickupParamDefs();
  const choice = (key: string) => {
    const def = defs.find((candidate) => candidate.key === key);
    return def?.labels ? pickerChoices(def)[pickerChoiceIndex(def, value(def))] : undefined;
  };
  const source = choice("source");
  const target = choice("target");
  return {
    text: source && target ? `${source.label} → ${target.label}` : "",
    icon: target?.info.icon,
  };
}

const DELETE_ICON = '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/></svg>';

/**
 * A dropdown of the saved guitars in name order, so a long list stays one row, showing the current
 * one, or "Unsaved settings" when none matches. Beside it, a delete for the current guitar.
 */
function guitarListHtml(guitars: readonly StoredEffectPreset[], current: StoredEffectPreset | undefined): string {
  if (guitars.length === 0) {
    return `<p class="pickup-match-tab-hint">Set Your Pickup for the guitar you are playing, then save it under
      the guitar's name to switch back to it in one click.</p>`;
  }
  const options = sortFactoryEffectPresets(guitars).map((entry) =>
    `<option value="${escapeHtml(entry.id)}"${entry.id === current?.id ? " selected" : ""}>${escapeHtml(entry.name)}</option>`,
  ).join("");
  const conversion = current ? conversionChoices((def) => savedValue(current, def) ?? def.default).text : "";
  const deleteLabel = escapeHtml(current ? `Delete ${current.name}` : "Load a saved guitar to delete it");
  return `
    <div class="pickup-guitar-row">
      <select class="pickup-guitar-select" aria-label="Load a saved guitar" title="${escapeHtml(conversion)}">
        ${current ? "" : `<option value="" disabled selected>Unsaved settings</option>`}
        ${options}
      </select>
      <button type="button" class="pickup-guitar-delete" data-guitar-delete="${escapeHtml(current?.id ?? "")}"
        ${current ? "" : "disabled"} aria-label="${deleteLabel}" title="${deleteLabel}">${DELETE_ICON}</button>
    </div>`;
}

/** Redraws the guitar list when what it shows has changed: a save, a delete, or a new match. */
function refreshGuitarList(): void {
  const list = tabRoot?.isConnected ? tabRoot.querySelector<HTMLElement>(".pickup-guitar-list") : null;
  if (!list) {
    return;
  }
  const guitars = savedGuitars();
  const current = currentGuitar();
  const key = JSON.stringify([guitars, current?.id ?? null]);
  if (key === drawnGuitarList) {
    return;
  }
  drawnGuitarList = key;
  list.innerHTML = guitarListHtml(guitars, current);
}

/**
 * Sets the node to a saved guitar. Only the settings that differ are sent, each as a knob turn
 * would be; then the tab is redrawn, since a different pickup can change its layout.
 */
function loadGuitar(id: string): void {
  const node = getGlobalPickupNode();
  const entry = savedGuitars().find((candidate) => candidate.id === id);
  if (!node || !entry) {
    return;
  }
  for (const def of pickupParamDefs()) {
    const value = savedValue(entry, def);
    if (value === undefined || value === paramValue(def)) {
      continue;
    }
    node.params[def.key] = value;
    sendGlobalChainParam(`pickup.${def.key}`, value);
  }
  appendLog(`preChainGraph.global_pickup ← guitar "${entry.name}"`);
  if (tabRoot?.isConnected) {
    renderPickupMatchTab(tabRoot);
  }
  updatePickupMatchIndicator();
}

/**
 * Saves every setting under the name in `input`. No node id goes with it: the engine looks a
 * node up in the active preset's chain, which the global node is not in, and Pickup Match has
 * no resources or config, so the parameters are the whole of it.
 */
async function saveGuitar(input: HTMLInputElement): Promise<void> {
  const node = getGlobalPickupNode();
  if (!node) {
    return;
  }
  const name = input.value.trim();
  if (!name) {
    input.focus();
    showNotification("Name required", "Enter the name of the guitar these settings are for.");
    return;
  }
  if (savedGuitars().some((entry) => entry.name === name)
      && !(await showConfirm(`Replace the saved settings for "${name}"?`, "Overwrite guitar"))) {
    return;
  }
  const parameters = Object.fromEntries(pickupParamDefs().map((def) => [def.key, paramValue(def)]));
  postMessage({ type: "saveEffectPreset", effectType: effectPresetStorageKey(node), name, parameters });
  input.value = "";
  showNotification("Guitar saved", name);
}

async function deleteGuitar(id: string): Promise<void> {
  const node = getGlobalPickupNode();
  const entry = savedGuitars().find((candidate) => candidate.id === id);
  if (!node || !entry) {
    return;
  }
  if (!(await showConfirm(`Delete the saved settings for "${entry.name}"?`, "Delete guitar"))) {
    return;
  }
  postMessage({ type: "deleteEffectPreset", effectType: effectPresetStorageKey(node), presetId: entry.id });
  showNotification("Guitar deleted", entry.name);
}

function guitarsSectionHtml(): string {
  return `
    <div class="pickup-guitars">
      <div class="flyout-settings-section-title">My Guitars</div>
      <div class="pickup-guitar-list"></div>
      <div class="pickup-guitar-save">
        <input type="text" class="pickup-guitar-name-input" maxlength="60" placeholder="Guitar name, e.g. My Strat"
          aria-label="Name of the guitar to save these settings for" />
        <button type="button" class="pickup-guitar-save-button">Save</button>
      </div>
    </div>`;
}

/** One set of listeners for the section; the dropdown inside it is redrawn without rebinding. */
function bindGuitarsSection(container: HTMLElement): void {
  const section = container.querySelector<HTMLElement>(".pickup-guitars");
  const input = section?.querySelector<HTMLInputElement>(".pickup-guitar-name-input");
  if (!section || !input) {
    return;
  }
  section.addEventListener("click", (event) => {
    const target = event.target instanceof Element ? event.target : null;
    const deleteButton = target?.closest<HTMLElement>("[data-guitar-delete]");
    if (deleteButton) {
      void deleteGuitar(deleteButton.dataset.guitarDelete ?? "");
      return;
    }
    if (target?.closest(".pickup-guitar-save-button")) {
      void saveGuitar(input);
    }
  });
  section.addEventListener("change", (event) => {
    if (event.target instanceof HTMLSelectElement && event.target.matches(".pickup-guitar-select")) {
      loadGuitar(event.target.value);
    }
  });
  input.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      void saveGuitar(input);
    }
  });
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
      ${guitarsSectionHtml()}
      ${buildPickupResponseSectionHtml(node)}
      ${rows(main)}
      ${buildPickupDesignSectionHtml(node, defs, rows)}
      ${advanced.length === 0 ? "" : `<div class="flyout-settings-section-title">Advanced</div>${rows(advanced)}`}
    </div>`;

  tabKnobs = bindFlyoutParamRows(container, defs.filter((def) => !isPickerParam(def)), binding);
  drawnLayout = layoutKey();
  drawnGuitarList = "";
  refreshGuitarList();
  bindGuitarsSection(container);
  bindEnumPickerTriggers(container, node, defs, (def, value) => applyParam(def.key, value, true));
  container.querySelector<HTMLInputElement>(".pickup-match-enable-input")
    ?.addEventListener("change", (event) => applyEnabled((event.target as HTMLInputElement).checked));
  updatePickupResponseVisualization(node, container);
}

/** Shows the indicator while Pickup Match is on, with the target's picture and the guitar's name. */
export function updatePickupMatchIndicator(): void {
  const indicator = document.getElementById("pickup-match-indicator");
  if (!indicator) {
    return;
  }
  const on = isFeatureEnabled(Features.PickupMatch) && isGlobalPickupMatchOn();
  indicator.hidden = !on;
  if (!on) {
    return;
  }
  const { text, icon } = conversionChoices(paramValue);
  const guitar = currentGuitar()?.name;
  indicator.innerHTML = pickerIcon(icon);
  const detail = guitar && text ? `${guitar} (${text})` : guitar ?? text;
  const title = detail ? `Pickup Match on: ${detail}` : "Pickup Match on";
  indicator.title = title;
  indicator.setAttribute("aria-label", `${title}. Open its settings`);
}

/** The saved effect presets changed: a guitar was saved or deleted, here or in a params panel. */
export function refreshPickupMatchGuitars(): void {
  refreshGuitarList();
  updatePickupMatchIndicator();
}

/**
 * With the feature flag off, a Pickup Match left on (from before the flag was switched off) is
 * switched off. Not before the app settings arrive: until then the flag reads as its default.
 */
function switchOffWhileFeatureOff(): void {
  if (haveAppSettingsArrived() && !isFeatureEnabled(Features.PickupMatch) && isGlobalPickupMatchOn()) {
    applyEnabled(false);
  }
}

/**
 * The global chain or the feature flag arrived or changed: bring the indicator and an open tab
 * up to date. The tab is redrawn only when its layout would change (on/off, a pickup choice);
 * otherwise its controls just take the new values, so a knob being dragged is not pulled out
 * from under the pointer.
 */
export function syncPickupMatchFromState(): void {
  switchOffWhileFeatureOff();
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
  refreshGuitarList();
  updatePickupResponseVisualization(node, tabRoot);
}
