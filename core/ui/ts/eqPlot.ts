/**
 * The static EQ plot: the frequency axis, the grid, the live spectrum behind
 * the curve, and the combined response of the bands. EqCurveInteraction
 * (eqCurve.ts) draws its handles on top of this; the graphic EQ draws only this.
 */
import type { EqSpectrum } from "./eqSpectrum.js";

export type EqBand = { freq: number; gainDb: number; q: number; shelfType?: 'low' | 'high' };

/** The standard frequency axis: 20 Hz to 20 kHz, logarithmic, edge to edge. */
export const EQ_CURVE_MIN_FREQ = 20;
export const EQ_CURVE_MAX_FREQ = 20000;

/** The spectrum's dB range, bottom of the plot to top. Independent of the ±18 dB
 * gain axis: the spectrum is a backdrop showing where the energy is, not a
 * reading against the gain scale. */
export const EQ_SPECTRUM_DISPLAY_FLOOR_DB = -90;
export const EQ_SPECTRUM_DISPLAY_CEILING_DB = -6;

/** Frequency to canvas x and back. Everything drawn on one plot — grid,
 * spectrum, response, handles — goes through the same one, so none of it can
 * disagree about where a frequency is. */
export interface EqFrequencyAxis {
  toX(freq: number): number;
  toFreq(x: number): number;
}

export function eqCurveFreqToX(freq: number, plotLeft: number, plotWidth: number): number {
  const logMin = Math.log10(EQ_CURVE_MIN_FREQ);
  const logMax = Math.log10(EQ_CURVE_MAX_FREQ);
  return plotLeft + plotWidth * (Math.log10(freq) - logMin) / (logMax - logMin);
}

/** The standard axis across a plot. The parametric curve's handles use it. */
export function logFrequencyAxis(plotLeft: number, plotWidth: number): EqFrequencyAxis {
  const logMin = Math.log10(EQ_CURVE_MIN_FREQ);
  const logSpan = Math.log10(EQ_CURVE_MAX_FREQ) - logMin;
  return {
    toX: (freq) => eqCurveFreqToX(freq, plotLeft, plotWidth),
    toFreq: (x) => Math.pow(10, logMin + ((x - plotLeft) / plotWidth) * logSpan),
  };
}

/**
 * An axis pinned to controls laid out by position rather than by frequency: a
 * graphic EQ's evenly spaced sliders. Log frequency is interpolated between
 * neighbouring anchors, and carried past the ends at the nearest segment's
 * rate, so every slider sits over its own frequency. Returns null unless the
 * anchors rise in both frequency and x.
 */
export function anchoredFrequencyAxis(anchors: ReadonlyArray<{ freq: number; x: number }>): EqFrequencyAxis | null {
  const points = anchors
    .filter((anchor) => Number.isFinite(anchor.x) && Number.isFinite(anchor.freq) && anchor.freq > 0)
    .map((anchor) => ({ logFreq: Math.log10(anchor.freq), x: anchor.x }))
    .sort((a, b) => a.x - b.x);
  if (points.length < 2 || points.some((point, i) => i > 0
    && (point.x <= points[i - 1].x || point.logFreq <= points[i - 1].logFreq))) {
    return null;
  }
  const last = points.length - 2;
  return {
    toX: (freq) => {
      const logFreq = Math.log10(freq);
      let i = 0;
      while (i < last && logFreq > points[i + 1].logFreq) i += 1;
      const a = points[i];
      const b = points[i + 1];
      return a.x + (logFreq - a.logFreq) * (b.x - a.x) / (b.logFreq - a.logFreq);
    },
    toFreq: (x) => {
      let i = 0;
      while (i < last && x > points[i + 1].x) i += 1;
      const a = points[i];
      const b = points[i + 1];
      return Math.pow(10, a.logFreq + (x - a.x) * (b.logFreq - a.logFreq) / (b.x - a.x));
    },
  };
}

/** Where each spectrum bin lands on a plot, y clamped to the plot. */
export function eqSpectrumPlotPoints(
  spectrum: EqSpectrum,
  axis: EqFrequencyAxis,
  plotTop: number,
  plotHeight: number,
): Array<{ x: number; y: number }> {
  const count = spectrum.binsDb.length;
  if (count < 2) {
    return [];
  }
  const logMin = Math.log10(spectrum.minFrequencyHz);
  const logSpan = Math.log10(spectrum.maxFrequencyHz) - logMin;
  const dbSpan = EQ_SPECTRUM_DISPLAY_CEILING_DB - EQ_SPECTRUM_DISPLAY_FLOOR_DB;
  return spectrum.binsDb.map((db, index) => {
    const freq = Math.pow(10, logMin + logSpan * (index / (count - 1)));
    const level = Math.max(0, Math.min(1, (db - EQ_SPECTRUM_DISPLAY_FLOOR_DB) / dbSpan));
    return {
      x: axis.toX(freq),
      y: plotTop + plotHeight * (1 - level),
    };
  });
}

export function getEqCurveThemeColors(canvas: HTMLCanvasElement): {
  grid: string;
  response: string;
  handleStroke: string;
  tooltipBackground: string;
  tooltipText: string;
  spectrumFillTop: string;
  spectrumFillBottom: string;
  spectrumLine: string;
} {
  const styles = window.getComputedStyle(canvas);
  return {
    grid: styles.getPropertyValue("--eq-curve-grid").trim() || "rgba(255,255,255,0.08)",
    response: styles.getPropertyValue("--eq-curve-response").trim() || "rgba(72, 168, 224, 0.9)",
    handleStroke: styles.getPropertyValue("--eq-curve-handle-stroke").trim() || "rgba(255, 255, 255, 0.9)",
    tooltipBackground: styles.getPropertyValue("--eq-curve-tooltip-bg").trim() || "rgba(0, 0, 0, 0.8)",
    tooltipText: styles.getPropertyValue("--eq-curve-tooltip-text").trim() || "#ffffff",
    spectrumFillTop: styles.getPropertyValue("--eq-curve-spectrum-fill-top").trim() || "rgba(255, 255, 255, 0.16)",
    spectrumFillBottom: styles.getPropertyValue("--eq-curve-spectrum-fill-bottom").trim() || "rgba(255, 255, 255, 0.02)",
    spectrumLine: styles.getPropertyValue("--eq-curve-spectrum-line").trim() || "rgba(255, 255, 255, 0.28)",
  };
}

/** Traces the spectrum's top edge, smoothed through the midpoints between bins. */
function traceSpectrum(ctx: CanvasRenderingContext2D, points: Array<{ x: number; y: number }>): void {
  ctx.moveTo(points[0].x, points[0].y);
  for (let i = 1; i < points.length - 1; i += 1) {
    const next = points[i + 1];
    ctx.quadraticCurveTo(points[i].x, points[i].y, (points[i].x + next.x) / 2, (points[i].y + next.y) / 2);
  }
  const last = points[points.length - 1];
  ctx.lineTo(last.x, last.y);
}

function drawSpectrum(
  ctx: CanvasRenderingContext2D,
  spectrum: EqSpectrum,
  axis: EqFrequencyAxis,
  padding: number,
  plotWidth: number,
  plotHeight: number,
  colors: ReturnType<typeof getEqCurveThemeColors>,
): void {
  const points = eqSpectrumPlotPoints(spectrum, axis, padding, plotHeight);
  if (points.length < 2) {
    return;
  }
  const bottom = padding + plotHeight;

  ctx.save();
  // An anchored axis can put the ends of the spectrum past the plot's edges.
  ctx.beginPath();
  ctx.rect(padding, padding, plotWidth, plotHeight);
  ctx.clip();
  const gradient = ctx.createLinearGradient(0, padding, 0, bottom);
  gradient.addColorStop(0, colors.spectrumFillTop);
  gradient.addColorStop(1, colors.spectrumFillBottom);
  ctx.fillStyle = gradient;
  ctx.beginPath();
  traceSpectrum(ctx, points);
  ctx.lineTo(points[points.length - 1].x, bottom);
  ctx.lineTo(points[0].x, bottom);
  ctx.closePath();
  ctx.fill();

  ctx.strokeStyle = colors.spectrumLine;
  ctx.lineWidth = 1;
  ctx.beginPath();
  traceSpectrum(ctx, points);
  ctx.stroke();
  ctx.restore();
}

/** Draws the grid, the live spectrum when there is one, and the combined
 * response, on the standard log axis unless given another. */
export function drawEqCurve(
  canvas: HTMLCanvasElement,
  bands: EqBand[],
  spectrum: EqSpectrum | null = null,
  axisOverride: EqFrequencyAxis | null = null,
): void {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }

  const themeColors = getEqCurveThemeColors(canvas);

  const width = canvas.width;
  const height = canvas.height;

  ctx.clearRect(0, 0, width, height);

  const padding = 8;
  const plotWidth = width - padding * 2;
  const plotHeight = height - padding * 2;
  const minDb = -18;
  const maxDb = 18;
  const sampleRate = 44100;
  const axis = axisOverride ?? logFrequencyAxis(padding, plotWidth);

  ctx.strokeStyle = themeColors.grid;
  ctx.lineWidth = 1;
  const gridLines = 4;
  for (let i = 0; i <= gridLines; i += 1) {
    const y = padding + (plotHeight * i) / gridLines;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }

  const freqMarkers = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  freqMarkers.forEach((freq) => {
    const x = axis.toX(freq);
    if (x < padding || x > width - padding) {
      return;
    }
    ctx.beginPath();
    ctx.moveTo(x, padding);
    ctx.lineTo(x, height - padding);
    ctx.stroke();
  });

  if (spectrum) {
    drawSpectrum(ctx, spectrum, axis, padding, plotWidth, plotHeight, themeColors);
  }

  ctx.strokeStyle = themeColors.response;
  ctx.lineWidth = 2;
  ctx.beginPath();

  for (let i = 0; i <= plotWidth; i += 1) {
    // Kept inside the audio band: an anchored axis can run past 20 Hz-20 kHz at the edges.
    const freq = Math.max(10, Math.min(21000, axis.toFreq(padding + i)));
    const magnitude = bands.reduce((acc, band) => acc * bandMagnitude(freq, band, sampleRate), 1.0);
    const db = 20 * Math.log10(Math.max(1e-6, magnitude));
    const clampedDb = Math.max(minDb, Math.min(maxDb, db));
    const x = padding + i;
    const y = padding + (maxDb - clampedDb) / (maxDb - minDb) * plotHeight;
    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }
  ctx.stroke();
}

function peakingMagnitude(freq: number, band: EqBand, sampleRate: number): number {
  if (!band || band.gainDb === 0 || band.freq <= 0) {
    return 1.0;
  }

  const w0 = 2 * Math.PI * band.freq / sampleRate;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const q = Math.max(0.1, band.q || 1.0);
  const A = Math.pow(10, band.gainDb / 40);
  const alpha = sinw0 / (2 * q);

  const b0 = 1 + alpha * A;
  const b1 = -2 * cosw0;
  const b2 = 1 - alpha * A;
  const a0 = 1 + alpha / A;
  const a1 = -2 * cosw0;
  const a2 = 1 - alpha / A;

  const w = 2 * Math.PI * freq / sampleRate;
  const cosw = Math.cos(w);
  const sinw = Math.sin(w);
  const cos2w = Math.cos(2 * w);
  const sin2w = Math.sin(2 * w);

  const numRe = b0 + b1 * cosw + b2 * cos2w;
  const numIm = b1 * -sinw + b2 * -sin2w;
  const denRe = a0 + a1 * cosw + a2 * cos2w;
  const denIm = a1 * -sinw + a2 * -sin2w;

  const numMag = Math.sqrt(numRe * numRe + numIm * numIm);
  const denMag = Math.sqrt(denRe * denRe + denIm * denIm);
  if (denMag <= 0) {
    return 1.0;
  }

  return numMag / denMag;
}

// Generic normalised-biquad transfer function magnitude (denominator starts at 1)
function biquadMagnitude(freq: number, b0: number, b1: number, b2: number, a1: number, a2: number, sampleRate: number): number {
  const w = 2 * Math.PI * freq / sampleRate;
  const cosw = Math.cos(w);
  const sinw = Math.sin(w);
  const cos2w = Math.cos(2 * w);
  const sin2w = Math.sin(2 * w);
  const numRe = b0 + b1 * cosw + b2 * cos2w;
  const numIm = b1 * -sinw + b2 * -sin2w;
  const denRe = 1 + a1 * cosw + a2 * cos2w;
  const denIm = a1 * -sinw + a2 * -sin2w;
  const numMag = Math.sqrt(numRe * numRe + numIm * numIm);
  const denMag = Math.sqrt(denRe * denRe + denIm * denIm);
  return denMag <= 0 ? 1.0 : numMag / denMag;
}

function lowShelfMagnitude(freq: number, band: EqBand, sampleRate: number): number {
  if (!band || band.gainDb === 0 || band.freq <= 0) return 1.0;
  const A = Math.pow(10, band.gainDb / 40);
  const w0 = 2 * Math.PI * band.freq / sampleRate;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const q = Math.max(0.1, band.q ?? 0.707);
  const alpha = sinw0 / (2 * q);
  const sqrtA = Math.sqrt(A);
  const a0 = (A + 1) + (A - 1) * cosw0 + 2 * sqrtA * alpha;
  if (Math.abs(a0) < 1e-9) return 1.0;
  const b0 = A * ((A + 1) - (A - 1) * cosw0 + 2 * sqrtA * alpha) / a0;
  const b1 = 2 * A * ((A - 1) - (A + 1) * cosw0) / a0;
  const b2 = A * ((A + 1) - (A - 1) * cosw0 - 2 * sqrtA * alpha) / a0;
  const a1 = -2 * ((A - 1) + (A + 1) * cosw0) / a0;
  const a2 = ((A + 1) + (A - 1) * cosw0 - 2 * sqrtA * alpha) / a0;
  return biquadMagnitude(freq, b0, b1, b2, a1, a2, sampleRate);
}

function highShelfMagnitude(freq: number, band: EqBand, sampleRate: number): number {
  if (!band || band.gainDb === 0 || band.freq <= 0) return 1.0;
  const A = Math.pow(10, band.gainDb / 40);
  const w0 = 2 * Math.PI * band.freq / sampleRate;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const q = Math.max(0.1, band.q ?? 0.707);
  const alpha = sinw0 / (2 * q);
  const sqrtA = Math.sqrt(A);
  const a0 = (A + 1) - (A - 1) * cosw0 + 2 * sqrtA * alpha;
  if (Math.abs(a0) < 1e-9) return 1.0;
  const b0 = A * ((A + 1) + (A - 1) * cosw0 + 2 * sqrtA * alpha) / a0;
  const b1 = -2 * A * ((A - 1) + (A + 1) * cosw0) / a0;
  const b2 = A * ((A + 1) + (A - 1) * cosw0 - 2 * sqrtA * alpha) / a0;
  const a1 = 2 * ((A - 1) - (A + 1) * cosw0) / a0;
  const a2 = ((A + 1) - (A - 1) * cosw0 - 2 * sqrtA * alpha) / a0;
  return biquadMagnitude(freq, b0, b1, b2, a1, a2, sampleRate);
}

export function bandMagnitude(freq: number, band: EqBand, sampleRate: number): number {
  if (band.shelfType === 'low') return lowShelfMagnitude(freq, band, sampleRate);
  if (band.shelfType === 'high') return highShelfMagnitude(freq, band, sampleRate);
  return peakingMagnitude(freq, band, sampleRate);
}
