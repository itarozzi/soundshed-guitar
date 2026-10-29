/**
 * The Multi-band Compressor's section of the params panel: its four bands drawn
 * over the live spectrum of the node's input and output, with each band's gain
 * reduction and level (multibandCompressorPlot.ts). The crossover lines can be
 * dragged; a drag sets crossover1..3 the way their knobs would.
 */

import { EffectGuids } from "../../effectGuids.js";
import type { EqSpectrum } from "../../eqSpectrum.js";
import {
  MBC_BAND_COUNT,
  MBC_CROSSOVER_KEYS,
  MBC_DEFAULT_THRESHOLD_DB,
  ReductionDisplay,
  crossoverAt,
  draggedCrossover,
  drawMultibandCompressorPlot,
  mbcPlotAxis,
  orderedCrossovers,
} from "../../multibandCompressorPlot.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import type { GraphNode } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";
import { ensureNodeSpectrumWatcher, fitCanvasToLayout } from "./nodeSpectrum.js";
import { nodeParamKnobs, paramsPanelInteractions } from "./state.js";

/** The gain reduction as drawn, for the node the panel shows. */
const reductionDisplay = new ReductionDisplay();
let displayNodeId = "";
/** The crossover being dragged, or hovered, or -1. */
let activeCrossover = -1;
/** The canvas whose pointer handlers are bound, so a redraw does not bind them twice, and the
 * node they edit: the panel can be handed a fresh copy of the node without being rebuilt. */
let boundCanvas: HTMLCanvasElement | null = null;
let boundNode: GraphNode | null = null;

export function isMultibandCompressorNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kMultibandCompressor;
}

/** The section's markup, or "" for any other effect. */
export function buildMultibandCompressorSectionHtml(node: GraphNode): string {
  if (!isMultibandCompressorNode(node)) {
    return "";
  }
  return `
    <section class="mbc-section" data-node-id="${escapeHtml(node.id)}">
      <div class="eq-visualizer mbc-visualizer">
        <div class="eq-visualizer-header">
          <span>Bands</span>
          <span class="eq-visualizer-range mbc-legend">
            <span class="mbc-legend-in">In</span><span class="mbc-legend-out">Out</span><span class="mbc-legend-reduction">Reduction</span>
          </span>
        </div>
        <canvas class="eq-curve-canvas mbc-canvas" role="img"
          aria-label="Spectrum in and out, with each band's gain reduction and level; drag a crossover to move it"></canvas>
      </div>
    </section>
  `;
}

function plotCanvas(nodeId: string): HTMLCanvasElement | null {
  const section = nodeParamsPanelElement?.querySelector<HTMLElement>(".mbc-section");
  return section?.dataset.nodeId === nodeId ? section.querySelector<HTMLCanvasElement>(".mbc-canvas") : null;
}

function thresholds(node: GraphNode): number[] {
  return Array.from({ length: MBC_BAND_COUNT }, (_, band) => {
    const value = node.params[`threshold${band + 1}`];
    return typeof value === "number" && Number.isFinite(value) ? value : MBC_DEFAULT_THRESHOLD_DB;
  });
}

function draw(canvas: HTMLCanvasElement, node: GraphNode, spectrum: EqSpectrum | null): void {
  drawMultibandCompressorPlot(canvas, {
    spectrum,
    crossovers: orderedCrossovers(node.params),
    thresholdsDb: thresholds(node),
    reductionDb: reductionDisplay.update(spectrum?.bands, performance.now()),
    activeCrossover,
  });
}

/** Canvas x for a pointer event, in drawing-buffer pixels: the panel may be zoomed. */
function canvasX(canvas: HTMLCanvasElement, event: PointerEvent): number {
  const rect = canvas.getBoundingClientRect();
  return rect.width > 0 ? (event.clientX - rect.left) * (canvas.width / rect.width) : 0;
}

function setCrossover(node: GraphNode, index: number, value: number): void {
  const key = MBC_CROSSOVER_KEYS[index];
  if (node.params[key] === value) {
    return;
  }
  node.params[key] = value;
  sendSignalPathNodeParamUpdate(node.id, key, value);
  nodeParamKnobs.get(key)?.setValue(value);
}

function bindDrag(canvas: HTMLCanvasElement, node: GraphNode): void {
  boundNode = node;
  if (boundCanvas === canvas) {
    return;
  }
  boundCanvas = canvas;
  let dragging = -1;
  const params = (): Record<string, number> => boundNode?.params ?? {};
  const redraw = (): void => {
    if (boundNode) {
      draw(canvas, boundNode, paramsPanelInteractions.eqSpectrum?.spectrum ?? null);
    }
  };
  const hover = (index: number): void => {
    if (index !== activeCrossover) {
      activeCrossover = index;
      canvas.style.cursor = index >= 0 ? "ew-resize" : "";
      redraw();
    }
  };

  canvas.addEventListener("pointerdown", (event) => {
    const index = crossoverAt(canvasX(canvas, event), orderedCrossovers(params()), mbcPlotAxis(canvas.width));
    if (index < 0) {
      return;
    }
    dragging = index;
    hover(index);
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
  });
  canvas.addEventListener("pointermove", (event) => {
    const splits = orderedCrossovers(params());
    const axis = mbcPlotAxis(canvas.width);
    if (dragging < 0) {
      hover(crossoverAt(canvasX(canvas, event), splits, axis));
      return;
    }
    if (!boundNode) {
      return;
    }
    setCrossover(boundNode, dragging, draggedCrossover(dragging, axis.toFreq(canvasX(canvas, event)), splits));
    redraw();
  });
  const endDrag = (event: PointerEvent): void => {
    if (dragging < 0) {
      return;
    }
    dragging = -1;
    if (canvas.hasPointerCapture(event.pointerId)) {
      canvas.releasePointerCapture(event.pointerId);
    }
    hover(crossoverAt(canvasX(canvas, event), orderedCrossovers(params()), mbcPlotAxis(canvas.width)));
  };
  canvas.addEventListener("pointerup", endDrag);
  canvas.addEventListener("pointercancel", endDrag);
  canvas.addEventListener("pointerleave", () => {
    if (dragging < 0) {
      hover(-1);
    }
  });
}

/** Draws the plot for the node's current parameters and keeps it fed with the
 * live spectrum while it is on screen. */
export function updateMultibandCompressorVisualization(node: GraphNode): void {
  const canvas = isMultibandCompressorNode(node) ? plotCanvas(node.id) : null;
  if (!canvas) {
    return;
  }
  if (displayNodeId !== node.id) {
    displayNodeId = node.id;
    reductionDisplay.reset();
    activeCrossover = -1;
  }
  fitCanvasToLayout(canvas);
  bindDrag(canvas, node);
  const watcher = ensureNodeSpectrumWatcher(canvas, node.id, (spectrum) => draw(canvas, boundNode ?? node, spectrum));
  draw(canvas, node, watcher.spectrum);
}
