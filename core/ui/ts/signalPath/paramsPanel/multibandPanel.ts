/**
 * The multi-band effects' section of the params panel (the Multi-band Compressor
 * and Distortion): their four bands drawn over the live spectrum of the node's
 * input and output, with how hard each band works and its level
 * (multibandPlot.ts), dimmed where a band is off or another is soloed. The
 * crossover lines can be dragged; a drag sets
 * crossover1..3 the way their knobs would. The two effects share bands and
 * crossovers; MULTIBAND_KINDS says what differs.
 */

import { EffectGuids } from "../../effectGuids.js";
import type { EqSpectrum, EqSpectrumBand } from "../../eqSpectrum.js";
import {
  MB_BAND_COUNT,
  MB_CROSSOVER_KEYS,
  MB_DEFAULT_THRESHOLD_DB,
  DepthDisplay,
  bandStates,
  crossoverAt,
  draggedCrossover,
  drawMultibandPlot,
  multibandPlotAxis,
  orderedCrossovers,
} from "../../multibandPlot.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import type { GraphNode } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";
import { ensureNodeSpectrumWatcher, fitCanvasToLayout } from "./nodeSpectrum.js";
import { nodeParamKnobs, paramsPanelInteractions } from "./state.js";

/** How hard each band works, as drawn, for the node the panel shows. */
const depthDisplay = new DepthDisplay();
let displayNodeId = "";
/** The crossover being dragged, or hovered, or -1. */
let activeCrossover = -1;
/** The canvas whose pointer handlers are bound, so a redraw does not bind them twice, and the
 * node they edit: the panel can be handed a fresh copy of the node without being rebuilt. */
let boundCanvas: HTMLCanvasElement | null = null;
let boundNode: GraphNode | null = null;

/** What one multi-band effect's plot shows that the other's does not. */
export interface MultibandKind {
  /** Added to the section, to colour its plot (css/multiband.css). */
  sectionClass: string;
  /** The legend's name for what hangs from the top of each band. */
  depthLegend: string;
  /** Before that depth's number. */
  depthSign: string;
  ariaLabel: string;
  /** Per band, the input level where its curve starts to act. */
  thresholds(params: Record<string, number | undefined>, bands: readonly EqSpectrumBand[] | undefined): number[];
  /** Per band, the knee's width around that level, for an effect that has one. */
  knees?: (params: Record<string, number | undefined>) => number[];
}

/** The compressor's Knee default (MultibandCompressorSupport.h). */
const COMPRESSOR_DEFAULT_KNEE_DB = 6;
/** The nominal operating level the engine assumes until told otherwise (LevelTargets.h). */
const DEFAULT_NOMINAL_DBFS = -18;
/** The distortion's knee over the nominal level (kKneeAboveNominalDb). */
const DISTORTION_KNEE_ABOVE_NOMINAL_DB = 12;
/** The distortion's Drive defaults (MultibandDistortionSupport.h). */
const DISTORTION_DEFAULT_DRIVE_DB = [18, 28, 36, 30] as const;

function bandParam(params: Record<string, number | undefined>, key: string, fallback: number): number {
  const value = params[key];
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

export const MULTIBAND_KINDS: Readonly<Record<string, MultibandKind>> = {
  [EffectGuids.kMultibandCompressor]: {
    sectionClass: "mb-compressor",
    depthLegend: "Reduction",
    depthSign: "−",
    ariaLabel: "Spectrum in and out, with each band's gain reduction and level; drag a crossover to move it",
    // From the knobs, so the mark follows a turn at once.
    thresholds: (params) =>
      Array.from({ length: MB_BAND_COUNT }, (_, band) => bandParam(params, `threshold${band + 1}`, MB_DEFAULT_THRESHOLD_DB)),
    knees: (params) =>
      Array.from({ length: MB_BAND_COUNT }, (_, band) => bandParam(params, `knee${band + 1}`, COMPRESSOR_DEFAULT_KNEE_DB)),
  },
  [EffectGuids.kMultibandDistortion]: {
    sectionClass: "mb-distortion",
    depthLegend: "Drive",
    depthSign: "+",
    ariaLabel: "Spectrum in and out, with how hard each band is driven and its level; drag a crossover to move it",
    // The engine's, which knows the nominal level; worked out from Drive until the first frame.
    thresholds: (params, bands) =>
      Array.from({ length: MB_BAND_COUNT }, (_, band) => {
        const sent = bands?.[band]?.thresholdDb;
        if (typeof sent === "number") {
          return sent;
        }
        const drive = bandParam(params, `drive${band + 1}`, DISTORTION_DEFAULT_DRIVE_DB[band]);
        return DEFAULT_NOMINAL_DBFS + DISTORTION_KNEE_ABOVE_NOMINAL_DB - drive;
      }),
  },
};

/** The node's multi-band description, or null for any other effect. */
export function multibandKind(node: GraphNode): MultibandKind | null {
  return MULTIBAND_KINDS[EffectTypeRegistry.resolve(node.type)] ?? null;
}

/** The section's markup, or "" for any other effect. */
export function buildMultibandSectionHtml(node: GraphNode): string {
  const kind = multibandKind(node);
  if (!kind) {
    return "";
  }
  return `
    <section class="mb-section ${escapeHtml(kind.sectionClass)}" data-node-id="${escapeHtml(node.id)}">
      <div class="eq-visualizer mb-visualizer">
        <div class="eq-visualizer-header">
          <span>Bands</span>
          <span class="eq-visualizer-range mb-legend">
            <span class="mb-legend-in">In</span><span class="mb-legend-out">Out</span><span class="mb-legend-depth">${escapeHtml(kind.depthLegend)}</span>
          </span>
        </div>
        <canvas class="eq-curve-canvas mb-canvas" role="img"
          aria-label="${escapeHtml(kind.ariaLabel)}"></canvas>
      </div>
    </section>
  `;
}

function plotCanvas(nodeId: string): HTMLCanvasElement | null {
  const section = nodeParamsPanelElement?.querySelector<HTMLElement>(".mb-section");
  return section?.dataset.nodeId === nodeId ? section.querySelector<HTMLCanvasElement>(".mb-canvas") : null;
}

function draw(canvas: HTMLCanvasElement, node: GraphNode, spectrum: EqSpectrum | null): void {
  const kind = multibandKind(node);
  if (!kind) {
    return;
  }
  drawMultibandPlot(canvas, {
    spectrum,
    crossovers: orderedCrossovers(node.params),
    thresholdsDb: kind.thresholds(node.params, spectrum?.bands),
    kneesDb: kind.knees?.(node.params),
    bandStates: bandStates(node.params),
    depthDb: depthDisplay.update(spectrum?.bands, performance.now()),
    depthSign: kind.depthSign,
    activeCrossover,
  });
}

/** Canvas x for a pointer event, in drawing-buffer pixels: the panel may be zoomed. */
function canvasX(canvas: HTMLCanvasElement, event: PointerEvent): number {
  const rect = canvas.getBoundingClientRect();
  return rect.width > 0 ? (event.clientX - rect.left) * (canvas.width / rect.width) : 0;
}

function setCrossover(node: GraphNode, index: number, value: number): void {
  const key = MB_CROSSOVER_KEYS[index];
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
    const index = crossoverAt(canvasX(canvas, event), orderedCrossovers(params()), multibandPlotAxis(canvas.width));
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
    const axis = multibandPlotAxis(canvas.width);
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
    hover(crossoverAt(canvasX(canvas, event), orderedCrossovers(params()), multibandPlotAxis(canvas.width)));
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
export function updateMultibandVisualization(node: GraphNode): void {
  const canvas = multibandKind(node) ? plotCanvas(node.id) : null;
  if (!canvas) {
    return;
  }
  if (displayNodeId !== node.id) {
    displayNodeId = node.id;
    depthDisplay.reset();
    activeCrossover = -1;
  }
  fitCanvasToLayout(canvas);
  bindDrag(canvas, node);
  const watcher = ensureNodeSpectrumWatcher(canvas, node.id, (spectrum) => draw(canvas, boundNode ?? node, spectrum));
  draw(canvas, node, watcher.spectrum);
}
