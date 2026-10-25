/**
 * The looper's layers panel: one row per layer, newest first, with its name, the preset it was
 * played through, its level and balance, mute, solo and remove. The sliders are left alone while
 * they are being dragged: the engine's echo of a change only moves a slider nobody is holding.
 */

import { escapeHtml } from "../utils.js";
import type { LooperLayer } from "./state.js";

function layerHue(order: number): number {
  return (order * 67) % 360;
}

function renderRow(layer: LooperLayer): string {
  const id = escapeHtml(layer.id);
  const tone = layer.presetName ? escapeHtml(layer.presetName) : "—";
  const changed = layer.toneChanged
    ? `<span class="looper-tone-changed" title="The preset or scene changed during this take">•</span>`
    : "";
  const recording = layer.recording ? `<span class="looper-layer-rec">Recording</span>` : "";
  const level = `<input type="range" min="0" max="2" step="0.01" value="${layer.level}" data-looper-layer-level aria-label="Level" />`;
  const balance = `<input type="range" min="-1" max="1" step="0.01" value="${layer.balance}" data-looper-layer-balance aria-label="Balance" />`;
  const mute = `<button type="button" class="looper-layer-flag${layer.muted ? " is-on" : ""}" data-looper-layer-mute aria-pressed="${layer.muted}" title="Mute">M</button>`;
  const solo = `<button type="button" class="looper-layer-flag${layer.solo ? " is-on" : ""}" data-looper-layer-solo aria-pressed="${layer.solo}" title="Solo">S</button>`;
  const remove = `<button type="button" class="looper-layer-remove" data-looper-layer-remove title="Remove layer" aria-label="Remove layer">×</button>`;
  const exportButtons = layer.recording
    ? ""
    : `<button type="button" class="looper-layer-export" data-looper-layer-export="riff" title="Send this layer's DI to the riff library as a take">Riff</button><button type="button" class="looper-layer-export" data-looper-layer-export="wetWav" title="Save this layer's sound as a WAV file">WAV</button>`;
  const title = `<input type="text" class="looper-layer-title" value="${escapeHtml(layer.title)}" maxlength="80" data-looper-layer-title aria-label="Layer name" />`;
  return `
    <div class="looper-layer-row" data-layer-id="${id}" style="--looper-layer-hue: ${layerHue(layer.order)}">
      <span class="looper-layer-swatch" aria-hidden="true"></span>
      ${title}
      <span class="looper-tone-chip" title="Played through">${tone}${changed}</span>
      ${recording}
      <label class="looper-layer-slider">Level ${level}</label>
      <label class="looper-layer-slider">Balance ${balance}</label>
      ${mute}${solo}${exportButtons}${remove}
    </div>`;
}

export function renderLooperLayers(layers: readonly LooperLayer[]): string {
  if (layers.length === 0) {
    return `<p class="looper-layers-empty">No layers yet. Press Rec and play.</p>`;
  }

  return layers.map(renderRow).join("");
}

/** The engine's level and balance into each row's sliders, except one being dragged or focused. */
export function updateLooperLayerControls(root: HTMLElement, layers: readonly LooperLayer[]): void {
  const rows = new Map<string, HTMLElement>();

  for (const row of Array.from(root.querySelectorAll<HTMLElement>(".looper-layer-row"))) {
    rows.set(row.dataset.layerId ?? "", row);
  }

  for (const layer of layers) {
    const row = rows.get(layer.id);

    if (!row) {
      continue;
    }

    for (const [selector, value] of [["[data-looper-layer-level]", layer.level], ["[data-looper-layer-balance]", layer.balance]] as const) {
      const input = row.querySelector<HTMLInputElement>(selector);

      if (input && document.activeElement !== input && !input.matches(":active")) {
        input.value = String(value);
      }
    }
  }
}
