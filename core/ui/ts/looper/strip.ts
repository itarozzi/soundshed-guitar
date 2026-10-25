/**
 * The looper strip's markup: the transport, the summary waveform and readout, the record modes and
 * the options, and (when open) the layers panel. renderLooperStrip() redraws it whole when what it
 * shows changes shape; updateLooperStrip() moves the playhead, the time and the bar in place, which
 * is all that changes while the loop runs.
 */

import { escapeHtml } from "../utils.js";
import { renderLooperLayers, updateLooperLayerControls } from "./layers.js";
import {
  getLivePeaks,
  getLooperLayers,
  getLooperStatus,
  getLayerPeaks,
  isLooperLayersOpen,
  mainButtonLabel,
  type LooperStatus,
} from "./state.js";
import { drawLooperWaveform } from "./waveform.js";

const STATE_LABELS: Record<string, string> = {
  empty: "Ready",
  countIn: "Count-in",
  armed: "Play to start",
  recording: "Recording",
  playing: "Playing",
  overdubArmed: "Play to overdub",
  overdubbing: "Overdubbing",
  stopped: "Stopped",
};

function formatTime(seconds: number): string {
  const safe = Math.max(0, seconds);
  const minutes = Math.floor(safe / 60);
  const rest = safe - minutes * 60;
  return `${minutes}:${rest.toFixed(1).padStart(4, "0")}`;
}

function segment(key: string, value: string, label: string, current: string): string {
  const on = value === current;
  return `<button type="button" class="looper-segment${on ? " is-on" : ""}" data-looper-set="${escapeHtml(key)}" data-value="${escapeHtml(value)}" aria-pressed="${on}">${label}</button>`;
}

function command(name: string, label: string, title: string, disabled = false): string {
  const attrs = `type="button" data-looper-command="${escapeHtml(name)}" title="${escapeHtml(title)}" aria-label="${escapeHtml(title)}"`;
  return `<button class="looper-btn looper-${escapeHtml(name)}-btn" ${attrs}${disabled ? " disabled" : ""}>${label}</button>`;
}

function option(value: number | string, label: string, current: number | string): string {
  return `<option value="${escapeHtml(value)}"${String(value) === String(current) ? " selected" : ""}>${label}</option>`;
}

function renderOptions(status: LooperStatus): string {
  const s = status.settings;
  const countIn = [0, 1, 2].map((n) => option(n, n === 0 ? "None" : `${n} bar${n > 1 ? "s" : ""}`, s.countInBars)).join("");
  const maxSeconds = [30, 60, 120, 300].map((n) => option(n, `${n} s`, s.maxSeconds)).join("");
  const after = option("play", "Play", s.afterRecord) + option("overdub", "Overdub", s.afterRecord);
  const click = `<input type="checkbox" data-looper-option="clickWhileRecording"${s.clickWhileRecording ? " checked" : ""} />`;
  const align = `<input type="number" min="-50" max="50" step="1" value="${s.alignOffsetMs}" data-looper-option="alignOffsetMs" />`;
  return `
    <details class="looper-options">
      <summary title="Looper options">Options</summary>
      <div class="looper-options-body">
        <label>Count-in <select data-looper-option="countInBars">${countIn}</select></label>
        <label>${click} Click while recording</label>
        <label>After a Bars take <select data-looper-option="afterRecord">${after}</select></label>
        <label>Longest loop <select data-looper-option="maxSeconds">${maxSeconds}</select></label>
        <label>Timing offset (ms) ${align}</label>
        <span class="looper-memory">${status.memoryMb.toFixed(0)} MB in use</span>
      </div>
    </details>`;
}

function renderModes(status: LooperStatus): string {
  const s = status.settings;
  const bars = s.length === "bars"
    ? `<span class="looper-bars-stepper">
         <button type="button" class="looper-step" data-looper-bars="-1" aria-label="Fewer bars">−</button>
         <span class="looper-bars-value">${s.bars} bar${s.bars === 1 ? "" : "s"}</span>
         <button type="button" class="looper-step" data-looper-bars="1" aria-label="More bars">+</button>
       </span>`
    : "";
  const tempo = status.tempoBpm ?? null;
  const lock = status.tempoLocked ? " is-locked" : "";
  const tempoTitle = status.tempoLocked ? "Tempo locked to the loop" : "Open the metronome";
  return `
    <div class="looper-modes">
      <span class="looper-segmented" role="group" aria-label="Start">${segment("start", "press", "Press", s.start)}${segment("start", "armed", "Armed", s.start)}</span>
      <span class="looper-segmented" role="group" aria-label="Length">${segment("length", "free", "Free", s.length)}${segment("length", "bars", "Bars", s.length)}</span>
      ${bars}
      <button type="button" class="looper-tempo-chip${lock}" data-looper-tempo title="${tempoTitle}">${tempo ? `${tempo.toFixed(0)} BPM` : "Tempo"}</button>
      ${renderOptions(status)}
    </div>`;
}

/** The strip, drawn whole into `root`. */
export function renderLooperStrip(root: HTMLElement): void {
  const status = getLooperStatus();

  if (!status) {
    root.innerHTML = "";
    return;
  }

  const state = status.state;
  const empty = state === "empty";
  const countIn = state === "countIn" && status.countInBeatsLeft !== undefined
    ? `<div class="looper-countin" aria-live="assertive">${status.countInBeatsLeft}</div>`
    : "";
  const layersOpen = isLooperLayersOpen();
  root.dataset.looperState = state;
  root.innerHTML = `
    <div class="looper-strip-row">
      <div class="looper-transport">
        ${command("recPlayDub", mainButtonLabel(state), "Record, play, overdub")}
        ${command("stop", "Stop", "Stop", empty)}
        ${command("undo", "Undo", "Undo the last layer", !status.canUndo && !["recording", "overdubbing", "countIn", "armed", "overdubArmed"].includes(state))}
        ${command("redo", "Redo", "Redo", !status.canRedo)}
        ${command("clear", "Clear", "Clear the loop", empty && !status.canRedo)}
      </div>
      <div class="looper-display">
        <canvas class="looper-wave" aria-hidden="true"></canvas>
        <div class="looper-progress"><span class="looper-progress-fill"></span></div>
        <div class="looper-readout">
          <span class="looper-status">${escapeHtml(STATE_LABELS[state] ?? state)}</span>
          <span class="looper-time"></span>
          <span class="looper-bar"></span>
          <span class="looper-layer-count">${status.layerCount} layer${status.layerCount === 1 ? "" : "s"}</span>
        </div>
        ${countIn}
      </div>
      ${renderModes(status)}
      <button type="button" class="looper-layers-toggle" data-looper-layers aria-expanded="${layersOpen}">Layers</button>
    </div>
    <div class="looper-layers"${layersOpen ? "" : " hidden"}>${layersOpen ? renderLooperLayers(getLooperLayers()) : ""}</div>`;
  updateLooperStrip(root);
}

/** The parts that move while the loop runs: time, bar, playhead, waveform, layer sliders. */
export function updateLooperStrip(root: HTMLElement): void {
  const status = getLooperStatus();

  if (!status) {
    return;
  }

  const live = getLivePeaks();
  const time = root.querySelector<HTMLElement>(".looper-time");
  const bar = root.querySelector<HTMLElement>(".looper-bar");
  const fill = root.querySelector<HTMLElement>(".looper-progress-fill");
  const recordingFirst = status.state === "recording" && status.lengthSec === 0;

  if (time) {
    time.textContent = recordingFirst
      ? formatTime(status.recordedSec ?? 0)
      : `${formatTime(status.positionSec)} / ${formatTime(status.lengthSec)}`;
  }

  if (bar) {
    bar.textContent = status.bars && status.barIndex ? `bar ${status.barIndex} of ${status.bars}` : "";
  }

  if (fill) {
    const ratio = status.lengthSec > 0 ? Math.min(1, status.positionSec / status.lengthSec) : 0;
    fill.style.width = `${(ratio * 100).toFixed(2)}%`;
  }

  const canvas = root.querySelector<HTMLCanvasElement>(".looper-wave");

  if (canvas) {
    drawLooperWaveform(canvas, status, getLooperLayers(), getLayerPeaks, live);
  }

  updateLooperLayerControls(root, getLooperLayers());
}
