/**
 * A Noise Gate node's Learn section: the meter of the noise it heard against its threshold,
 * and the button that listens (gateLearn.ts). A finished listen sets Threshold the way the
 * knob would, so the change is saved with the preset and lands in undo.
 */

import { EffectGuids } from "../../effectGuids.js";
import {
  bindGateLearnView,
  findGateLearnView,
  gateLearnMeterHtml,
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

export function isNoiseGateNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kDynamicsGate;
}

function learnTarget(node: GraphNode, preset: Preset | null | undefined): GateLearnTarget {
  return { scope: "preset", nodeId: node.id, presetId: preset?.id ?? "" };
}

function paramValue(node: GraphNode, key: "threshold" | "hysteresis"): number {
  const stored = node.params[key];
  if (typeof stored === "number" && Number.isFinite(stored)) {
    return stored;
  }
  const def = EffectTypeRegistry.get(EffectGuids.kDynamicsGate)?.parameters.find((candidate) => candidate.key === key);
  return def?.default ?? FALLBACK[key];
}

function thresholdRange(): { min: number; max: number } {
  const def = EffectTypeRegistry.get(EffectGuids.kDynamicsGate)?.parameters.find((candidate) => candidate.key === "threshold");
  return def ? { min: def.min, max: def.max } : { min: FALLBACK.min, max: FALLBACK.max };
}

/** The section's markup, or "" for any other effect. */
export function buildGateLearnSectionHtml(node: GraphNode): string {
  if (!isNoiseGateNode(node)) {
    return "";
  }
  return `
    <section class="gate-learn-section">
      <div class="eq-visualizer gate-learn-visualizer">
        <div class="eq-visualizer-header">
          <span>Noise Floor</span>
        </div>
        ${gateLearnMeterHtml(learnTarget(node, getSignalPathPreset()))}
      </div>
    </section>
  `;
}

export function bindGateLearnSection(node: GraphNode, preset: Preset): void {
  if (!isNoiseGateNode(node) || !nodeParamsPanelElement) {
    return;
  }
  bindGateLearnView(nodeParamsPanelElement, {
    target: learnTarget(node, preset),
    threshold: () => paramValue(node, "threshold"),
    hysteresis: () => paramValue(node, "hysteresis"),
    range: thresholdRange,
    ensureEnabled: () => {
      if (isNodeBypassed(node)) {
        applySignalPathNodeBypassState(node, preset, false);
      }
    },
  });
}

/** After one of the node's parameters changes, so the meter follows Threshold and Hysteresis. */
export function updateGateLearnVisualization(node: GraphNode): void {
  if (isNoiseGateNode(node)) {
    refreshGateLearnViews(learnTarget(node, getSignalPathPreset()));
  }
}

/**
 * A finished listen, applied to the node it measured — as long as that preset is still the
 * one being edited. One switched away from during the two seconds is left alone.
 */
function applyLearnedThreshold(target: GateLearnTarget, thresholdDb: number): void {
  const preset = getSignalPathPreset();
  if (!preset || preset.id !== target.presetId) {
    return;
  }
  const node = preset.graph?.nodes.find((candidate) => candidate.id === target.nodeId);
  if (!node) {
    return;
  }

  node.params.threshold = thresholdDb;
  sendSignalPathNodeParamUpdate(node.id, "threshold", thresholdDb);

  // The knob belongs to whichever node the panel shows: move it only if that is this one.
  if (nodeParamsPanelElement && findGateLearnView(nodeParamsPanelElement, target)) {
    nodeParamKnobs.get("threshold")?.setValue(thresholdDb);
  }
}

registerGateLearnApplier("preset", applyLearnedThreshold);
