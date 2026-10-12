/**
 * Pickup Match's sections of the params panel:
 *
 * - The conversion it applies, drawn over the live spectrum of its input. The curve is the
 *   engine's own (effectResponse.ts), so it includes the level change when Level follows the
 *   pickups, the Amount, and the Max Treble Boost cap.
 * - A design block per side, shown while that side is set to Custom Design. It holds the side's
 *   construction controls (type, resistance, gauge, magnet, cover, wiring, capacitance trim)
 *   and what the engine predicts it is built as. The figures come with the curve's reply
 *   (EffectProcessor::DescribeSettings), so the panel never keeps a second copy of the model.
 */

import { EffectGuids } from "../../effectGuids.js";
import { requestEffectResponse, responseDbAt, type ResponseCurve } from "../../effectResponse.js";
import { drawResponsePlot } from "../../eqPlot.js";
import { enumLabelIndex } from "../../paramLabels.js";
import { EffectTypeRegistry, type ParameterDef } from "../../presetV2.js";
import type { GraphNode } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { nodeParamsPanelElement } from "../state.js";
import { ensureNodeSpectrumWatcher, fitCanvasToLayout } from "./nodeSpectrum.js";
import { paramsPanelInteractions } from "./state.js";

/** Measured pickups differ in output by up to ~25 dB, and the treble boost is capped at 24. */
const CURVE_MIN_DB = -24;
const CURVE_MAX_DB = 24;
const CURVE_GRID_DB = 12;

type Side = "source" | "target";
const SIDES: readonly Side[] = ["source", "target"];

/** A side's design parameters are its name and one of these (PickupMatchEffect.h). */
const DESIGN_SUFFIXES = ["DesignType", "Resistance", "Gauge", "Magnet", "Cover", "Wiring", "CapacitanceTrim"] as const;

/** Every design parameter: the panel draws them in the design blocks, not with the rest. */
export const PICKUP_DESIGN_PARAM_KEYS: readonly string[] =
  SIDES.flatMap((side) => DESIGN_SUFFIXES.map((suffix) => `${side}${suffix}`));

/** The picker group the engine files its Custom Design choice under. */
const CUSTOM_GROUP = "Custom";

let shownCurve: { nodeId: string; curve: ResponseCurve } | null = null;

export function isPickupMatchNode(node: GraphNode): boolean {
  return EffectTypeRegistry.resolve(node.type) === EffectGuids.kPickupMatch;
}

/** The section's markup, or "" for any other effect. */
export function buildPickupResponseSectionHtml(node: GraphNode): string {
  if (!isPickupMatchNode(node)) {
    return "";
  }
  return `
    <section class="pickup-response" data-node-id="${escapeHtml(node.id)}">
      <div class="eq-visualizer pickup-response-visualizer">
        <div class="eq-visualizer-header">
          <span>Pickup Conversion</span>
          <span class="eq-visualizer-range">±${CURVE_MAX_DB} dB</span>
        </div>
        <canvas class="eq-curve-canvas pickup-response-canvas" role="img"
          aria-label="What the conversion does at each frequency"></canvas>
      </div>
    </section>
  `;
}

/** Whether `side` is set to Custom Design on `node`. */
export function isCustomSide(node: GraphNode, defs: readonly ParameterDef[], side: Side): boolean {
  const def = defs.find((candidate) => candidate.key === side);
  if (!def?.labels || !def.labelInfo) {
    return false;
  }
  const index = enumLabelIndex(node.params[side] ?? def.default, def.labels.length, def.min, def.step);
  return def.labelInfo[index]?.group === CUSTOM_GROUP;
}

/**
 * The two design blocks, each hidden until its side is Custom Design. `renderControls` is the
 * panel's own control builder, so the controls carry the node's values and bind like any other.
 */
export function buildPickupDesignSectionHtml(
  node: GraphNode,
  defs: readonly ParameterDef[],
  renderControls: (defs: ParameterDef[]) => string,
): string {
  if (!isPickupMatchNode(node)) {
    return "";
  }
  return SIDES.map((side) => {
    const sideDefs = DESIGN_SUFFIXES
      .map((suffix) => defs.find((def) => def.key === `${side}${suffix}`))
      .filter((def): def is ParameterDef => def !== undefined)
      // Their own block says they are a design; the Advanced tab does not hold them.
      .map((def) => ({ ...def, advanced: false }));
    if (sideDefs.length === 0) {
      return "";
    }
    return `
      <section class="pickup-design" data-node-id="${escapeHtml(node.id)}" data-side="${side}"
        ${isCustomSide(node, defs, side) ? "" : "hidden"}>
        <div class="params-controls">${renderControls(sideDefs)}</div>
        <p class="pickup-design-summary" data-side="${side}" role="status" aria-live="polite"></p>
        <p class="pickup-design-note">Predicted from construction, using fits to 397 measured pickups: the peak to
          about a semitone and its height to about 0.7 dB. Magnet grade changes inductance only. Capacitance Trim
          stands in for insulation and wind pattern, which measurements cannot predict. The brass cover comes from two
          measured pickups. Coil wiring applies to humbuckers and assumes matched coils.</p>
      </section>
    `;
  }).join("");
}

function formatNumber(value: number, digits: number): string {
  return value.toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

/** "1,191 m of wire · 2.66 H · 6.50 kΩ · 118 pF → peak 3.40 kHz, Q 1.42, output +0.4 dB". */
export function describeDesign(summary: Readonly<Record<string, number>>, side: Side): string {
  const get = (name: string): number | undefined => summary[`${side}${name}`];
  const length = get("WireLengthM");
  const inductance = get("InductanceH");
  const resistance = get("ResistanceKohm");
  const capacitance = get("CapacitancePf");
  const peak = get("PeakHz");
  const q = get("Q");
  const level = get("LevelDb");
  if ([length, inductance, resistance, capacitance, peak, q, level].some((value) => value === undefined)) {
    return "";
  }
  const signed = (value: number): string => `${value >= 0 ? "+" : "−"}${formatNumber(Math.abs(value), 1)}`;
  return `${formatNumber(length!, 0)} m of wire · ${formatNumber(inductance!, 2)} H · `
    + `${formatNumber(resistance!, 2)} kΩ · ${formatNumber(capacitance!, 0)} pF → `
    + `peak ${formatNumber(peak! / 1000, 2)} kHz, Q ${formatNumber(q!, 2)}, output ${signed(level!)} dB`;
}

/** Where the sections are drawn: the params panel, unless a caller hosts them elsewhere (the
 * global Pickup Match tab in the input calibration flyout). */
type Root = ParentNode | null | undefined;

function curveCanvas(root: Root): HTMLCanvasElement | null {
  return root?.querySelector<HTMLCanvasElement>(".pickup-response-canvas") ?? null;
}

function drawCurve(canvas: HTMLCanvasElement, nodeId: string, withSpectrum: boolean): void {
  const curve = shownCurve?.nodeId === nodeId ? shownCurve.curve : null;
  drawResponsePlot(canvas, curve ? (freq) => responseDbAt(curve, freq) : () => Number.NaN, {
    spectrum: withSpectrum ? paramsPanelInteractions.eqSpectrum?.spectrum ?? null : null,
    minDb: CURVE_MIN_DB,
    maxDb: CURVE_MAX_DB,
    gridStepDb: CURVE_GRID_DB,
  });
}

/** Shows the design block of each side set to Custom Design, with its figures when known. */
function updateDesignBlocks(node: GraphNode, root: Root): void {
  const defs = EffectTypeRegistry.get(node.type)?.parameters ?? [];
  const summary = shownCurve?.nodeId === node.id ? shownCurve.curve.summary : undefined;
  for (const side of SIDES) {
    const block = root?.querySelector<HTMLElement>(`.pickup-design[data-side="${side}"]`);
    if (!block || block.dataset.nodeId !== node.id) {
      continue;
    }
    block.hidden = !isCustomSide(node, defs, side);
    const line = block.querySelector<HTMLElement>(".pickup-design-summary");
    if (line) {
      line.textContent = summary ? describeDesign(summary, side) : "";
    }
  }
}

/** Redraws the curve straight away with the last one, then again when the engine answers. */
export function updatePickupResponseVisualization(node: GraphNode, root: Root = nodeParamsPanelElement): void {
  const canvas = isPickupMatchNode(node) ? curveCanvas(root) : null;
  if (!canvas) {
    return;
  }
  fitCanvasToLayout(canvas);
  // The spectrum is the params panel's selected node's; a hosted curve draws without it.
  const inPanel = root === nodeParamsPanelElement;
  if (inPanel) {
    ensureNodeSpectrumWatcher(canvas, node.id, () => drawCurve(canvas, node.id, true));
  }
  drawCurve(canvas, node.id, inPanel);
  updateDesignBlocks(node, root);

  requestEffectResponse(node.type, node.params ?? {}, (curve) => {
    if (curve) {
      shownCurve = { nodeId: node.id, curve };
    }
    // The panel may have been rebuilt, or moved to another node, while the request was out.
    const current = curveCanvas(root);
    if (current && current.closest<HTMLElement>(".pickup-response")?.dataset.nodeId === node.id) {
      drawCurve(current, node.id, inPanel);
      updateDesignBlocks(node, root);
    }
  });
}
