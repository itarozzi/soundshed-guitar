/**
 * The looper strip's summary waveform: every audible layer's peaks across the loop, stacked as
 * they sum, the take being recorded over them, bar lines for a Bars loop, and the playhead.
 */

import type { LooperLayer, LooperLivePeaks, LooperPeaks, LooperStatus } from "./state.js";

function cssColor(element: Element, name: string, fallback: string): string {
  const value = getComputedStyle(element).getPropertyValue(name).trim();
  return value || fallback;
}

export function drawLooperWaveform(
  canvas: HTMLCanvasElement,
  status: LooperStatus,
  layers: readonly LooperLayer[],
  peaksFor: (id: string) => LooperPeaks | undefined,
  live: LooperLivePeaks | null,
): void {
  const width = Math.max(1, Math.round(canvas.clientWidth * (window.devicePixelRatio || 1)));
  const height = Math.max(1, Math.round(canvas.clientHeight * (window.devicePixelRatio || 1)));

  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }

  const context = canvas.getContext("2d");

  if (!context) {
    return;
  }

  context.clearRect(0, 0, width, height);
  const middle = height / 2;
  const anySolo = layers.some((layer) => layer.solo);
  const audible = layers.filter((layer) => !layer.recording && !layer.muted && (!anySolo || layer.solo));
  const buckets = 256;
  const sum = new Array<number>(buckets).fill(0);

  for (const layer of audible) {
    const peaks = peaksFor(layer.id);

    if (!peaks) {
      continue;
    }

    for (let b = 0; b < buckets; ++b) {
      sum[b] += Math.max(peaks.left[b] ?? 0, peaks.right[b] ?? 0) * layer.level;
    }
  }

  // Scaled to the loudest moment, so a stack of layers does not fill the strip solid.
  const loudest = Math.max(1, ...sum);
  const barWidth = width / buckets;
  context.fillStyle = cssColor(canvas, "--looper-wave", "#7aa2c8");

  for (let b = 0; b < buckets; ++b) {
    const amplitude = (sum[b] / loudest) * middle;
    context.fillRect(b * barWidth, middle - amplitude, Math.max(1, barWidth - 0.5), amplitude * 2);
  }

  // The take being recorded, as far as it has got.
  if (live && live.peaks.length > 0) {
    context.fillStyle = cssColor(canvas, "--looper-wave-live", "#e0525a");

    for (let b = 0; b < live.peaks.length; ++b) {
      const amplitude = Math.min(1, live.peaks[b]) * middle;

      if (amplitude > 0) {
        context.fillRect(b * (width / live.peaks.length), middle - amplitude, Math.max(1, width / live.peaks.length - 0.5), amplitude * 2);
      }
    }
  }

  context.fillStyle = cssColor(canvas, "--looper-grid", "rgba(255,255,255,0.18)");

  // The metronome's bar lines: a loop that starts on a later beat has them that far in.
  if (status.bars && status.bars > 0) {
    const offset = status.clickBar > 0 && status.clickBar < 1 ? status.clickBar : 0;

    for (let bar = 1; bar <= status.bars; ++bar) {
      const at = (bar - offset) / status.bars;

      if (at > 0 && at < 1) {
        context.fillRect(Math.round(at * width), 0, 1, height);
      }
    }
  }

  if (status.lengthSec > 0) {
    context.fillStyle = cssColor(canvas, "--looper-playhead", "#ffffff");
    context.fillRect(Math.round((status.positionSec / status.lengthSec) * width), 0, Math.max(1, window.devicePixelRatio || 1), height);
  }
}
