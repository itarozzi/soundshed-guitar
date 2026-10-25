/**
 * The looper strip: a bar docked above the footer, opened from the footer's Loop button, with the
 * transport, the loop's waveform and time, the record modes, and the layers panel. Behind the
 * Looper feature flag, and only in the standalone app (the engine says whether it is available).
 *
 * The engine is the looper (LooperService); this sends presses and settings and shows what it
 * reports. docs/plans/looper.md is the design.
 */

import { requestLooperState, sendLooperCommand, sendLooperLayer, sendLooperSettings, sendRemoveLooperLayer } from "./bridge.js";
import { FEATURE_FLAGS_CHANGED_EVENT, Features, isFeatureEnabled } from "./featureFlags.js";
import { registerLooperRenderer } from "./looper/refresh.js";
import {
  getLooperLayers,
  getLooperStatus,
  isLooperLayersOpen,
  isLooperStripOpen,
  setLooperLayersOpen,
  setLooperStripOpen,
} from "./looper/state.js";
import { renderLooperStrip, updateLooperStrip } from "./looper/strip.js";
import { showNotification } from "./notifications.js";

const NOTICES: Record<string, string> = {
  disabled: "The looper is off. Turn it on in Settings → Features.",
  noCapture: "The looper is still getting ready. Try again in a moment.",
  tooLong: "That many bars is longer than the longest loop. Choose fewer bars, or a longer loop in the looper's options.",
  noSpare: "The next layer was not ready in time. Press again.",
  noLayerFree: "A loop holds 16 layers at most. Remove or undo one first.",
  noTempo: "Set a tempo in the metronome first.",
  tooShort: "Too short to loop.",
  memory: "The looper is out of memory. Remove or undo some layers, or raise its memory limit.",
  unknownCommand: "The looper did not understand that press.",
};

let strip: HTMLElement | null = null;
let button: HTMLButtonElement | null = null;
let lastShape = "";
let lastNotice = "";
let enabledSent: boolean | null = null;
let initialized = false;

function isLooperOn(): boolean {
  return isFeatureEnabled(Features.Looper);
}

/** What changes the strip's markup, as opposed to what moves while it plays. */
function shapeKey(): string {
  const status = getLooperStatus();

  if (!status) {
    return "";
  }

  const { positionSec: _p, armWaitingSec: _a, recordedSec: _r, barIndex: _b, memoryMb: _m, notice: _n, ...shape } = status;
  const layers = getLooperLayers().map((l) => [l.id, l.title, l.muted, l.solo, l.recording, l.presetName, l.toneChanged]);
  return JSON.stringify([shape, isLooperLayersOpen() ? layers : layers.length]);
}

function render(full: boolean): void {
  const status = getLooperStatus();
  const on = isLooperOn();
  const available = Boolean(status?.available);

  // The engine follows the flag.
  if (status && available && status.settings.enabled !== on && enabledSent !== on) {
    enabledSent = on;
    sendLooperSettings({ enabled: on });
  } else if (status && status.settings.enabled === on) {
    enabledSent = null;
  }

  if (button) {
    button.hidden = !(on && available);
    button.setAttribute("aria-pressed", String(isLooperStripOpen()));
  }

  if (status?.notice && status.notice !== lastNotice) {
    showNotification(NOTICES[status.notice] ?? status.notice);
  }

  lastNotice = status?.notice ?? "";

  if (!strip) {
    return;
  }

  const visible = on && available && isLooperStripOpen();
  strip.hidden = !visible;

  if (!visible) {
    lastShape = "";
    return;
  }

  const shape = shapeKey();

  if (full || shape !== lastShape) {
    const optionsOpen = strip.querySelector<HTMLDetailsElement>("details.looper-options")?.open ?? false;
    renderLooperStrip(strip);
    const options = strip.querySelector<HTMLDetailsElement>("details.looper-options");

    if (options) {
      options.open = optionsOpen;
    }

    lastShape = shape;
  } else {
    updateLooperStrip(strip);
  }
}

function layerIdOf(target: HTMLElement): string {
  return target.closest<HTMLElement>(".looper-layer-row")?.dataset.layerId ?? "";
}

function handleClick(event: MouseEvent): void {
  const target = event.target instanceof HTMLElement ? event.target : null;
  const status = getLooperStatus();

  if (!target || !status) {
    return;
  }

  const commandButton = target.closest<HTMLElement>("[data-looper-command]");

  if (commandButton) {
    sendLooperCommand(commandButton.dataset.looperCommand ?? "");
    return;
  }

  const setting = target.closest<HTMLElement>("[data-looper-set]");

  if (setting) {
    sendLooperSettings({ [setting.dataset.looperSet ?? ""]: setting.dataset.value });
    return;
  }

  const step = target.closest<HTMLElement>("[data-looper-bars]");

  if (step) {
    const bars = Math.min(64, Math.max(1, status.settings.bars + Number(step.dataset.looperBars ?? 0)));
    sendLooperSettings({ bars });
    return;
  }

  if (target.closest("[data-looper-tempo]")) {
    document.getElementById("footer-metronome-btn")?.click();
    return;
  }

  if (target.closest("[data-looper-layers]")) {
    setLooperLayersOpen(!isLooperLayersOpen());
    render(true);
    return;
  }

  const id = layerIdOf(target);
  const layer = getLooperLayers().find((l) => l.id === id);

  if (!layer) {
    return;
  }

  if (target.closest("[data-looper-layer-mute]")) {
    sendLooperLayer(id, { muted: !layer.muted });
  } else if (target.closest("[data-looper-layer-solo]")) {
    sendLooperLayer(id, { solo: !layer.solo });
  } else if (target.closest("[data-looper-layer-remove]")) {
    sendRemoveLooperLayer(id);
  }
}

function handleInput(event: Event): void {
  const input = event.target instanceof HTMLInputElement ? event.target : null;

  if (!input) {
    return;
  }

  const id = layerIdOf(input);

  if (id && input.matches("[data-looper-layer-level]")) {
    sendLooperLayer(id, { level: Number(input.value) });
  } else if (id && input.matches("[data-looper-layer-balance]")) {
    sendLooperLayer(id, { balance: Number(input.value) });
  }
}

function handleChange(event: Event): void {
  const field = event.target instanceof HTMLInputElement || event.target instanceof HTMLSelectElement ? event.target : null;

  if (!field) {
    return;
  }

  const id = layerIdOf(field);

  if (id && field.matches("[data-looper-layer-title]")) {
    sendLooperLayer(id, { title: field.value });
    return;
  }

  const key = field.dataset.looperOption;

  if (!key) {
    return;
  }

  if (field instanceof HTMLInputElement && field.type === "checkbox") {
    sendLooperSettings({ [key]: field.checked });
  } else if (key === "afterRecord") {
    sendLooperSettings({ [key]: field.value });
  } else if (Number.isFinite(Number(field.value))) {
    sendLooperSettings({ [key]: Number(field.value) });
  }
}

export function initializeLooper(): void {
  // Once: a second call only asks for the state again.
  if (initialized) {
    requestLooperState();
    return;
  }

  initialized = true;
  strip = document.getElementById("looper-strip");
  button = document.getElementById("footer-looper-btn") as HTMLButtonElement | null;
  registerLooperRenderer(render);

  button?.addEventListener("click", () => {
    setLooperStripOpen(!isLooperStripOpen());
    render(true);
  });

  strip?.addEventListener("click", handleClick);
  strip?.addEventListener("input", handleInput);
  strip?.addEventListener("change", handleChange);
  document.addEventListener(FEATURE_FLAGS_CHANGED_EVENT, () => render(true));
  // The first state is asked for by main.ts after uiReady, once replies have somewhere to go.
}
