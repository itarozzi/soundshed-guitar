/**
 * The live spectrum of the selected node's input, drawn behind whichever
 * response curve its panel shows: an EQ's curve or a cabinet's response.
 */

import { EqSpectrumWatcher, type EqSpectrumListener, type EqSpectrumSource } from "../../eqSpectrum.js";
import { uiState } from "../../state.js";
import { paramsPanelInteractions } from "./state.js";

/** A node of the active preset, addressed the way its param edits are. A node
 * inside a composite being edited is not in any running graph, so it has none. */
export function nodeSpectrumSource(nodeId: string): EqSpectrumSource | null {
  if (uiState.compositeEditMode) {
    return null;
  }
  const presetId = uiState.activePresetId ?? undefined;
  return { scope: "preset", nodeId, ...(presetId ? { presetId } : {}) };
}

/** The watcher for this panel's curve canvas, created on first use. The panel
 * tears it down with the curve whenever it rebuilds. */
export function ensureNodeSpectrumWatcher(
  canvas: HTMLCanvasElement,
  nodeId: string,
  onSpectrum: EqSpectrumListener,
): EqSpectrumWatcher {
  const existing = paramsPanelInteractions.eqSpectrum;
  if (existing?.element === canvas) {
    return existing;
  }
  existing?.destroy();
  const watcher = new EqSpectrumWatcher(canvas, () => nodeSpectrumSource(nodeId), onSpectrum);
  paramsPanelInteractions.eqSpectrum = watcher;
  return watcher;
}

/** Sizes a canvas's drawing buffer to its laid-out size, so a plot is drawn at
 * one pixel per pixel. */
export function fitCanvasToLayout(canvas: HTMLCanvasElement): void {
  const width = Math.max(1, canvas.clientWidth);
  const height = Math.max(1, canvas.clientHeight);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
}
