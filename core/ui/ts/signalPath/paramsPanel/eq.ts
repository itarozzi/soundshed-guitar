/**
 * The EQ curve visualisation, with the live spectrum of the node's input
 * behind it, and the graphic EQ band controls beneath it.
 */

import { EffectGuids } from "../../effectGuids.js";
import { EqCurveInteraction, GRAPHIC_EQ_FREQUENCIES, buildEqBandConfigsFromParams, buildGraphicEqBandConfigs, clampGraphicEqFrequency, drawEqCurve, eqBandChangeToParams } from "../../eqCurve.js";
import { anchoredFrequencyAxis, type EqFrequencyAxis } from "../../eqPlot.js";
import { EqSpectrumWatcher, type EqSpectrumListener, type EqSpectrumSource } from "../../eqSpectrum.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../../presetV2.js";
import { getActivePresetForRender, uiState } from "../../state.js";
import type { GraphNode, Preset } from "../../types.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { requestNodeParamsPanel } from "../render.js";
import { nodeParamsPanelElement } from "../state.js";
import { nodeParamKnobs, paramsPanelInteractions } from "./state.js";

/** A node of the active preset, addressed the way its param edits are. A node
 * inside a composite being edited is not in any running graph, so it has none. */
function nodeSpectrumSource(nodeId: string): EqSpectrumSource | null {
  if (uiState.compositeEditMode) {
    return null;
  }
  const presetId = uiState.activePresetId ?? undefined;
  return { scope: "preset", nodeId, ...(presetId ? { presetId } : {}) };
}

/** The watcher for this panel's curve canvas, created on first use. The panel
 * tears it down with the curve whenever it rebuilds. */
function ensureSpectrumWatcher(canvas: HTMLCanvasElement, nodeId: string, onSpectrum: EqSpectrumListener): EqSpectrumWatcher {
  const existing = paramsPanelInteractions.eqSpectrum;
  if (existing?.element === canvas) {
    return existing;
  }
  existing?.destroy();
  const watcher = new EqSpectrumWatcher(canvas, () => nodeSpectrumSource(nodeId), onSpectrum);
  paramsPanelInteractions.eqSpectrum = watcher;
  return watcher;
}

/**
 * The graphic EQ's sliders are spaced evenly, not by frequency, so its plot is
 * pinned to where each slider actually sits: the spectrum and the curve then
 * line up with the slider for each band. Measured on every draw, since the
 * sliders move with the panel's width. Null falls back to the plain log axis.
 */
function graphicEqSliderAxis(canvas: HTMLCanvasElement, node: GraphNode): EqFrequencyAxis | null {
  const canvasRect = canvas.getBoundingClientRect();
  const section = canvas.closest(".graphic-eq-controls");
  if (!section || canvasRect.width <= 0) {
    return null;
  }
  const scale = canvas.width / canvasRect.width;
  const anchors = Array.from(section.querySelectorAll<HTMLElement>(".graphic-eq-band")).flatMap((band) => {
    const number = Number(band.dataset.bandNumber);
    const slider = band.querySelector<HTMLElement>(".graphic-eq-gain")?.getBoundingClientRect();
    if (!slider || slider.width <= 0 || !Number.isInteger(number) || number < 1) {
      return [];
    }
    const freq = node.params[`band${number}Freq`] ?? GRAPHIC_EQ_FREQUENCIES[number - 1];
    return [{ freq, x: (slider.left + slider.width / 2 - canvasRect.left) * scale }];
  });
  return anchoredFrequencyAxis(anchors);
}

export function updateEqVisualization(node: GraphNode): void {
  const typeInfo = getNodeEffectInfo(node);
  if (!typeInfo || (typeInfo.category !== "eq" && !node.type.startsWith("eq_"))) {
    return;
  }

  const isGraphicEqNode = EffectTypeRegistry.resolve(node.type) === EffectGuids.kEqGraphic;
  const canvasSelector = isGraphicEqNode ? ".graphic-eq-curve-canvas" : ".eq-curve-canvas";
  const canvas = nodeParamsPanelElement?.querySelector(canvasSelector) as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const width = Math.max(1, canvas.clientWidth);
  const height = Math.max(1, canvas.clientHeight);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }

  const bandConfigs = isGraphicEqNode
    ? buildGraphicEqBandConfigs(node.params ?? {})
    : buildEqBandConfigsFromParams(node.params ?? {});

  if (isGraphicEqNode) {
    const watcher = ensureSpectrumWatcher(canvas, node.id, (spectrum) => {
      drawEqCurve(canvas, buildGraphicEqBandConfigs(node.params ?? {}), spectrum, graphicEqSliderAxis(canvas, node));
    });
    drawEqCurve(canvas, bandConfigs, watcher.spectrum, graphicEqSliderAxis(canvas, node));
    return;
  }

  if (paramsPanelInteractions.eq) {
    // Update existing interaction in place
    paramsPanelInteractions.eq.updateBands(bandConfigs);
  } else {
    // Create new interactive curve
    const preset = getActivePresetForRender();
    paramsPanelInteractions.eq = new EqCurveInteraction(
      canvas,
      bandConfigs,
      (bandIndex, freq, gainDb, q) => {
        // Lightweight onChange: update params, send to plugin, and sync knobs live
        const changed = eqBandChangeToParams(bandIndex, freq, gainDb, q);
        for (const [key, value] of Object.entries(changed)) {
          node.params[key] = value;
          sendSignalPathNodeParamUpdate(node.id, key, value);
          // Sync corresponding knob display
          const knob = nodeParamKnobs.get(key);
          if (knob) knob.setValue(value);
        }
      },
      (bandIndex, freq, gainDb, q) => {
        // onCommit: full update including panel rebuild for knob display sync
        const changed = eqBandChangeToParams(bandIndex, freq, gainDb, q);
        for (const [key, value] of Object.entries(changed)) {
          node.params[key] = value;
          sendSignalPathNodeParamUpdate(node.id, key, value);
        }

        if (preset) {
          requestNodeParamsPanel(node, preset);
        }
      }
    );
    const watcher = ensureSpectrumWatcher(canvas, node.id, (spectrum) => paramsPanelInteractions.eq?.setSpectrum(spectrum));
    paramsPanelInteractions.eq.setSpectrum(watcher.spectrum);
  }
}

export function bindGraphicEqControls(node: GraphNode, preset: Preset): void {  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kEqGraphic) {
    return;
  }

  const applyParams = (updates: Record<string, number>, rerender = false): void => {
    Object.entries(updates).forEach(([key, value]) => {
      node.params[key] = value;
      sendSignalPathNodeParamUpdate(node.id, key, value);
    });
    if (rerender) {
      requestNodeParamsPanel(node, preset);
    } else {
      updateEqVisualization(node);
    }
  };

  // Reset flattens the curve: every band gain back to 0 dB. The selected profile's
  // band count, frequencies and Q values are deliberately left alone — this resets
  // the curve the user drew, not the profile they chose.
  const resetButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".graphic-eq-reset-btn");
  resetButton?.addEventListener("click", () => {
    const bandCount = node.params.bandCount ?? GRAPHIC_EQ_FREQUENCIES.length;
    const flatParams: Record<string, number> = {};
    for (let band = 1; band <= bandCount; band++) {
      flatParams[`band${band}Gain`] = 0;
    }
    applyParams(flatParams, true);
  });

  nodeParamsPanelElement?.querySelectorAll<HTMLInputElement>(".graphic-eq-gain, .graphic-eq-gain-value-input, .graphic-eq-frequency").forEach((input) => {
    input.addEventListener("input", () => {
      const key = input.dataset.paramKey;
      if (input.value.trim() === "") return;
      const rawValue = Number(input.value);
      if (!key || !Number.isFinite(rawValue)) return;
      const bandMatch = /^band(\d+)Freq$/.exec(key);
      const value = bandMatch
        ? clampGraphicEqFrequency(node.params, Number(bandMatch[1]), rawValue)
        : Math.max(-18, Math.min(18, rawValue));
      if (value !== rawValue) input.value = bandMatch ? `${Math.round(value)}` : value.toFixed(1);
      applyParams({ [key]: value });
      if (input.classList.contains("graphic-eq-gain") || input.classList.contains("graphic-eq-gain-value-input")) {
        const band = input.closest(".graphic-eq-band");
        const gainSlider = band?.querySelector<HTMLInputElement>(".graphic-eq-gain");
        const gainValueInput = band?.querySelector<HTMLInputElement>(".graphic-eq-gain-value-input");
        if (gainSlider) {
          gainSlider.value = `${value}`;
          gainSlider.style.setProperty("--graphic-eq-gain", `${((value + 18) / 36) * 100}%`);
        }
        if (gainValueInput && gainValueInput !== input) gainValueInput.value = value.toFixed(1);
      }
    });
    input.addEventListener("change", () => {
      if (input.classList.contains("graphic-eq-frequency")) requestNodeParamsPanel(node, preset);
    });
  });

  nodeParamsPanelElement?.querySelectorAll<HTMLInputElement>(".graphic-eq-gain").forEach((input) => {
    let lastTouchTapAt = 0;
    let touchStart: { pointerId: number; x: number; y: number } | undefined;
    const resetGain = (): void => {
      input.value = "0";
      input.style.setProperty("--graphic-eq-gain", "50%");
      const gainValueInput = input.closest(".graphic-eq-band")?.querySelector<HTMLInputElement>(".graphic-eq-gain-value-input");
      if (gainValueInput) gainValueInput.value = "0.0";
      const key = input.dataset.paramKey;
      if (key) applyParams({ [key]: 0 });
    };

    input.addEventListener("dblclick", resetGain);
    input.addEventListener("pointerdown", (event) => {
      if (event.pointerType === "touch") {
        touchStart = { pointerId: event.pointerId, x: event.clientX, y: event.clientY };
      }
    });
    input.addEventListener("pointerup", (event) => {
      const startedAsTap = event.pointerType === "touch"
        && touchStart?.pointerId === event.pointerId
        && Math.hypot(event.clientX - touchStart.x, event.clientY - touchStart.y) <= 8;
      touchStart = undefined;
      if (!startedAsTap) return;
      const now = performance.now();
      if (now - lastTouchTapAt <= 350) {
        lastTouchTapAt = 0;
        resetGain();
        return;
      }
      lastTouchTapAt = now;
    });
  });
}
