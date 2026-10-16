/**
 * The output meter's readout: a flyout from the chevron under the output meter, as the input meter
 * has one for calibration. It shows the app's output, measured where the output meter measures it
 * (after the output level and master volume), and the whole chain's DSP load, its worst block in
 * the last ten seconds, its processing time and its latency. Which of those it shows is the user's
 * choice, made with the same tiles as an effect's readout (dspStatusReadout.ts).
 *
 * Hung from the body, as the input mode flyout is, so neither the control bar's clipping nor the
 * compact layout's scrolling sheet cuts it off. The output's loudness is only measured while the
 * flyout is open and shows a loudness figure.
 */

import { recentPeakDspLoadPercent } from "./dspPerformance.js";
import {
  DspStatusAverager,
  OUTPUT_DSP_STATUS,
  bindDspStatusTiles,
  closeDspStatusAddMenu,
  formatDspStatusDb,
  formatDspStatusHeadroom,
  formatDspStatusLatency,
  formatDspStatusLufs,
  formatDspStatusPercent,
  formatDspStatusTime,
  integratedLoudnessTitle,
  isInDspStatusAddMenu,
  renderDspStatusTiles,
  setDspStatusTitle,
  setDspStatusValues,
  showsLoudness,
} from "./dspStatusReadout.js";
import { getOutputLoudness, setOutputLoudnessListener, setOutputLoudnessWatch } from "./nodeLoudness.js";
import { getUiZoom } from "./pointerDrag.js";
import { uiState } from "./state.js";

const FLYOUT_GAP = 8;
const VIEWPORT_MARGIN = 8;

const averages = new DspStatusAverager();
let repositionFrame = 0;
/** The chevron and the flyout, held from initialization: the flyout moves to the body then. */
let trigger: HTMLButtonElement | null = null;
let flyout: HTMLDivElement | null = null;

function triggerEl(): HTMLButtonElement | null {
  return trigger;
}

function flyoutEl(): HTMLDivElement | null {
  return flyout;
}

export function isOutputStatusOpen(): boolean {
  const flyout = flyoutEl();
  return Boolean(flyout && !flyout.hidden);
}

function renderTiles(): void {
  const tiles = flyoutEl()?.querySelector<HTMLElement>(".dsp-status-tiles");
  if (tiles) {
    tiles.innerHTML = renderDspStatusTiles(OUTPUT_DSP_STATUS);
  }
}

/** Centred under the chevron and kept inside the window, or above it when there is more room. */
function positionFlyout(): void {
  const flyout = flyoutEl();
  const trigger = triggerEl();
  if (!flyout || !trigger || flyout.hidden) {
    return;
  }

  // The app's zoom is CSS zoom on the body, which scales a fixed element's left and top.
  const zoom = getUiZoom();
  const client = trigger.getBoundingClientRect();
  const rect = { left: client.left / zoom, top: client.top / zoom, bottom: client.bottom / zoom, width: client.width / zoom };
  const viewportWidth = window.innerWidth / zoom;
  const viewportHeight = window.innerHeight / zoom;
  const width = flyout.offsetWidth;
  const height = flyout.offsetHeight;
  const centred = rect.left + rect.width / 2 - width / 2;
  const left = Math.min(Math.max(centred, VIEWPORT_MARGIN), Math.max(VIEWPORT_MARGIN, viewportWidth - VIEWPORT_MARGIN - width));
  const spaceBelow = viewportHeight - rect.bottom - FLYOUT_GAP - VIEWPORT_MARGIN;
  const spaceAbove = rect.top - FLYOUT_GAP - VIEWPORT_MARGIN;
  const above = height > spaceBelow && spaceAbove > spaceBelow;
  const top = above ? Math.max(VIEWPORT_MARGIN, rect.top - FLYOUT_GAP - height) : rect.bottom + FLYOUT_GAP;
  flyout.style.left = `${Math.round(left)}px`;
  flyout.style.top = `${Math.round(top)}px`;
}

function schedulePosition(): void {
  if (repositionFrame || !isOutputStatusOpen()) {
    return;
  }
  repositionFrame = window.requestAnimationFrame(() => {
    repositionFrame = 0;
    positionFlyout();
  });
}

export function openOutputStatus(): void {
  if (!flyout) {
    return;
  }
  flyout.hidden = false;
  renderTiles();
  averages.reset();
  triggerEl()?.setAttribute("aria-expanded", "true");
  updateOutputStatusReadout();
  updateOutputStatusLoudness();
  positionFlyout();
}

export function closeOutputStatus(): void {
  if (!flyout || flyout.hidden) {
    return;
  }
  flyout.hidden = true;
  closeDspStatusAddMenu();
  setOutputLoudnessWatch(false);
  triggerEl()?.setAttribute("aria-expanded", "false");
}

/** The figures that come with the telemetry feeds. Called on every telemetry redraw. */
export function updateOutputStatusReadout(): void {
  if (!flyout || flyout.hidden) {
    return;
  }

  // Held exactly while the flyout is open and shows loudness; repeating it costs nothing.
  setOutputLoudnessWatch(showsLoudness(OUTPUT_DSP_STATUS));

  const output = uiState.signalDiagnostics?.output;
  const perf = uiState.dspPerformance;
  const values = {
    peak: formatDspStatusDb(averages.add("peak", output?.peakDbfs)),
    rms: formatDspStatusDb(averages.add("rms", output?.rmsDbfs)),
    headroom: formatDspStatusHeadroom(averages.add("headroom", output?.headroomDb)),
    processing: formatDspStatusTime(averages.add("processing", perf?.totalProcessingTimeUs)),
    latency: formatDspStatusLatency(perf?.totalLatencySamples ?? null, perf?.sampleRate, true),
    dspLoad: formatDspStatusPercent(averages.add("dspLoad", perf?.dspLoadPercent)),
    dspPeak: formatDspStatusPercent(recentPeakDspLoadPercent()),
  };
  if (averages.takeRenderTurn()) {
    setDspStatusValues(flyout, values);
  }
}

/** The loudness figures, from the newest reading. Runs on every output "sldL" frame (10 Hz). */
function updateOutputStatusLoudness(): void {
  if (!flyout || flyout.hidden) {
    return;
  }
  const reading = getOutputLoudness();
  setDspStatusValues(flyout, {
    momentary: formatDspStatusLufs(reading?.momentaryLufs),
    shortTerm: formatDspStatusLufs(reading?.shortTermLufs),
    integrated: formatDspStatusLufs(reading?.integratedLufs),
  });
  setDspStatusTitle(flyout, "integrated", integratedLoudnessTitle(reading ? reading.integratedSeconds : null));
}

export function initializeOutputStatusReadout(): void {
  const foundTrigger = document.getElementById("output-status-trigger") as HTMLButtonElement | null;
  const foundFlyout = document.getElementById("output-status-flyout") as HTMLDivElement | null;
  if (trigger || !foundTrigger || !foundFlyout) {
    return;
  }
  trigger = foundTrigger;
  flyout = foundFlyout;

  // Moved to the body, as the input mode flyout is: inside the bar, a theme giving the bar a filter
  // or a backdrop would make it the box a fixed element is laid out in, and clip it.
  document.body.append(foundFlyout);
  foundFlyout.innerHTML = `<div class="output-status-flyout-title">Output</div>`
    + `<div class="output-status-flyout-tiles"><div class="dsp-status-tiles"></div></div>`;

  setOutputLoudnessListener(updateOutputStatusLoudness);

  foundTrigger.addEventListener("click", () => {
    if (isOutputStatusOpen()) {
      closeOutputStatus();
    } else {
      openOutputStatus();
    }
  });

  // A figure added or removed: every value at once, and the loudness watch to suit.
  bindDspStatusTiles(foundFlyout, OUTPUT_DSP_STATUS, () => {
    if (!isOutputStatusOpen()) {
      return;
    }
    renderTiles();
    averages.reset();
    updateOutputStatusReadout();
    updateOutputStatusLoudness();
    schedulePosition();
  });

  document.addEventListener("pointerdown", (event) => {
    if (!isOutputStatusOpen() || !(event.target instanceof Node)) {
      return;
    }
    if (flyoutEl()?.contains(event.target) || triggerEl()?.contains(event.target) || isInDspStatusAddMenu(event.target)) {
      return;
    }
    closeOutputStatus();
  }, true);

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && isOutputStatusOpen()) {
      closeOutputStatus();
      triggerEl()?.focus();
    }
  });

  // The chevron can move under an open flyout: the window resizes, or the compact sheet scrolls.
  window.addEventListener("resize", schedulePosition);
  document.addEventListener("scroll", (event) => {
    if (!(event.target instanceof Node) || !flyoutEl()?.contains(event.target)) {
      schedulePosition();
    }
  }, true);
}
