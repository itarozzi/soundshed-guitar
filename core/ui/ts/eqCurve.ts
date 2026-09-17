import type { EqSpectrum } from "./eqSpectrum.js";
import { EQ_CURVE_MAX_FREQ, EQ_CURVE_MIN_FREQ, bandMagnitude, drawEqCurve, eqCurveFreqToX, getEqCurveThemeColors, type EqBand } from "./eqPlot.js";

// The plot itself lives in eqPlot.ts; its entry point stays importable from here.
export { drawEqCurve } from "./eqPlot.js";

// ===== Shared 4-band parametric EQ constants =====

/** Canonical param keys for each EQ band (matches ParametricEQEffect). */
export const EQ_BAND_KEYS: ReadonlyArray<{ gain: string; freq: string; q: string | null }> = [
  { gain: "lowGain", freq: "lowFreq", q: "lowQ" },
  { gain: "lowMidGain", freq: "lowMidFreq", q: "lowMidQ" },
  { gain: "highMidGain", freq: "highMidFreq", q: "highMidQ" },
  { gain: "highGain", freq: "highFreq", q: "highQ" },
];

export const EQ_BAND_LABELS: ReadonlyArray<string> = ["Low", "Low Mid", "High Mid", "High"];

export const EQ_BAND_RANGES: ReadonlyArray<{
  freqMin: number; freqMax: number;
  hasQ: boolean; qMin: number; qMax: number; qDefault: number;
  shelfType: 'low' | 'high' | undefined;
}> = [
  { freqMin: 20, freqMax: 500, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 0.707, shelfType: 'low' },
  { freqMin: 200, freqMax: 2000, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 1.0, shelfType: undefined },
  { freqMin: 1000, freqMax: 8000, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 1.0, shelfType: undefined },
  { freqMin: 4000, freqMax: 20000, hasQ: true, qMin: 0.1, qMax: 10, qDefault: 0.707, shelfType: 'high' },
];

export const EQ_FREQ_DEFAULTS: ReadonlyArray<number> = [100, 400, 2000, 8000];

export const GRAPHIC_EQ_FREQUENCIES = [31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000] as const;

function graphicEqFrequencyValue(params: Record<string, number | undefined>, index: number): number {
  const value = params[`band${index + 1}Freq`];
  return typeof value === "number" && Number.isFinite(value)
    ? Math.max(20, Math.min(20000, value))
    : GRAPHIC_EQ_FREQUENCIES[index];
}

export function graphicEqFrequencyBounds(
  params: Record<string, number | undefined>,
  bandNumber: number,
): { min: number; max: number } {
  const index = bandNumber - 1;
  const min = index > 0 ? graphicEqFrequencyValue(params, index - 1) + 1 : 20;
  const max = index < GRAPHIC_EQ_FREQUENCIES.length - 1
    ? graphicEqFrequencyValue(params, index + 1) - 1
    : 20000;
  return min <= max ? { min, max } : { min: 20, max: 20000 };
}

export function clampGraphicEqFrequency(
  params: Record<string, number | undefined>,
  bandNumber: number,
  frequency: number,
): number {
  const { min, max } = graphicEqFrequencyBounds(params, bandNumber);
  return Math.max(min, Math.min(max, frequency));
}

export function buildGraphicEqBandConfigs(params: Record<string, number | undefined>): EqBandConfig[] {
  const bandCount = Math.max(5, Math.min(10, Math.round(params.bandCount ?? 10)));
  return GRAPHIC_EQ_FREQUENCIES.slice(0, bandCount).flatMap((defaultFreq, index) => {
    const number = index + 1;
    if ((params[`band${number}Enabled`] ?? 1) < 0.5) return [];
    return [{
      freq: Math.max(20, Math.min(20000, params[`band${number}Freq`] ?? defaultFreq)),
      gainDb: Math.max(-18, Math.min(18, params[`band${number}Gain`] ?? 0)),
      q: Math.max(0.2, Math.min(8, params[`band${number}Q`] ?? 1.0)),
      defaultFreq,
      defaultGainDb: 0,
      defaultQ: 1.0,
      freqMin: 20,
      freqMax: 20000,
      gainMin: -18,
      gainMax: 18,
      hasQ: false,
      qMin: 0.2,
      qMax: 8,
      shelfType: undefined,
      label: `${Math.round(defaultFreq)} Hz`,
    }];
  });
}

export function graphicEqBandChangeToParams(
  params: Record<string, number | undefined>,
  activeBandIndex: number,
  freq: number,
  gainDb: number,
): Record<string, number> {
  const activeBandNumbers = GRAPHIC_EQ_FREQUENCIES
    .slice(0, Math.max(5, Math.min(10, Math.round(params.bandCount ?? 10))))
    .map((_, index) => index + 1)
    .filter((number) => (params[`band${number}Enabled`] ?? 1) >= 0.5);
  const bandNumber = activeBandNumbers[activeBandIndex];
  return bandNumber
    ? { [`band${bandNumber}Freq`]: clampGraphicEqFrequency(params, bandNumber, freq), [`band${bandNumber}Gain`]: gainDb }
    : {};
}

/**
 * Build EqBandConfig[] from a flat params dict using the canonical param keys.
 * Works for both global EQ node.params and signal-path node.params.
 */
export function buildEqBandConfigsFromParams(
  params: Record<string, number | undefined>,
): EqBandConfig[] {
  const normalize = (value: number | undefined, fallback: number, min: number, max: number): number => {
    if (typeof value !== "number" || !Number.isFinite(value)) {
      return fallback;
    }
    return Math.max(min, Math.min(max, value));
  };

  return EQ_BAND_KEYS.map((keys, i) => {
    const range = EQ_BAND_RANGES[i];
    const freqDefault = EQ_FREQ_DEFAULTS[i];
    const gainValue = normalize(params[keys.gain], 0, -18, 18);
    const freqValue = normalize(params[keys.freq], freqDefault, range.freqMin, range.freqMax);
    const qValue = keys.q
      ? normalize(params[keys.q], range.qDefault, range.qMin, range.qMax)
      : range.qDefault;

    return {
      freq: freqValue,
      gainDb: gainValue,
      q: qValue,
      defaultFreq: freqDefault,
      defaultGainDb: 0,
      defaultQ: range.qDefault,
      freqMin: range.freqMin,
      freqMax: range.freqMax,
      gainMin: -18,
      gainMax: 18,
      hasQ: range.hasQ,
      qMin: range.qMin,
      qMax: range.qMax,
      shelfType: range.shelfType,
      label: EQ_BAND_LABELS[i],
    };
  });
}

/**
 * Convert a band change from EqCurveInteraction into a param-key → value dict
 * using the canonical EQ_BAND_KEYS.
 */
export function eqBandChangeToParams(
  bandIndex: number,
  freq: number,
  gainDb: number,
  q: number,
): Record<string, number> {
  const keys = EQ_BAND_KEYS[bandIndex];
  if (!keys) return {};
  const result: Record<string, number> = {
    [keys.gain]: gainDb,
    [keys.freq]: freq,
  };
  if (keys.q) result[keys.q] = q;
  return result;
}

// ===== Interactive EQ Curve with Draggable Handles =====

const BAND_COLORS = [
  "rgba(232, 168, 56, 0.9)",   // Low - warm amber
  "rgba(126, 207, 74, 0.9)",   // Low Mid - green
  "rgba(74, 200, 207, 0.9)",   // High Mid - cyan
  "rgba(207, 106, 232, 0.9)",  // High - purple
];

export interface EqBandConfig {
  freq: number;
  gainDb: number;
  q: number;
  defaultFreq: number;
  defaultGainDb: number;
  defaultQ: number;
  freqMin: number;
  freqMax: number;
  gainMin: number;
  gainMax: number;
  hasQ: boolean;
  qMin: number;
  qMax: number;
  shelfType: 'low' | 'high' | undefined;
  label: string;
}

export type EqBandChangeHandler = (bandIndex: number, freq: number, gainDb: number, q: number) => void;

interface HandleInfo {
  bandIndex: number;
  type: "main" | "q-left" | "q-right";
  x: number;
  y: number;
  radius: number;
}

/**
 * Interactive EQ curve with draggable band handles.
 *
 * Each band is represented by a main handle (drag horizontally for frequency,
 * vertically for gain). Bands with adjustable Q also show wing handles that
 * can be dragged horizontally to widen/narrow the bandwidth.
 */
export class EqCurveInteraction {
  private canvas: HTMLCanvasElement;
  private bands: EqBandConfig[];
  private onChange: EqBandChangeHandler;
  private onCommit: EqBandChangeHandler;
  private handles: HandleInfo[] = [];
  private hoveredHandle: HandleInfo | null = null;
  private dragHandle: HandleInfo | null = null;
  private isDragging = false;
  private destroyed = false;
  private spectrum: EqSpectrum | null = null;

  private readonly HANDLE_RADIUS = 7;
  private readonly Q_HANDLE_RADIUS = 5;
  private readonly PADDING = 8;
  private readonly MIN_FREQ = EQ_CURVE_MIN_FREQ;
  private readonly MAX_FREQ = EQ_CURVE_MAX_FREQ;
  private readonly MIN_DB = -18;
  private readonly MAX_DB = 18;
  private readonly SAMPLE_RATE = 44100;
  private readonly HIT_SLOP = 4;

  private boundPointerDown: (e: PointerEvent) => void;
  private boundPointerMove: (e: PointerEvent) => void;
  private boundPointerUp: (e: PointerEvent) => void;
  private boundPointerLeave: (e: PointerEvent) => void;
  private boundMouseDown: (e: MouseEvent) => void;
  private boundMouseMove: (e: MouseEvent) => void;
  private boundMouseUp: (e: MouseEvent) => void;
  private boundMouseLeave: (e: MouseEvent) => void;
  private readonly usePointerEvents: boolean;

  constructor(
    canvas: HTMLCanvasElement,
    bands: EqBandConfig[],
    onChange: EqBandChangeHandler,
    onCommit: EqBandChangeHandler
  ) {
    this.canvas = canvas;
    this.bands = bands.map(b => ({ ...b }));
    this.onChange = onChange;
    this.onCommit = onCommit;

    this.boundPointerDown = this.handlePointerDown.bind(this);
    this.boundPointerMove = this.handlePointerMove.bind(this);
    this.boundPointerUp = this.handlePointerUp.bind(this);
    this.boundPointerLeave = this.handlePointerLeave.bind(this);

    this.boundMouseDown = (e: MouseEvent) => this.handlePointerDown(e as unknown as PointerEvent);
    this.boundMouseMove = (e: MouseEvent) => this.handlePointerMove(e as unknown as PointerEvent);
    this.boundMouseUp = (e: MouseEvent) => this.handlePointerUp(e as unknown as PointerEvent);
    this.boundMouseLeave = (e: MouseEvent) => this.handlePointerLeave(e as unknown as PointerEvent);

    this.usePointerEvents = typeof window !== "undefined" && "PointerEvent" in window;

    if (this.usePointerEvents) {
      this.canvas.addEventListener("pointerdown", this.boundPointerDown);
      this.canvas.addEventListener("pointermove", this.boundPointerMove);
      this.canvas.addEventListener("pointerup", this.boundPointerUp);
      this.canvas.addEventListener("pointerleave", this.boundPointerLeave);
    } else {
      this.canvas.addEventListener("mousedown", this.boundMouseDown);
      window.addEventListener("mousemove", this.boundMouseMove);
      window.addEventListener("mouseup", this.boundMouseUp);
      this.canvas.addEventListener("mouseleave", this.boundMouseLeave);
    }
    this.canvas.style.touchAction = "none";

    this.draw();
  }

  /** Shows the live spectrum behind the curve, or with null clears it. */
  setSpectrum(spectrum: EqSpectrum | null): void {
    if (spectrum === this.spectrum) return;
    this.spectrum = spectrum;
    this.draw();
  }

  updateBands(bands: EqBandConfig[]): void {
    if (this.isDragging) return; // Don't overwrite bands during an active drag
    this.bands = bands.map(b => ({ ...b }));
    this.draw();
  }

  draw(): void {
    if (this.destroyed) return;
    const ctx = this.canvas.getContext("2d");
    if (!ctx) return;

    const width = Math.max(1, this.canvas.clientWidth);
    const height = Math.max(1, this.canvas.clientHeight);
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
    }

    // Draw base curve (clears canvas, draws grid + combined response)
    const eqBands: EqBand[] = this.bands.map(b => ({ freq: b.freq, gainDb: b.gainDb, q: b.q, shelfType: b.shelfType }));
    drawEqCurve(this.canvas, eqBands, this.spectrum);

    // Draw per-band shaded fills and individual response curves
    this.drawBandOverlays(ctx);

    // Compute handle positions and render them
    this.handles = [];
    this.computeHandles();
    this.drawHandles(ctx);
  }

  // --- Coordinate mapping ---

  private get plotWidth(): number {
    return this.canvas.width - this.PADDING * 2;
  }

  private get plotHeight(): number {
    return this.canvas.height - this.PADDING * 2;
  }

  private freqToX(freq: number): number {
    return eqCurveFreqToX(freq, this.PADDING, this.plotWidth);
  }

  private xToFreq(x: number): number {
    const logMin = Math.log10(this.MIN_FREQ);
    const logMax = Math.log10(this.MAX_FREQ);
    const ratio = (x - this.PADDING) / this.plotWidth;
    return this.MIN_FREQ * Math.pow(10, ratio * (logMax - logMin));
  }

  private dbToY(db: number): number {
    const clamped = Math.max(this.MIN_DB, Math.min(this.MAX_DB, db));
    return this.PADDING + (this.MAX_DB - clamped) / (this.MAX_DB - this.MIN_DB) * this.plotHeight;
  }

  private yToDb(y: number): number {
    return this.MAX_DB - (y - this.PADDING) / this.plotHeight * (this.MAX_DB - this.MIN_DB);
  }

  // --- Drawing helpers ---

  private drawBandOverlays(ctx: CanvasRenderingContext2D): void {
    const zeroY = this.dbToY(0);

    for (let i = 0; i < this.bands.length; i++) {
      const band = this.bands[i];
      if (Math.abs(band.gainDb) < 0.05) continue;

      const color = BAND_COLORS[i % BAND_COLORS.length];

      // Subtle filled area showing band contribution
      ctx.fillStyle = color.replace(/[\d.]+\)$/, "0.06)");
      ctx.beginPath();
      ctx.moveTo(this.PADDING, zeroY);

      for (let px = 0; px <= this.plotWidth; px++) {
        const freq = this.xToFreq(this.PADDING + px);
        const mag = bandMagnitude(freq, band, this.SAMPLE_RATE);
        const db = 20 * Math.log10(Math.max(1e-6, mag));
        ctx.lineTo(this.PADDING + px, this.dbToY(db));
      }

      ctx.lineTo(this.PADDING + this.plotWidth, zeroY);
      ctx.closePath();
      ctx.fill();

      // Individual band response curve
      ctx.strokeStyle = color.replace(/[\d.]+\)$/, "0.2)");
      ctx.lineWidth = 1;
      ctx.beginPath();

      for (let px = 0; px <= this.plotWidth; px++) {
        const freq = this.xToFreq(this.PADDING + px);
        const mag = bandMagnitude(freq, band, this.SAMPLE_RATE);
        const db = 20 * Math.log10(Math.max(1e-6, mag));
        const y = this.dbToY(db);
        if (px === 0) ctx.moveTo(this.PADDING, y);
        else ctx.lineTo(this.PADDING + px, y);
      }

      ctx.stroke();
    }
  }

  private computeHandles(): void {
    for (let i = 0; i < this.bands.length; i++) {
      const band = this.bands[i];
      const x = this.freqToX(band.freq);
      const y = this.dbToY(band.gainDb);

      // Main frequency/gain handle
      this.handles.push({ bandIndex: i, type: "main", x, y, radius: this.HANDLE_RADIUS });

      // Q wing handles for bands that support Q adjustment
      if (band.hasQ && band.q > 0) {
        const bw = 2 * Math.asinh(1 / (2 * band.q)) / Math.LN2;
        const fLow = band.freq / Math.pow(2, bw / 2);
        const fHigh = band.freq * Math.pow(2, bw / 2);

        this.handles.push({
          bandIndex: i, type: "q-left",
          x: this.freqToX(Math.max(this.MIN_FREQ, fLow)), y,
          radius: this.Q_HANDLE_RADIUS,
        });
        this.handles.push({
          bandIndex: i, type: "q-right",
          x: this.freqToX(Math.min(this.MAX_FREQ, fHigh)), y,
          radius: this.Q_HANDLE_RADIUS,
        });
      }
    }
  }

  private drawHandles(ctx: CanvasRenderingContext2D): void {
    const themeColors = getEqCurveThemeColors(this.canvas);

    // Draw Q connecting lines first (behind handles)
    for (const handle of this.handles) {
      if (handle.type !== "q-left" && handle.type !== "q-right") continue;
      const mainHandle = this.handles.find(h => h.bandIndex === handle.bandIndex && h.type === "main");
      if (!mainHandle) continue;

      const color = BAND_COLORS[handle.bandIndex % BAND_COLORS.length];
      ctx.strokeStyle = color.replace(/[\d.]+\)$/, "0.3)");
      ctx.lineWidth = 1.5;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(mainHandle.x, mainHandle.y);
      ctx.lineTo(handle.x, handle.y);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // Draw all handle circles
    for (const handle of this.handles) {
      const isHovered = this.hoveredHandle !== null &&
        this.hoveredHandle.bandIndex === handle.bandIndex &&
        this.hoveredHandle.type === handle.type;
      const isDragged = this.dragHandle !== null &&
        this.dragHandle.bandIndex === handle.bandIndex &&
        this.dragHandle.type === handle.type;

      const color = BAND_COLORS[handle.bandIndex % BAND_COLORS.length];

      ctx.save();
      if (isDragged) {
        ctx.shadowColor = color;
        ctx.shadowBlur = 12;
      } else if (isHovered) {
        ctx.shadowColor = color;
        ctx.shadowBlur = 6;
      }

      const radius = isHovered || isDragged ? handle.radius + 2 : handle.radius;

      if (handle.type === "main") {
        // Circle for main handles
        ctx.beginPath();
        ctx.arc(handle.x, handle.y, radius, 0, Math.PI * 2);
        ctx.fillStyle = color;
        ctx.fill();
      } else {
        // Semi-translucent square for Q wing handles
        const size = radius * 2;
        ctx.fillStyle = color.replace(/[\d.]+\)$/, "0.35)");
        ctx.fillRect(handle.x - size / 2, handle.y - size / 2, size, size);
      }

      ctx.strokeStyle = themeColors.handleStroke;
      ctx.lineWidth = isDragged ? 2 : 1.5;
      if (handle.type === "main") {
        ctx.stroke();
      } else {
        const size = (isHovered || isDragged ? handle.radius + 2 : handle.radius) * 2;
        ctx.strokeRect(handle.x - size / 2, handle.y - size / 2, size, size);
      }
      ctx.restore();
    }

    // Draw tooltip for hovered or actively-dragged handle
    const tooltipHandle = this.dragHandle ?? this.hoveredHandle;
    if (tooltipHandle) {
      const band = this.bands[tooltipHandle.bandIndex];
      const handle = this.handles.find(
        h => h.bandIndex === tooltipHandle.bandIndex && h.type === tooltipHandle.type
      );
      if (!handle) return;

      let label: string;
      if (tooltipHandle.type === "main") {
        label = `${band.label}: ${Math.round(band.freq)} Hz  ${band.gainDb >= 0 ? "+" : ""}${band.gainDb.toFixed(1)} dB`;
      } else {
        label = `Q: ${band.q.toFixed(1)}`;
      }

      ctx.save();
      ctx.font = "11px -apple-system, BlinkMacSystemFont, sans-serif";
      ctx.textAlign = "center";

      const labelY = Math.max(20, handle.y - handle.radius - 10);
      const metrics = ctx.measureText(label);
      const pad = 5;
      const bgX = handle.x - metrics.width / 2 - pad;
      const bgW = metrics.width + pad * 2;
      const bgH = 18;
      const bgY = labelY - 12;

      // Tooltip background
      const r = 4;
      ctx.fillStyle = themeColors.tooltipBackground;
      ctx.beginPath();
      ctx.moveTo(bgX + r, bgY);
      ctx.lineTo(bgX + bgW - r, bgY);
      ctx.arcTo(bgX + bgW, bgY, bgX + bgW, bgY + r, r);
      ctx.lineTo(bgX + bgW, bgY + bgH - r);
      ctx.arcTo(bgX + bgW, bgY + bgH, bgX + bgW - r, bgY + bgH, r);
      ctx.lineTo(bgX + r, bgY + bgH);
      ctx.arcTo(bgX, bgY + bgH, bgX, bgY + bgH - r, r);
      ctx.lineTo(bgX, bgY + r);
      ctx.arcTo(bgX, bgY, bgX + r, bgY, r);
      ctx.closePath();
      ctx.fill();

      ctx.fillStyle = themeColors.tooltipText;
      ctx.fillText(label, handle.x, labelY);
      ctx.restore();
    }
  }

  // --- Pointer event handlers ---

  private getCanvasCoords(e: PointerEvent): { x: number; y: number } {
    const rect = this.canvas.getBoundingClientRect();
    const scaleX = this.canvas.width / rect.width;
    const scaleY = this.canvas.height / rect.height;
    return {
      x: (e.clientX - rect.left) * scaleX,
      y: (e.clientY - rect.top) * scaleY,
    };
  }

  private hitTestHandle(x: number, y: number): HandleInfo | null {
    let bestDist = Infinity;
    let bestHandle: HandleInfo | null = null;

    for (const h of this.handles) {
      const dx = x - h.x;
      const dy = y - h.y;
      const dist = Math.sqrt(dx * dx + dy * dy);
      if (dist <= h.radius + this.HIT_SLOP && dist < bestDist) {
        bestDist = dist;
        bestHandle = h;
      }
    }
    return bestHandle;
  }

  private handlePointerDown(e: PointerEvent): void {
    const coords = this.getCanvasCoords(e);
    const hit = this.hitTestHandle(coords.x, coords.y);
    if (hit && hit.type === "main" && e.detail >= 2) {
      const band = this.bands[hit.bandIndex];
      band.freq = Math.max(band.freqMin, Math.min(band.freqMax, band.defaultFreq));
      band.gainDb = Math.max(band.gainMin, Math.min(band.gainMax, band.defaultGainDb));
      band.q = Math.max(band.qMin, Math.min(band.qMax, band.defaultQ));
      this.onChange(hit.bandIndex, band.freq, band.gainDb, band.q);
      this.onCommit(hit.bandIndex, band.freq, band.gainDb, band.q);
      this.draw();
      e.preventDefault();
      e.stopPropagation();
      return;
    }

    if (hit) {
      this.isDragging = true;
      this.dragHandle = hit;
      if (this.usePointerEvents && typeof e.pointerId === "number" && this.canvas.setPointerCapture) {
        this.canvas.setPointerCapture(e.pointerId);
      }
      this.canvas.style.cursor = "grabbing";
      e.preventDefault();
      e.stopPropagation();
    }
  }

  private handlePointerMove(e: PointerEvent): void {
    const coords = this.getCanvasCoords(e);

    if (this.isDragging && this.dragHandle) {
      const band = this.bands[this.dragHandle.bandIndex];

      if (this.dragHandle.type === "main") {
        // Horizontal → frequency, vertical → gain
        let newFreq = this.xToFreq(coords.x);
        newFreq = Math.max(band.freqMin, Math.min(band.freqMax, newFreq));
        newFreq = Math.round(newFreq);

        let newGain = this.yToDb(coords.y);
        newGain = Math.max(band.gainMin, Math.min(band.gainMax, newGain));
        // Snap to 0 dB when close
        if (Math.abs(newGain) < 0.3) newGain = 0;
        newGain = Math.round(newGain * 10) / 10;

        band.freq = newFreq;
        band.gainDb = newGain;
        this.onChange(this.dragHandle.bandIndex, band.freq, band.gainDb, band.q);
      } else {
        // Q handle drag: distance from center frequency controls Q
        const dragFreq = this.xToFreq(coords.x);
        const centerFreq = band.freq;
        let ratio: number;
        if (this.dragHandle.type === "q-right") {
          ratio = Math.max(1.05, dragFreq / centerFreq);
        } else {
          ratio = Math.max(1.05, centerFreq / Math.max(1, dragFreq));
        }
        const bw = 2 * Math.log2(ratio);
        let newQ = bw > 0.001 ? 1 / (2 * Math.sinh(bw * Math.LN2 / 2)) : band.qMax;
        newQ = Math.max(band.qMin, Math.min(band.qMax, newQ));
        newQ = Math.round(newQ * 10) / 10;

        band.q = newQ;
        this.onChange(this.dragHandle.bandIndex, band.freq, band.gainDb, band.q);
      }

      this.draw();
      return;
    }

    // Hover detection (not dragging)
    const hit = this.hitTestHandle(coords.x, coords.y);
    if (hit !== this.hoveredHandle) {
      this.hoveredHandle = hit;
      this.canvas.style.cursor = hit ? "grab" : "";
      this.draw();
    }
  }

  private handlePointerUp(e: PointerEvent): void {
    if (this.isDragging && this.dragHandle) {
      const band = this.bands[this.dragHandle.bandIndex];
      this.onCommit(this.dragHandle.bandIndex, band.freq, band.gainDb, band.q);
      this.isDragging = false;
      this.dragHandle = null;
      if (this.usePointerEvents && typeof e.pointerId === "number" && this.canvas.releasePointerCapture) {
        this.canvas.releasePointerCapture(e.pointerId);
      }

      const coords = this.getCanvasCoords(e);
      const hit = this.hitTestHandle(coords.x, coords.y);
      this.hoveredHandle = hit;
      this.canvas.style.cursor = hit ? "grab" : "";
      this.draw();
    }
  }

  private handlePointerLeave(_e: PointerEvent): void {
    if (!this.isDragging) {
      this.hoveredHandle = null;
      this.canvas.style.cursor = "";
      this.draw();
    }
  }

  destroy(): void {
    this.destroyed = true;
    if (this.usePointerEvents) {
      this.canvas.removeEventListener("pointerdown", this.boundPointerDown);
      this.canvas.removeEventListener("pointermove", this.boundPointerMove);
      this.canvas.removeEventListener("pointerup", this.boundPointerUp);
      this.canvas.removeEventListener("pointerleave", this.boundPointerLeave);
    } else {
      this.canvas.removeEventListener("mousedown", this.boundMouseDown);
      window.removeEventListener("mousemove", this.boundMouseMove);
      window.removeEventListener("mouseup", this.boundMouseUp);
      this.canvas.removeEventListener("mouseleave", this.boundMouseLeave);
    }
  }
}
