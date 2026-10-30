/**
 * The multi-band effects' plot (the Multi-band Compressor and Distortion): their
 * four bands over the live spectrum, so the player can see what each band is
 * doing and how hard.
 *
 *   - the node's input spectrum, shaded, as behind an EQ curve
 *   - its output spectrum, as a line over it: the gap in a band is what that
 *     band is losing, or (for the distortion) the harmonics it is adding
 *   - a column per band, divided at the crossovers, with how hard the band is
 *     working hanging from the top and its number: the compressor's gain
 *     reduction, the distortion's drive past its clipper's knee
 *   - under the plot, each band's level against where its curve starts to act,
 *     with the compressor's knee as a shaded span around that mark
 *   - a band turned off, or silenced by another band's solo, dimmed
 *
 * The engine sends the output bins and the band meters with the spectrum
 * ("sldS" o and b, eqSpectrum.ts). This file only draws and does the
 * crossover maths; signalPath/paramsPanel/multibandPanel.ts owns the
 * canvas, the feed and the drags.
 */

import { drawSpectrum, getEqCurveThemeColors, logFrequencyAxis, type EqFrequencyAxis } from "./eqPlot.js";
import type { EqSpectrum, EqSpectrumBand } from "./eqSpectrum.js";

export const MB_BAND_COUNT = 4;
export const MB_BAND_LABELS = ["Low", "Low Mid", "High Mid", "High"] as const;
export const MB_CROSSOVER_KEYS = ["crossover1", "crossover2", "crossover3"] as const;

/** The parameter ranges and defaults, as MultibandCompressorSupport.h and
 * MultibandDistortionSupport.h both declare them. */
export const MB_CROSSOVER_RANGES = [
  { min: 40, max: 1000, default: 200 },
  { min: 150, max: 5000, default: 800 },
  { min: 600, max: 12000, default: 3000 },
] as const;
export const MB_DEFAULT_THRESHOLD_DB = -24;

/** A crossover is kept at least a third of an octave above the one below it
 * (kMinSplitRatio in LinkwitzRileyCrossover.h). */
export const MB_MIN_SPLIT_RATIO = Math.cbrt(2);

/** How hard a band works, from nothing to the most the plot shows, top down. */
export const MB_DEPTH_SCALE_DB = 24;
/** The level strip's range, dBFS. */
export const MB_LEVEL_MIN_DB = -60;
export const MB_LEVEL_MAX_DB = 0;

/** How fast a drawn depth falls back once a band lets go, dB per second. */
const REDUCTION_FALL_DB_PER_SECOND = 30;
const PADDING = 8;
const STRIP_HEIGHT = 16;
const STRIP_GAP = 6;
/** How close, in canvas pixels, a pointer has to be to grab a crossover. */
export const MB_GRAB_DISTANCE_PX = 6;

/** The crossovers the engine will use for `params`: each pushed a third of an
 * octave above the one below (OrderedSplits, without the Nyquist guard, which
 * only matters at sample rates below 27 kHz). */
export function orderedCrossovers(params: Record<string, number | undefined>): number[] {
  const splits = MB_CROSSOVER_KEYS.map((key, index) => {
    const range = MB_CROSSOVER_RANGES[index];
    const value = params[key];
    return Math.min(range.max, Math.max(range.min, typeof value === "number" && Number.isFinite(value) ? value : range.default));
  });
  for (let i = 1; i < splits.length; i += 1) {
    splits[i] = Math.max(splits[i], splits[i - 1] * MB_MIN_SPLIT_RATIO);
  }
  return splits;
}

/** Where crossover `index` lands when dragged to `freq`: inside its own range
 * and a third of an octave clear of its neighbours, so a drag never pushes
 * another crossover along with it. */
export function draggedCrossover(index: number, freq: number, splits: readonly number[]): number {
  const range = MB_CROSSOVER_RANGES[index];
  let low: number = range.min;
  let high: number = range.max;
  if (index > 0) {
    low = Math.max(low, splits[index - 1] * MB_MIN_SPLIT_RATIO);
  }
  if (index + 1 < splits.length) {
    high = Math.min(high, splits[index + 1] / MB_MIN_SPLIT_RATIO);
  }
  const clamped = Math.min(Math.max(freq, low), Math.max(low, high));
  // Whole hertz below 1 kHz, tens above, as the knob shows them.
  return clamped < 1000 ? Math.round(clamped) : Math.round(clamped / 10) * 10;
}

/** The crossover a pointer at canvas `x` would grab, or -1. */
export function crossoverAt(x: number, splits: readonly number[], axis: EqFrequencyAxis): number {
  let best = -1;
  let bestDistance = MB_GRAB_DISTANCE_PX;
  splits.forEach((freq, index) => {
    const distance = Math.abs(axis.toX(freq) - x);
    if (distance <= bestDistance) {
      best = index;
      bestDistance = distance;
    }
  });
  return best;
}

/** The plot's frequency axis on a canvas of `width`: the EQ's, inside the padding. */
export function multibandPlotAxis(width: number): EqFrequencyAxis {
  return logFrequencyAxis(PADDING, Math.max(1, width - PADDING * 2));
}

/** How hard each band works, as drawn: a new peak shows at once, and a band letting go
 * falls back at a steady rate rather than flickering frame to frame. */
export class DepthDisplay {
  private readonly shown: number[] = new Array<number>(MB_BAND_COUNT).fill(0);
  private lastAt = 0;

  update(bands: readonly EqSpectrumBand[] | undefined, now: number): readonly number[] {
    const elapsed = this.lastAt > 0 ? Math.min(0.5, (now - this.lastAt) / 1000) : 0;
    this.lastAt = now;
    for (let band = 0; band < MB_BAND_COUNT; band += 1) {
      const fallen = Math.max(0, this.shown[band] - REDUCTION_FALL_DB_PER_SECOND * elapsed);
      this.shown[band] = Math.max(bands?.[band]?.gainReductionDb ?? 0, fallen);
    }
    return this.shown;
  }

  reset(): void {
    this.shown.fill(0);
    this.lastAt = 0;
  }
}

/** A band's On and Solo, as the plot shows them. */
export interface MultibandBandState {
  /** Off, it passes untouched: nothing hangs from the top, and its level is never "over". */
  on: boolean;
  soloed: boolean;
  /** False while another band is soloed and this one is not: drawn dimmed. */
  heard: boolean;
}

function flag(params: Record<string, number | undefined>, key: string, fallback: boolean): boolean {
  const value = params[key];
  return typeof value === "number" && Number.isFinite(value) ? value >= 0.5 : fallback;
}

/** Each band's state from on1..4 and solo1..4; an effect without them has every band on and heard. */
export function bandStates(params: Record<string, number | undefined>): MultibandBandState[] {
  const soloed = Array.from({ length: MB_BAND_COUNT }, (_, band) => flag(params, `solo${band + 1}`, false));
  const anySolo = soloed.some(Boolean);
  return soloed.map((solo, band) => ({
    on: flag(params, `on${band + 1}`, true),
    soloed: solo,
    heard: !anySolo || solo,
  }));
}

export interface MultibandPlotState {
  spectrum: EqSpectrum | null;
  /** The crossovers to draw (orderedCrossovers). */
  crossovers: readonly number[];
  /** Per band, the input level where its curve starts to act: the strip's mark. */
  thresholdsDb: readonly number[];
  /** Per band, the knee's width centred on that mark, shaded on the strip; none when absent. */
  kneesDb?: readonly number[];
  /** Per band, On and Solo (bandStates); every band on and heard when absent. */
  bandStates?: readonly MultibandBandState[];
  /** How hard each band works, as DepthDisplay gives it. */
  depthDb: readonly number[];
  /** Before the depth's number: "−" for a cut, "+" for a drive. */
  depthSign: string;
  /** The crossover being dragged or hovered, or -1. */
  activeCrossover: number;
}

function themeColor(styles: CSSStyleDeclaration, name: string, fallback: string): string {
  return styles.getPropertyValue(name).trim() || fallback;
}

function formatHz(freq: number): string {
  return freq >= 1000 ? `${(freq / 1000).toFixed(freq >= 10000 ? 0 : 1)}k` : `${Math.round(freq)}`;
}

/** Draws the whole plot onto `canvas`, sized to its drawing buffer. */
export function drawMultibandPlot(canvas: HTMLCanvasElement, state: MultibandPlotState): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }
  const width = canvas.width;
  const height = canvas.height;
  const plotWidth = Math.max(1, width - PADDING * 2);
  const plotHeight = Math.max(1, height - PADDING * 2 - STRIP_HEIGHT - STRIP_GAP);
  const plotBottom = PADDING + plotHeight;
  const stripTop = plotBottom + STRIP_GAP;
  const axis = multibandPlotAxis(width);
  const colors = getEqCurveThemeColors(canvas);
  const styles = window.getComputedStyle(canvas);
  const depthFill = themeColor(styles, "--mb-depth-fill", "rgba(232, 128, 72, 0.32)");
  const depthEdge = themeColor(styles, "--mb-depth-edge", "rgba(240, 150, 90, 0.95)");
  const crossoverLine = themeColor(styles, "--mb-crossover", "rgba(255, 255, 255, 0.38)");
  const crossoverActive = themeColor(styles, "--mb-crossover-active", colors.response);
  const label = themeColor(styles, "--mb-label", "rgba(240, 240, 248, 0.72)");
  const levelFill = themeColor(styles, "--mb-level", "rgba(240, 240, 248, 0.42)");
  const levelOver = themeColor(styles, "--mb-level-over", "rgba(240, 150, 90, 0.85)");
  const thresholdMark = themeColor(styles, "--mb-threshold", colors.response);
  const stripBackground = themeColor(styles, "--mb-strip-bg", "rgba(0, 0, 0, 0.28)");
  const kneeFill = themeColor(styles, "--mb-knee", "rgba(72, 168, 224, 0.3)");
  const unheardFill = themeColor(styles, "--mb-unheard", "rgba(0, 0, 0, 0.5)");
  const soloLabel = themeColor(styles, "--mb-solo", "rgba(240, 200, 80, 0.95)");

  ctx.clearRect(0, 0, width, height);

  // Grid: the EQ's frequency lines.
  ctx.strokeStyle = colors.grid;
  ctx.lineWidth = 1;
  for (const freq of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
    const x = axis.toX(freq);
    ctx.beginPath();
    ctx.moveTo(x, PADDING);
    ctx.lineTo(x, plotBottom);
    ctx.stroke();
  }

  const spectrum = state.spectrum;
  if (spectrum) {
    drawSpectrum(ctx, spectrum, axis, PADDING, plotWidth, plotHeight, colors);
    if (spectrum.outputBinsDb) {
      // The output as a line only: the shading stays the input's.
      drawSpectrum(ctx, { ...spectrum, binsDb: spectrum.outputBinsDb }, axis, PADDING, plotWidth, plotHeight, {
        ...colors,
        spectrumFillTop: "rgba(0, 0, 0, 0)",
        spectrumFillBottom: "rgba(0, 0, 0, 0)",
        spectrumLine: colors.response,
      });
    }
  }

  const edges = [PADDING, ...state.crossovers.map((freq) => axis.toX(freq)), PADDING + plotWidth];
  const levels = spectrum?.bands;
  ctx.font = "10px system-ui, sans-serif";
  ctx.textBaseline = "top";

  for (let band = 0; band < MB_BAND_COUNT; band += 1) {
    const left = edges[band];
    const right = edges[band + 1];
    const columnWidth = Math.max(0, right - left);
    const bandState = state.bandStates?.[band] ?? { on: true, soloed: false, heard: true };
    // A band turned off does nothing, whatever the meter is still falling back from.
    const depthDb = bandState.on ? Math.min(MB_DEPTH_SCALE_DB, Math.max(0, state.depthDb[band] ?? 0)) : 0;

    // How hard the band works hangs from the top, over half the plot at full scale.
    const depth = (depthDb / MB_DEPTH_SCALE_DB) * plotHeight * 0.5;
    if (depthDb > 0.05) {
      ctx.fillStyle = depthFill;
      ctx.fillRect(left + 1, PADDING, columnWidth - 2, depth);
      ctx.fillStyle = depthEdge;
      ctx.fillRect(left + 1, PADDING + depth - 1.5, columnWidth - 2, 2);
    }

    // A band silenced by another's solo is shaded over, spectrum, reduction and all: it still
    // compresses, but none of it is heard.
    if (!bandState.heard) {
      ctx.fillStyle = unheardFill;
      ctx.fillRect(left, PADDING, columnWidth, plotHeight);
    }

    // The band's name, with Solo or Off when it fits, and its depth while it is working.
    ctx.fillStyle = bandState.soloed ? soloLabel : label;
    ctx.globalAlpha = bandState.on && bandState.heard ? 1 : 0.55;
    ctx.textAlign = "center";
    const centre = left + columnWidth / 2;
    if (columnWidth > 36) {
      const name = MB_BAND_LABELS[band];
      const marked = bandState.soloed ? `${name} · Solo` : bandState.on ? name : `${name} · Off`;
      ctx.fillText(ctx.measureText(marked).width <= columnWidth - 4 ? marked : name, centre, PADDING + 3);
    }
    if (depthDb >= 0.5 && columnWidth > 30) {
      ctx.fillStyle = depthEdge;
      // Under the bar's edge, so the edge never strikes through it.
      ctx.fillText(`${state.depthSign}${depthDb.toFixed(1)} dB`, centre, Math.max(PADDING + 16, PADDING + depth + 3));
    }
    ctx.globalAlpha = 1;

    // The level strip: the band's level from the left, its threshold as a mark.
    const toStripX = (db: number): number => {
      const fraction = (db - MB_LEVEL_MIN_DB) / (MB_LEVEL_MAX_DB - MB_LEVEL_MIN_DB);
      return left + 2 + Math.min(1, Math.max(0, fraction)) * Math.max(0, columnWidth - 4);
    };
    ctx.fillStyle = stripBackground;
    ctx.fillRect(left + 1, stripTop, columnWidth - 2, STRIP_HEIGHT);
    const threshold = state.thresholdsDb[band] ?? MB_DEFAULT_THRESHOLD_DB;
    const kneeDb = state.kneesDb?.[band] ?? 0;
    if (kneeDb > 0) {
      const kneeLeft = toStripX(threshold - kneeDb / 2);
      ctx.fillStyle = kneeFill;
      ctx.fillRect(kneeLeft, stripTop, toStripX(threshold + kneeDb / 2) - kneeLeft, STRIP_HEIGHT);
    }
    const level = levels?.[band]?.levelDb;
    if (typeof level === "number" && level > MB_LEVEL_MIN_DB) {
      const levelX = toStripX(level);
      // Off, nothing acts on the band, so none of its level is "over".
      const thresholdX = bandState.on ? toStripX(threshold) : levelX;
      ctx.fillStyle = levelFill;
      ctx.fillRect(left + 2, stripTop + 3, Math.min(levelX, thresholdX) - left - 2, STRIP_HEIGHT - 6);
      if (levelX > thresholdX) {
        ctx.fillStyle = levelOver;
        ctx.fillRect(thresholdX, stripTop + 3, levelX - thresholdX, STRIP_HEIGHT - 6);
      }
    }
    ctx.fillStyle = thresholdMark;
    ctx.fillRect(toStripX(threshold) - 1, stripTop, 2, STRIP_HEIGHT);
  }

  // The crossovers, over everything, with their frequency at the foot.
  ctx.textAlign = "center";
  ctx.textBaseline = "bottom";
  state.crossovers.forEach((freq, index) => {
    const x = Math.round(axis.toX(freq)) + 0.5;
    const active = index === state.activeCrossover;
    ctx.strokeStyle = active ? crossoverActive : crossoverLine;
    ctx.lineWidth = active ? 2 : 1;
    ctx.beginPath();
    ctx.moveTo(x, PADDING);
    ctx.lineTo(x, stripTop + STRIP_HEIGHT);
    ctx.stroke();
    ctx.fillStyle = active ? crossoverActive : label;
    ctx.fillRect(x - 3, plotBottom - 14, 6, 10);
    ctx.fillText(formatHz(freq), x, plotBottom - 16);
  });
}
