/**
 * The Multi-band Compressor's plot: its four bands over the live spectrum, so
 * the player can see which band is being compressed and by how much.
 *
 *   - the node's input spectrum, shaded, as behind an EQ curve
 *   - its output spectrum, as a line over it: the gap in a band is what that
 *     band is losing (and its makeup puts back)
 *   - a column per band, divided at the crossovers, with the gain reduction
 *     hanging from the top and its number
 *   - under the plot, each band's level against its threshold
 *
 * The engine sends the output bins and the band meters with the spectrum
 * ("sldS" o and b, eqSpectrum.ts). This file only draws and does the
 * crossover maths; signalPath/paramsPanel/multibandCompressorPanel.ts owns the
 * canvas, the feed and the drags.
 */

import { drawSpectrum, getEqCurveThemeColors, logFrequencyAxis, type EqFrequencyAxis } from "./eqPlot.js";
import type { EqSpectrum, EqSpectrumBand } from "./eqSpectrum.js";

export const MBC_BAND_COUNT = 4;
export const MBC_BAND_LABELS = ["Low", "Low Mid", "High Mid", "High"] as const;
export const MBC_CROSSOVER_KEYS = ["crossover1", "crossover2", "crossover3"] as const;

/** The parameter ranges and defaults, as MultibandCompressorSupport.h declares them. */
export const MBC_CROSSOVER_RANGES = [
  { min: 40, max: 1000, default: 200 },
  { min: 150, max: 5000, default: 800 },
  { min: 600, max: 12000, default: 3000 },
] as const;
export const MBC_DEFAULT_THRESHOLD_DB = -24;

/** A crossover is kept at least a third of an octave above the one below it
 * (kMinSplitRatio in MultibandCompressorSupport.h). */
export const MBC_MIN_SPLIT_RATIO = Math.cbrt(2);

/** Gain reduction from no cut to the deepest the plot shows, top down. */
export const MBC_REDUCTION_SCALE_DB = 24;
/** The level strip's range, dBFS. */
export const MBC_LEVEL_MIN_DB = -60;
export const MBC_LEVEL_MAX_DB = 0;

/** How fast a drawn gain reduction falls back once a band lets go, dB per second. */
const REDUCTION_FALL_DB_PER_SECOND = 30;
const PADDING = 8;
const STRIP_HEIGHT = 16;
const STRIP_GAP = 6;
/** How close, in canvas pixels, a pointer has to be to grab a crossover. */
export const MBC_GRAB_DISTANCE_PX = 6;

/** The crossovers the engine will use for `params`: each pushed a third of an
 * octave above the one below (OrderedSplits, without the Nyquist guard, which
 * only matters at sample rates below 27 kHz). */
export function orderedCrossovers(params: Record<string, number | undefined>): number[] {
  const splits = MBC_CROSSOVER_KEYS.map((key, index) => {
    const range = MBC_CROSSOVER_RANGES[index];
    const value = params[key];
    return Math.min(range.max, Math.max(range.min, typeof value === "number" && Number.isFinite(value) ? value : range.default));
  });
  for (let i = 1; i < splits.length; i += 1) {
    splits[i] = Math.max(splits[i], splits[i - 1] * MBC_MIN_SPLIT_RATIO);
  }
  return splits;
}

/** Where crossover `index` lands when dragged to `freq`: inside its own range
 * and a third of an octave clear of its neighbours, so a drag never pushes
 * another crossover along with it. */
export function draggedCrossover(index: number, freq: number, splits: readonly number[]): number {
  const range = MBC_CROSSOVER_RANGES[index];
  let low: number = range.min;
  let high: number = range.max;
  if (index > 0) {
    low = Math.max(low, splits[index - 1] * MBC_MIN_SPLIT_RATIO);
  }
  if (index + 1 < splits.length) {
    high = Math.min(high, splits[index + 1] / MBC_MIN_SPLIT_RATIO);
  }
  const clamped = Math.min(Math.max(freq, low), Math.max(low, high));
  // Whole hertz below 1 kHz, tens above, as the knob shows them.
  return clamped < 1000 ? Math.round(clamped) : Math.round(clamped / 10) * 10;
}

/** The crossover a pointer at canvas `x` would grab, or -1. */
export function crossoverAt(x: number, splits: readonly number[], axis: EqFrequencyAxis): number {
  let best = -1;
  let bestDistance = MBC_GRAB_DISTANCE_PX;
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
export function mbcPlotAxis(width: number): EqFrequencyAxis {
  return logFrequencyAxis(PADDING, Math.max(1, width - PADDING * 2));
}

/** Gain reduction as drawn: a new peak shows at once, and a band letting go
 * falls back at a steady rate rather than flickering frame to frame. */
export class ReductionDisplay {
  private readonly shown: number[] = new Array<number>(MBC_BAND_COUNT).fill(0);
  private lastAt = 0;

  update(bands: readonly EqSpectrumBand[] | undefined, now: number): readonly number[] {
    const elapsed = this.lastAt > 0 ? Math.min(0.5, (now - this.lastAt) / 1000) : 0;
    this.lastAt = now;
    for (let band = 0; band < MBC_BAND_COUNT; band += 1) {
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

export interface MbcPlotState {
  spectrum: EqSpectrum | null;
  /** The crossovers to draw (orderedCrossovers). */
  crossovers: readonly number[];
  thresholdsDb: readonly number[];
  /** Gain reduction per band as ReductionDisplay gives it. */
  reductionDb: readonly number[];
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
export function drawMultibandCompressorPlot(canvas: HTMLCanvasElement, state: MbcPlotState): void {
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
  const axis = mbcPlotAxis(width);
  const colors = getEqCurveThemeColors(canvas);
  const styles = window.getComputedStyle(canvas);
  const reductionFill = themeColor(styles, "--mbc-reduction-fill", "rgba(232, 128, 72, 0.32)");
  const reductionEdge = themeColor(styles, "--mbc-reduction-edge", "rgba(240, 150, 90, 0.95)");
  const crossoverLine = themeColor(styles, "--mbc-crossover", "rgba(255, 255, 255, 0.38)");
  const crossoverActive = themeColor(styles, "--mbc-crossover-active", colors.response);
  const label = themeColor(styles, "--mbc-label", "rgba(240, 240, 248, 0.72)");
  const levelFill = themeColor(styles, "--mbc-level", "rgba(240, 240, 248, 0.42)");
  const levelOver = themeColor(styles, "--mbc-level-over", "rgba(240, 150, 90, 0.85)");
  const thresholdMark = themeColor(styles, "--mbc-threshold", colors.response);
  const stripBackground = themeColor(styles, "--mbc-strip-bg", "rgba(0, 0, 0, 0.28)");

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

  for (let band = 0; band < MBC_BAND_COUNT; band += 1) {
    const left = edges[band];
    const right = edges[band + 1];
    const columnWidth = Math.max(0, right - left);
    const reduction = Math.min(MBC_REDUCTION_SCALE_DB, Math.max(0, state.reductionDb[band] ?? 0));

    // Gain reduction hangs from the top, over half the plot at full scale.
    const depth = (reduction / MBC_REDUCTION_SCALE_DB) * plotHeight * 0.5;
    if (reduction > 0.05) {
      ctx.fillStyle = reductionFill;
      ctx.fillRect(left + 1, PADDING, columnWidth - 2, depth);
      ctx.fillStyle = reductionEdge;
      ctx.fillRect(left + 1, PADDING + depth - 1.5, columnWidth - 2, 2);
    }

    // The band's name, and its gain reduction while it is cutting.
    ctx.fillStyle = label;
    ctx.textAlign = "center";
    const centre = left + columnWidth / 2;
    if (columnWidth > 36) {
      ctx.fillText(MBC_BAND_LABELS[band], centre, PADDING + 3);
    }
    if (reduction >= 0.5 && columnWidth > 30) {
      ctx.fillStyle = reductionEdge;
      // Under the bar's edge, so the edge never strikes through it.
      ctx.fillText(`−${reduction.toFixed(1)} dB`, centre, Math.max(PADDING + 16, PADDING + depth + 3));
    }

    // The level strip: the band's level from the left, its threshold as a mark.
    const toStripX = (db: number): number => {
      const fraction = (db - MBC_LEVEL_MIN_DB) / (MBC_LEVEL_MAX_DB - MBC_LEVEL_MIN_DB);
      return left + 2 + Math.min(1, Math.max(0, fraction)) * Math.max(0, columnWidth - 4);
    };
    ctx.fillStyle = stripBackground;
    ctx.fillRect(left + 1, stripTop, columnWidth - 2, STRIP_HEIGHT);
    const threshold = state.thresholdsDb[band] ?? MBC_DEFAULT_THRESHOLD_DB;
    const level = levels?.[band]?.levelDb;
    if (typeof level === "number" && level > MBC_LEVEL_MIN_DB) {
      const levelX = toStripX(level);
      const thresholdX = toStripX(threshold);
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
