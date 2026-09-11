/**
 * The generic parameter controls: the tab strip, the default knob/toggle markup,
 * and the bindings that push a change back to the engine.
 */

import { GenericKnob, enhanceRangeInput } from "../../controls.js";
import { formatParamValue } from "../../layoutRenderer.js";
import { getNodeEffectInfo } from "../../presetV2.js";
import type { ParameterDef } from "../../presetV2.js";
import { BLEND_MAPPING_EPS, buildParameterMapFromLegacy, getBlendState, normalizeBlendValue, updateBlendMatchSummary, updateBlendParamIndicators } from "../../signalPathBlend.js";
import type { BlendParamSpec } from "../../signalPathBlend.js";
import type { BlendMode, BlendModelMapping, GraphNode, Preset } from "../../types.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";
import { updateEqVisualization } from "./eq.js";
import { showNodeParamsPanel } from "./panel.js";
import { updateSpatialVisualization } from "./spatial.js";
import { nodeParamKnobs } from "./state.js";

export function bindParamTabs(): void {
  const tabButtons = nodeParamsPanelElement?.querySelectorAll(".node-param-tab");
  const tabPanels = nodeParamsPanelElement?.querySelectorAll(".node-param-tab-panel");
  if (!tabButtons || !tabPanels || tabButtons.length === 0 || tabPanels.length === 0) {
    return;
  }

  tabButtons.forEach((button) => {
    button.addEventListener("click", () => {
      const tab = (button as HTMLElement).dataset.tab;
      if (!tab) return;

      tabButtons.forEach((btn) => {
        const active = (btn as HTMLElement).dataset.tab === tab;
        btn.classList.toggle("is-active", active);
        btn.setAttribute("aria-selected", active ? "true" : "false");
      });
      tabPanels.forEach((panel) => {
        const active = (panel as HTMLElement).dataset.tab === tab;
        panel.classList.toggle("is-active", active);
      });
    });
  });
}

export function formatParamLabel(key: string): string {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (char) => char.toUpperCase());
}

export function isToggleParam(paramDef: { key: string; min?: number; max?: number; unit?: string }): boolean {
  return paramDef.unit==="toggle";
}

/**
 * Build the default parameter controls HTML using only default values.
 * Produces the same DOM structure as the live renderParamControl path so the
 * layout designer can render a faithful preview without a live node.
 * nodeId is used for data attributes so knob CSS still applies correctly.
 */
export function buildDefaultParamControlsHtml(
  paramDefs: ParameterDef[],
  nodeId = "preview"
): string {
  const renderOne = (p: ParameterDef): string => {
    const label = p.name || formatParamLabel(p.key);
    const value = p.default ?? 0;
    const min = p.min ?? 0;
    const max = p.max ?? 1;
    const unit = p.unit || "amount";
    const isToggle = isToggleParam(p);
    const isEnum = unit === "enum" && Array.isArray(p.labels) && p.labels.length > 0;
    const enumLabels = Array.isArray(p.labels) ? p.labels : [];

    if (isToggle) {
      const checked = value >= 0.5;
      return `
        <div class="node-param-group">
          <span class="node-param-label">${label}</span>
          <label class="toggle-switch">
            <input class="node-param-toggle" type="checkbox" data-node-id="${nodeId}" data-param-key="${p.key}" ${checked ? "checked" : ""} disabled>
            <span class="toggle-slider"></span>
          </label>
          <span class="node-param-value">${checked ? "On" : "Off"}</span>
        </div>`;
    }

    return `
      <div class="node-param-group">
        <span class="node-param-label">${label}</span>
        <div class="knob node-param-knob"
          data-node-id="${nodeId}"
          data-param-key="${p.key}"
          data-value="${value}"
          data-default="${value}"
          data-min="${min}"
          data-max="${max}"
          data-unit="${unit}"
          ${p.step !== undefined ? `data-step="${p.step}"` : ""}
          ${isEnum ? `data-labels="${enumLabels.join("|")}"` : ""}
        >
          <div class="knob-indicator"></div>
        </div>
        <span class="node-param-value">${formatParamValue(value, unit, enumLabels)}</span>
      </div>`;
  };

  const hasGroups = paramDefs.some((p) => typeof p.group === "string" && p.group.trim().length > 0);
  if (!hasGroups) {
    return paramDefs.map(renderOne).join("");
  }

  const groupOrder: string[] = [];
  const groupMap = new Map<string, string[]>();
  paramDefs.forEach((p) => {
    const group = p.group?.trim() || "Other";
    if (!groupMap.has(group)) {
      groupMap.set(group, []);
      groupOrder.push(group);
    }
    groupMap.get(group)!.push(renderOne(p));
  });

  return groupOrder.map((group) => `
    <div class="node-param-group-block">
      <div class="node-param-group-title">${group}</div>
      <div class="node-param-group-items">
        ${groupMap.get(group)!.join("")}
      </div>
    </div>`).join("");
}

export function bindNodeParamControls(node: GraphNode, preset: Preset): void {
  // Bind slider inputs from custom layouts
  const sliders = nodeParamsPanelElement?.querySelectorAll(".node-param-slider");
  sliders?.forEach((sliderEl) => {
    const input = sliderEl as HTMLInputElement;
    enhanceRangeInput(input);
    input.addEventListener("input", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = parseFloat(input.value);
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);

        // Update associated value display
        const parentControl = input.closest(".custom-layout-control");
        const valueEl = parentControl?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueEl) {
          const paramDef = getNodeEffectInfo(node)?.parameters.find((p) => p.key === paramKey);
          if (paramDef) {
            if (paramDef.unit === "dB" || paramDef.unit === "ms" || paramDef.unit === "Hz") {
              valueEl.textContent = `${value.toFixed(1)}${paramDef.unit}`;
            } else if (paramDef.unit === "enum" && Array.isArray(paramDef.labels)) {
              valueEl.textContent = paramDef.labels[Math.round(value)] ?? `${Math.round(value)}`;
            } else {
              valueEl.textContent = value.toFixed(2);
            }
          }
        }
      }
    });
  });

  // Bind blend slider inputs (irBlend-style A/B range controls)
  const blendSliders = nodeParamsPanelElement?.querySelectorAll(".node-param-blend-slider");
  blendSliders?.forEach((sliderEl) => {
    const input = sliderEl as HTMLInputElement;
    enhanceRangeInput(input);
    input.addEventListener("input", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = parseFloat(input.value);
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);

        const valueEl = input.closest(".node-param-blend-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueEl) {
          valueEl.textContent = value <= 0.01 ? "A" : value >= 0.99 ? "B" : `${Math.round(value * 100)}%`;
        }
      }
    });
  });

  const toggles = nodeParamsPanelElement?.querySelectorAll(".node-param-toggle");
  toggles?.forEach((toggleEl) => {
    const input = toggleEl as HTMLInputElement;
    input.addEventListener("change", () => {
      const nodeId = input.dataset.nodeId;
      const paramKey = input.dataset.paramKey;
      if (nodeId && paramKey) {
        const value = input.checked ? 1 : 0;
        node.params[paramKey] = value;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, value);
        
        // Handle standard toggle labels
        const valueLabel = input.closest(".node-param-group")?.querySelector(".node-param-value") as HTMLElement | null;
        if (valueLabel) {
          valueLabel.textContent = input.checked ? "On" : "Off";
        }
        
        // Handle mixer mute labels
        const muteLabel = input.closest(".mixer-input-header")?.querySelector(".mixer-mute-label") as HTMLElement | null;
        if (muteLabel) {
          muteLabel.textContent = input.checked ? "Muted" : "Active";
        }
        
        updateEqVisualization(node);
      }
    });
  });

  const knobs = nodeParamsPanelElement?.querySelectorAll(".node-param-knob");
  if (!knobs) {
    return;
  }

  const blendState = getBlendState(node);

  const findClosestBlendMappingForParam = (
    activeParamId: string,
    targetValue: number,
    target: Record<string, number>,
  ): BlendModelMapping | null => {
    if (!blendState) {
      return null;
    }

    let best: BlendModelMapping | null = null;
    let bestDelta = Number.POSITIVE_INFINITY;
    let bestSecondary = Number.POSITIVE_INFINITY;

    blendState.mappings.forEach((mapping) => {
      const params = buildParameterMapFromLegacy(mapping);
      const mappedValue = params[activeParamId];
      if (typeof mappedValue !== "number") {
        return;
      }

      const delta = Math.abs(mappedValue - targetValue);
      let secondary = 0;

      blendState.paramIds.forEach((paramId) => {
        if (paramId === activeParamId) {
          return;
        }
        const targetOther = target[paramId];
        const mappedOther = params[paramId];
        if (typeof targetOther !== "number" || typeof mappedOther !== "number") {
          secondary += 4;
          return;
        }
        const diff = mappedOther - targetOther;
        secondary += diff * diff;
      });

      const isBetter = delta < bestDelta - BLEND_MAPPING_EPS
        || (Math.abs(delta - bestDelta) <= BLEND_MAPPING_EPS && secondary < bestSecondary);
      if (isBetter) {
        best = mapping;
        bestDelta = delta;
        bestSecondary = secondary;
      }
    });

    return best;
  };

  knobs.forEach((knobElement) => {
    const knob = knobElement as HTMLElement;
    const valueDisplay = knob.parentElement?.querySelector(".node-param-value") as HTMLElement | null;
    
    const nodeId = knob.dataset.nodeId;
    const paramKey = knob.dataset.paramKey;
    const min = parseFloat(knob.dataset.min || "0");
    const max = parseFloat(knob.dataset.max || "1");
    const unit = knob.dataset.unit || "amount";
    const defaultValue = parseFloat(knob.dataset.default || knob.dataset.value || "0");
    const sensitivity = (max - min) / 200;
    const step = knob.dataset.step ? parseFloat(knob.dataset.step) : undefined;
    const labels = (knob.dataset.labels || "").split("|").filter(Boolean);
    const isEnum = unit === "enum" && labels.length > 0;
    const isBlendParam = knob.dataset.blendParam === "true";
    const isPitchShiftSemitones = node.type === "pitch_shift" && paramKey === "semitones";
    const isPitchShiftStepMode = node.type === "pitch_shift" && paramKey === "stepMode";
    const isPitchShiftMin = node.type === "pitch_shift" && paramKey === "minSemitones";
    const isPitchShiftMax = node.type === "pitch_shift" && paramKey === "maxSemitones";
    const blendSpecMin = knob.dataset.blendSpecMin ? parseFloat(knob.dataset.blendSpecMin) : 0;
    const blendSpecMax = knob.dataset.blendSpecMax ? parseFloat(knob.dataset.blendSpecMax) : 10;
    const blendMode = (knob.dataset.blendMode ?? "interpolate") as BlendMode;
    const blendSpec: BlendParamSpec | null = isBlendParam
      ? { id: paramKey ?? "", label: paramKey ?? "", min: blendSpecMin, max: blendSpecMax }
      : null;

    const snapValue = (rawValue: number): number => {
      if (isPitchShiftSemitones && (node.params.stepMode ?? 1) >= 0.5) {
        const minBound = typeof node.params.minSemitones === "number" ? node.params.minSemitones : -12;
        const maxBound = typeof node.params.maxSemitones === "number" ? node.params.maxSemitones : 12;
        const range = Math.max(0.0, maxBound - minBound);
        if (range <= 0.0) return Math.max(min, Math.min(max, rawValue));
        const mapped = minBound + (rawValue + 1) * 0.5 * range;
        const snappedSemitones = Math.max(minBound, Math.min(maxBound, Math.round(mapped)));
        const snappedControl = ((snappedSemitones - minBound) / range) * 2 - 1;
        return Math.max(min, Math.min(max, snappedControl));
      }
      if (!step || step <= 0) return rawValue;
      const snapped = Math.round((rawValue - min) / step) * step + min;
      return Math.max(min, Math.min(max, snapped));
    };

    const formatValue = (rawValue: number): string => {
      if (isEnum) {
        const index = Math.round(rawValue);
        return labels[index] ?? `${index}`;
      }
      if (isBlendParam) {
        return rawValue.toFixed(1);
      }
      // Special formatting for pan values
      if (unit === "pan") {
        if (Math.abs(rawValue) < 0.01) return "C";
        return rawValue < 0 
          ? `L${Math.abs(rawValue * 100).toFixed(0)}`
          : `R${(rawValue * 100).toFixed(0)}`;
      }
      return `${rawValue.toFixed(2)}${unit === "amount" ? "" : unit}`;
    };

    const knobInstance = new GenericKnob({
      knobElement: knob,
      paramId: `${nodeId ?? "node"}_${paramKey ?? "param"}`,
      minValue: min,
      maxValue: max,
      defaultValue,
      displayFormat: (value) => formatValue(value),
      valueDisplay,
      labelElement: knob.parentElement?.querySelector(".node-param-label, .custom-control-label") as HTMLElement | null,
      sensitivity,
      stepValue: step,
      sendParameter: false,
      onValueChange: (value) => {
        if (!nodeId || !paramKey) return;
        const finalValue = snapValue(value);
        if (finalValue !== value) {
          knobInstance.setValue(finalValue);
        }

        const normalizedValue = isBlendParam ? normalizeBlendValue(finalValue, blendSpec) : finalValue;

        if (isBlendParam && blendMode === "snap" && blendState) {
          const target: Record<string, number> = { ...node.params, [paramKey]: normalizedValue };
          const closest = findClosestBlendMappingForParam(paramKey, normalizedValue, target);
          if (closest) {
            const params = buildParameterMapFromLegacy(closest);
            let updated = false;
            blendState.paramIds.forEach((paramId) => {
              const mappedValue = params[paramId];
              if (typeof mappedValue === "number" && node.params[paramId] !== mappedValue) {
                node.params[paramId] = mappedValue;
                sendSignalPathNodeParamUpdate(nodeId, paramId, mappedValue);
                updated = true;
              }
            });
            if (updated) {
              showNodeParamsPanel(node, preset);
            }
            return;
          }
        }

        node.params[paramKey] = normalizedValue;
        sendSignalPathNodeParamUpdate(nodeId, paramKey, normalizedValue);

        if (isPitchShiftMin || isPitchShiftMax) {
          const minBound = typeof node.params.minSemitones === "number" ? node.params.minSemitones : -12;
          const maxBound = typeof node.params.maxSemitones === "number" ? node.params.maxSemitones : 12;
          let nextMin = minBound;
          let nextMax = maxBound;

          if (isPitchShiftMin) {
            nextMin = Math.max(-12, Math.min(12, normalizedValue));
            nextMax = Math.max(nextMin, maxBound);
          } else {
            nextMax = Math.max(-12, Math.min(12, normalizedValue));
            nextMin = Math.min(nextMax, minBound);
          }

          if (nextMin !== minBound) {
            node.params.minSemitones = nextMin;
            sendSignalPathNodeParamUpdate(nodeId, "minSemitones", nextMin);
          }
          if (nextMax !== maxBound) {
            node.params.maxSemitones = nextMax;
            sendSignalPathNodeParamUpdate(nodeId, "maxSemitones", nextMax);
          }

          const currentSemitones = typeof node.params.semitones === "number" ? node.params.semitones : 0;
          const clampedSemitones = Math.max(nextMin, Math.min(nextMax, currentSemitones));
          if (clampedSemitones !== currentSemitones) {
            node.params.semitones = clampedSemitones;
            sendSignalPathNodeParamUpdate(nodeId, "semitones", clampedSemitones);
          }

          showNodeParamsPanel(node, preset);
          return;
        }

        if (isPitchShiftStepMode && normalizedValue >= 0.5) {
          const currentControl = typeof node.params.semitones === "number" ? node.params.semitones : 0;
          const snappedControl = snapValue(currentControl);
          if (snappedControl !== currentControl) {
            node.params.semitones = snappedControl;
            sendSignalPathNodeParamUpdate(nodeId, "semitones", snappedControl);
            showNodeParamsPanel(node, preset);
            return;
          }
        }
        updateEqVisualization(node);
        updateSpatialVisualization(node);

        if (isBlendParam && blendState) {
          updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
          updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
        }
      },
    });

    // Store knob instance for live EQ curve sync
    if (paramKey) {
      nodeParamKnobs.set(paramKey, knobInstance);
    }
  });

  if (blendState) {
    updateBlendParamIndicators(nodeParamsPanelElement, node, blendState);
    updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
  }
}
