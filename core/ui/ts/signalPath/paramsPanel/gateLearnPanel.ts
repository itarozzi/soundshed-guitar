/**
 * A Noise Gate or Multi-band Gate node's Learn section: the meter of the noise it heard against
 * its threshold (one meter per band for the Multi-band Gate), and the button that listens
 * (gateLearn.ts). A finished listen sets the thresholds the way the knobs would, so the change is
 * saved with the preset and lands in undo.
 */

import { EffectGuids } from "../../effectGuids.js";
import {
  bindGateLearnView,
  findGateLearnView,
  gateLearnMeterHtml,
  type GateLearnStatus,
  type GateLearnTarget,
  refreshGateLearnViews,
  registerGateLearnApplier,
} from "../../gateLearn.js";
import { isNodeBypassed } from "../../graphNodes.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import { getSignalPathPreset } from "../../state.js";
import type { GraphNode, Preset } from "../../types.js";
import { applySignalPathNodeBypassState } from "../bypass.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";
import { nodeParamKnobs } from "./state.js";

const FALLBACK = { threshold: -60, hysteresis: 4, min: -80, max: 0 };
const MULTIBAND_FALLBACK = { threshold: -65, min: -96, max: 0 };

/** The Multi-band Gate's threshold parameters, low band first, and the bands' labels. */
const BAND_THRESHOLD_KEYS = ["threshold1", "threshold2", "threshold3", "threshold4", "threshold5", "threshold6"];
const BAND_LABELS = ["< 150 Hz", "150–400", "400–1k", "1k–2.5k", "2.5k–6k", "> 6 kHz"];

export function isNoiseGateNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kDynamicsGate;
}

export function isMultibandGateNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kMultibandGate;
}

function hasLearn(node: GraphNode): boolean {
  return isNoiseGateNode(node) || isMultibandGateNode(node);
}

function learnTarget(node: GraphNode, preset: Preset | null | undefined): GateLearnTarget {
  return { scope: "preset", nodeId: node.id, presetId: preset?.id ?? "" };
}

function paramDef(effectType: string, key: string) {
  return EffectTypeRegistry.get(effectType)?.parameters.find((candidate) => candidate.key === key);
}

function paramValue(node: GraphNode, effectType: string, key: string, fallback: number): number {
  const stored = node.params[key];
  if (typeof stored === "number" && Number.isFinite(stored)) {
    return stored;
  }
  return paramDef(effectType, key)?.default ?? fallback;
}

function thresholdRange(effectType: string, key: string, fallback: { min: number; max: number }) {
  const def = paramDef(effectType, key);
  return def ? { min: def.min, max: def.max } : { min: fallback.min, max: fallback.max };
}

/** The section's markup, or "" for any other effect. */
export function buildGateLearnSectionHtml(node: GraphNode): string {
  if (!hasLearn(node)) {
    return "";
  }
  const target = learnTarget(node, getSignalPathPreset());
  return `
    <section class="gate-learn-section">
      <div class="eq-visualizer gate-learn-visualizer">
        <div class="eq-visualizer-header">
          <span>${isMultibandGateNode(node) ? "Noise Floor by Band" : "Noise Floor"}</span>
        </div>
        ${isMultibandGateNode(node) ? gateLearnMeterHtml(target, BAND_LABELS) : gateLearnMeterHtml(target)}
      </div>
    </section>
  `;
}

export function bindGateLearnSection(node: GraphNode, preset: Preset): void {
  if (!hasLearn(node) || !nodeParamsPanelElement) {
    return;
  }
  const ensureEnabled = () => {
    if (isNodeBypassed(node)) {
      applySignalPathNodeBypassState(node, preset, false);
    }
  };

  if (isMultibandGateNode(node)) {
    const type = EffectGuids.kMultibandGate;
    bindGateLearnView(nodeParamsPanelElement, {
      target: learnTarget(node, preset),
      threshold: () => paramValue(node, type, BAND_THRESHOLD_KEYS[0], MULTIBAND_FALLBACK.threshold),
      hysteresis: () => 0,
      bandThresholds: () => BAND_THRESHOLD_KEYS.map((key) => {
        // Offset moves every band's threshold, so the meters show where each one really sits.
        const offset = paramValue(node, type, "offset", 0);
        return paramValue(node, type, key, MULTIBAND_FALLBACK.threshold) + offset;
      }),
      range: () => thresholdRange(type, BAND_THRESHOLD_KEYS[0], MULTIBAND_FALLBACK),
      ensureEnabled,
    });
    return;
  }

  const type = EffectGuids.kDynamicsGate;
  bindGateLearnView(nodeParamsPanelElement, {
    target: learnTarget(node, preset),
    threshold: () => paramValue(node, type, "threshold", FALLBACK.threshold),
    hysteresis: () => paramValue(node, type, "hysteresis", FALLBACK.hysteresis),
    range: () => thresholdRange(type, "threshold", FALLBACK),
    ensureEnabled,
  });
}

/** After one of the node's parameters changes, so the meter follows its thresholds. */
export function updateGateLearnVisualization(node: GraphNode): void {
  if (hasLearn(node)) {
    refreshGateLearnViews(learnTarget(node, getSignalPathPreset()));
  }
}

/** Sets one learned parameter on the node, and its knob when the panel shows this node. */
function applyParam(node: GraphNode, key: string, value: number, panelShowsNode: boolean): void {
  node.params[key] = value;
  sendSignalPathNodeParamUpdate(node.id, key, value);
  if (panelShowsNode) {
    nodeParamKnobs.get(key)?.setValue(value);
  }
}

/**
 * A finished listen, applied to the node it measured — as long as that preset is still the
 * one being edited. One switched away from during the two seconds is left alone.
 */
function applyLearnedThresholds(target: GateLearnTarget, status: GateLearnStatus): void {
  const preset = getSignalPathPreset();
  if (!preset || preset.id !== target.presetId) {
    return;
  }
  const node = preset.graph?.nodes.find((candidate) => candidate.id === target.nodeId);
  if (!node) {
    return;
  }

  // The knobs belong to whichever node the panel shows: move them only if that is this one.
  const panelShowsNode = Boolean(nodeParamsPanelElement && findGateLearnView(nodeParamsPanelElement, target));

  if (isMultibandGateNode(node)) {
    status.bands?.forEach((band, index) => {
      const key = BAND_THRESHOLD_KEYS[index];
      if (key && band.thresholdDb !== undefined) {
        applyParam(node, key, band.thresholdDb, panelShowsNode);
      }
    });
    return;
  }

  if (status.thresholdDb !== undefined) {
    applyParam(node, "threshold", status.thresholdDb, panelShowsNode);
  }
}

registerGateLearnApplier("preset", applyLearnedThresholds);
