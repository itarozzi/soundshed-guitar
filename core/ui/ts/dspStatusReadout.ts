/**
 * The tiles of a DSP status readout, and which of them it shows.
 *
 * Two readouts share this: an effect's (the meter button on its panel) and the app output's (the
 * chevron under the output meter). Each shows a short list of figures, the user's choice: every
 * tile has a × that removes it, and the + at the end opens a menu of the figures not shown. The
 * choice is an app setting per readout, so it lasts and follows the user into every instance.
 *
 * This module owns the tiles, the menu, the choice, the averaging and the number formats. What
 * each figure is comes from the readout that hosts them: it hands over the values and is told when
 * the choice changes, so it can redraw and start or stop any measurement only a shown figure needs
 * (loudness is measured only while a loudness tile is on screen).
 */

import { getAppSetting, updateAppSetting } from "./appSettingsStore.js";
import { getUiZoom } from "./pointerDrag.js";
import { escapeHtml } from "./utils.js";

export type DspStatusItemId =
  | "peak"
  | "rms"
  | "headroom"
  | "processing"
  | "latency"
  | "momentary"
  | "shortTerm"
  | "integrated"
  | "dspLoad"
  | "dspPeak";

type DspStatusGroup = "Levels" | "Loudness" | "DSP";

interface DspStatusItem {
  label: string;
  /** What the tile shows, as its tooltip, unless the readout says otherwise. */
  title: string;
  group: DspStatusGroup;
}

const ITEMS: Record<DspStatusItemId, DspStatusItem> = {
  peak: { label: "Peak avg", title: "Peak level, averaged", group: "Levels" },
  rms: { label: "RMS avg", title: "RMS level, averaged", group: "Levels" },
  headroom: { label: "Headroom avg", title: "Room left below 0 dBFS, averaged", group: "Levels" },
  momentary: { label: "Momentary", title: "Momentary loudness: the last 400 ms (BS.1770)", group: "Loudness" },
  shortTerm: { label: "Short-term", title: "Short-term loudness: the last 3 seconds (BS.1770)", group: "Loudness" },
  integrated: { label: "Integrated", title: "Integrated loudness since this readout opened", group: "Loudness" },
  processing: { label: "Processing avg", title: "Processing time per block, averaged, and its share of all processing", group: "DSP" },
  latency: { label: "Latency", title: "Delay this adds to the signal", group: "DSP" },
  dspLoad: { label: "DSP load avg", title: "Processing time as a share of each block's real time, averaged", group: "DSP" },
  dspPeak: { label: "Load peak", title: "The heaviest single block in the last 10 seconds, as a share of its real time", group: "DSP" },
};

const GROUP_ORDER: readonly DspStatusGroup[] = ["Levels", "Loudness", "DSP"];

/** The loudness figures, which need a measurement of their own while shown. */
export const LOUDNESS_ITEMS: readonly DspStatusItemId[] = ["momentary", "shortTerm", "integrated"];

export interface DspStatusReadout {
  /** App setting holding the shown items, in order. */
  settingKey: string;
  /** Every figure this readout can show, in menu order within each group. */
  available: readonly DspStatusItemId[];
  /** What it shows until the user chooses. */
  defaults: readonly DspStatusItemId[];
  /** Tooltips that differ from the general ones here. */
  titles?: Partial<Record<DspStatusItemId, string>>;
}

/** An effect's readout: its own levels, loudness, time and latency. */
export const EFFECT_DSP_STATUS: DspStatusReadout = {
  settingKey: "ui.dspStatus.items",
  available: ["peak", "rms", "headroom", "momentary", "shortTerm", "integrated", "processing", "latency"],
  defaults: ["peak", "rms", "processing", "latency"],
};

/** The output meter's readout: the app's output, and the whole chain's load and latency. */
export const OUTPUT_DSP_STATUS: DspStatusReadout = {
  settingKey: "ui.outputStatus.items",
  available: ["peak", "rms", "headroom", "momentary", "shortTerm", "integrated", "dspLoad", "dspPeak", "processing", "latency"],
  defaults: ["dspLoad", "dspPeak", "latency", "peak", "rms"],
  titles: {
    peak: "Output peak level after the master volume, averaged",
    rms: "Output RMS level after the master volume, averaged",
    headroom: "Output room left below 0 dBFS, averaged",
    momentary: "Momentary loudness of the output: the last 400 ms (BS.1770)",
    shortTerm: "Short-term loudness of the output: the last 3 seconds (BS.1770)",
    integrated: "Integrated loudness of the output since this readout opened",
    processing: "Processing time per block for the whole chain, averaged",
    latency: "Delay the whole chain adds to the signal",
  },
};

function isItemId(value: unknown): value is DspStatusItemId {
  return typeof value === "string" && Object.prototype.hasOwnProperty.call(ITEMS, value);
}

/** The items a readout shows: the user's choice, kept to what it can show, else its defaults. */
export function shownDspStatusItems(readout: DspStatusReadout): DspStatusItemId[] {
  const stored = getAppSetting(readout.settingKey);
  if (!Array.isArray(stored)) {
    return [...readout.defaults];
  }
  const shown: DspStatusItemId[] = [];
  for (const value of stored) {
    if (isItemId(value) && readout.available.includes(value) && !shown.includes(value)) {
      shown.push(value);
    }
  }
  return shown;
}

/** Whether any loudness figure is shown, which is what the loudness measurement waits on. */
export function showsLoudness(readout: DspStatusReadout): boolean {
  return shownDspStatusItems(readout).some((id) => LOUDNESS_ITEMS.includes(id));
}

function setShownItems(readout: DspStatusReadout, items: readonly DspStatusItemId[]): void {
  updateAppSetting(readout.settingKey, [...items]);
}

function itemTitle(readout: DspStatusReadout, id: DspStatusItemId): string {
  return readout.titles?.[id] ?? ITEMS[id].title;
}

/** The tiles and the + button, for a readout's container. Values start as dashes. */
export function renderDspStatusTiles(readout: DspStatusReadout): string {
  const shown = shownDspStatusItems(readout);
  const tiles = shown.map((id) => {
    const item = ITEMS[id];
    return `<div class="dsp-status-tile" data-dsp-item="${escapeHtml(id)}" title="${escapeHtml(itemTitle(readout, id))}">`
      + `<span>${escapeHtml(item.label)}</span><strong data-dsp-value="${escapeHtml(id)}">—</strong>`
      + `<button class="dsp-status-tile-remove" type="button" data-dsp-remove="${escapeHtml(id)}" aria-label="${escapeHtml(`Remove ${item.label}`)}" title="${escapeHtml(`Remove ${item.label}`)}">×</button>`
      + "</div>";
  });
  const canAdd = readout.available.some((id) => !shown.includes(id));
  const add = canAdd
    ? `<button class="dsp-status-add" type="button" data-dsp-add aria-haspopup="menu" aria-expanded="false" aria-label="Add a figure" title="Add a figure">+</button>`
    : "";
  return tiles.join("") + add;
}

/** Writes each value into its tile, touching only those that changed. */
export function setDspStatusValues(container: ParentNode, values: Partial<Record<DspStatusItemId, string>>): void {
  for (const [id, text] of Object.entries(values)) {
    const element = container.querySelector<HTMLElement>(`[data-dsp-value="${id}"]`);
    if (element && text !== undefined && element.textContent !== text) {
      element.textContent = text;
    }
  }
}

/** Replaces a tile's tooltip, for a figure whose meaning moves (how long integrated has run). */
export function setDspStatusTitle(container: ParentNode, id: DspStatusItemId, title: string): void {
  const tile = container.querySelector<HTMLElement>(`[data-dsp-item="${id}"]`);
  if (tile && tile.title !== title) {
    tile.title = title;
  }
}

/**
 * Handles a readout's × and + clicks, delegated from its container so a redraw needs no rebinding.
 * `onChange` runs after the shown items change, once the click that changed them has finished: a
 * redraw any sooner would detach the button the click is still bubbling from, and the compact
 * layout's outside-click check walks up from it.
 */
export function bindDspStatusTiles(container: HTMLElement, readout: DspStatusReadout, onChange: () => void): void {
  container.addEventListener("click", (event) => {
    const target = event.target instanceof Element ? event.target : null;
    const remove = target?.closest<HTMLButtonElement>("[data-dsp-remove]");
    if (remove && container.contains(remove)) {
      const id = remove.dataset.dspRemove;
      if (isItemId(id)) {
        remove.closest<HTMLElement>(".dsp-status-tile")?.setAttribute("hidden", "");
        setShownItems(readout, shownDspStatusItems(readout).filter((shown) => shown !== id));
        window.requestAnimationFrame(onChange);
      }
      return;
    }

    const add = target?.closest<HTMLButtonElement>("[data-dsp-add]");
    if (add && container.contains(add)) {
      if (isAddMenuOpenFor(add)) {
        closeDspStatusAddMenu();
      } else {
        openAddMenu(add, readout, onChange, event.detail === 0);
      }
    }
  });
}

// ── The + menu ─────────────────────────────────────────────────────────────

/** Gap between the + button and the menu, and the menu's least distance from the window edge. */
const MENU_GAP = 6;
const VIEWPORT_MARGIN = 8;

let menu: HTMLDivElement | null = null;
let menuAnchor: HTMLButtonElement | null = null;
let menuListenersBound = false;

function isAddMenuOpenFor(anchor: HTMLButtonElement): boolean {
  return Boolean(menu && !menu.hidden && menuAnchor === anchor);
}

/** Whether `target` is inside the + menu, for a readout's own outside-click check. */
export function isInDspStatusAddMenu(target: EventTarget | null): boolean {
  return Boolean(menu && target instanceof Node && menu.contains(target));
}

export function closeDspStatusAddMenu(): void {
  if (!menu || menu.hidden) {
    return;
  }
  // Hidden rather than removed, so a click on one of its items is still inside it while that
  // click bubbles.
  menu.hidden = true;
  menuAnchor?.setAttribute("aria-expanded", "false");
  menuAnchor = null;
}

function ensureMenu(): HTMLDivElement {
  if (menu) {
    return menu;
  }
  // Hung from the body, as the input mode flyout is, so no readout's clipping cuts it off. The
  // attribute keeps the compact Controls sheet open while it is used.
  menu = document.createElement("div");
  menu.className = "dsp-status-add-menu";
  menu.setAttribute("role", "menu");
  menu.setAttribute("aria-label", "Add a figure");
  menu.setAttribute("data-control-bar-popover", "");
  menu.hidden = true;
  document.body.append(menu);

  if (!menuListenersBound) {
    menuListenersBound = true;
    document.addEventListener("pointerdown", (event) => {
      if (!menu || menu.hidden || !(event.target instanceof Node)) {
        return;
      }
      if (menu.contains(event.target) || menuAnchor?.contains(event.target)) {
        return;
      }
      closeDspStatusAddMenu();
    }, true);
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && menu && !menu.hidden) {
        const anchor = menuAnchor;
        closeDspStatusAddMenu();
        anchor?.focus();
        event.stopPropagation();
      }
    }, true);
    window.addEventListener("resize", closeDspStatusAddMenu);
  }
  return menu;
}

function openAddMenu(anchor: HTMLButtonElement, readout: DspStatusReadout, onChange: () => void, focusFirst: boolean): void {
  const element = ensureMenu();
  const shown = shownDspStatusItems(readout);
  const sections = GROUP_ORDER.map((group) => {
    const items = readout.available.filter((id) => ITEMS[id].group === group && !shown.includes(id));
    if (!items.length) {
      return "";
    }
    const buttons = items.map((id) => `<button class="dsp-status-add-option" type="button" role="menuitem" data-dsp-add-item="${escapeHtml(id)}" title="${escapeHtml(itemTitle(readout, id))}">${escapeHtml(ITEMS[id].label)}</button>`);
    return `<div class="dsp-status-add-group" role="group" aria-label="${escapeHtml(group)}"><div class="dsp-status-add-heading">${escapeHtml(group)}</div>${buttons.join("")}</div>`;
  }).join("");

  element.innerHTML = sections;
  element.onclick = (event) => {
    const option = event.target instanceof Element ? event.target.closest<HTMLButtonElement>("[data-dsp-add-item]") : null;
    const id = option?.dataset.dspAddItem;
    if (!isItemId(id)) {
      return;
    }
    closeDspStatusAddMenu();
    setShownItems(readout, [...shownDspStatusItems(readout), id]);
    window.requestAnimationFrame(onChange);
  };

  menuAnchor?.setAttribute("aria-expanded", "false");
  menuAnchor = anchor;
  anchor.setAttribute("aria-expanded", "true");
  element.hidden = false;
  positionMenu(element, anchor);
  if (focusFirst) {
    element.querySelector<HTMLButtonElement>("[data-dsp-add-item]")?.focus();
  }
}

/**
 * Under the + button and kept inside the window, or above it when there is more room there. The
 * app's zoom is CSS zoom on the body, which scales a fixed element's left and top, so the button's
 * client rect and the window are brought into the body's own pixels first.
 */
function positionMenu(element: HTMLDivElement, anchor: HTMLElement): void {
  const zoom = getUiZoom();
  const client = anchor.getBoundingClientRect();
  const rect = { left: client.left / zoom, right: client.right / zoom, top: client.top / zoom, bottom: client.bottom / zoom };
  const viewportWidth = window.innerWidth / zoom;
  const viewportHeight = window.innerHeight / zoom;
  const width = element.offsetWidth;
  const height = element.offsetHeight;
  const left = Math.min(Math.max(rect.right - width, VIEWPORT_MARGIN), Math.max(VIEWPORT_MARGIN, viewportWidth - VIEWPORT_MARGIN - width));
  const spaceBelow = viewportHeight - rect.bottom - MENU_GAP - VIEWPORT_MARGIN;
  const above = height > spaceBelow && rect.top - MENU_GAP - VIEWPORT_MARGIN > spaceBelow;
  const top = above ? Math.max(VIEWPORT_MARGIN, rect.top - MENU_GAP - height) : rect.bottom + MENU_GAP;
  element.style.left = `${Math.round(left)}px`;
  element.style.top = `${Math.round(top)}px`;
}

// ── Averaging and formats ──────────────────────────────────────────────────

/** Smoothing factor for the averaged figures; lower is slower. */
const AVERAGE_SMOOTHING = 0.01;

/** How often averaged figures are redrawn, in milliseconds. */
const AVERAGE_RENDER_INTERVAL_MS = 500;

/**
 * Exponential averages of a readout's figures, fed at the telemetry rate and redrawn at most
 * every AVERAGE_RENDER_INTERVAL_MS, so the numbers can be read rather than watched flicker.
 */
export class DspStatusAverager {
  private readonly averages = new Map<string, number>();
  private lastRenderAt = 0;

  /** Folds in one sample and returns the average, or null for no sample. */
  add(name: string, value: number | null | undefined): number | null {
    if (typeof value !== "number" || !Number.isFinite(value)) {
      return null;
    }
    const previous = this.averages.get(name);
    const average = previous === undefined ? value : previous + AVERAGE_SMOOTHING * (value - previous);
    this.averages.set(name, average);
    return average;
  }

  /** Whether averaged figures are due a redraw; marks them drawn when they are. */
  takeRenderTurn(now = performance.now()): boolean {
    if (this.lastRenderAt !== 0 && now - this.lastRenderAt < AVERAGE_RENDER_INTERVAL_MS) {
      return false;
    }
    this.lastRenderAt = now;
    return true;
  }

  reset(): void {
    this.averages.clear();
    this.lastRenderAt = 0;
  }
}

export function formatDspStatusDb(value: number | null | undefined): string {
  return typeof value === "number" && Number.isFinite(value) ? `${value.toFixed(1)} dBFS` : "—";
}

export function formatDspStatusHeadroom(value: number | null | undefined): string {
  return typeof value === "number" && Number.isFinite(value) ? `${value.toFixed(1)} dB` : "—";
}

/** Microseconds, with the share of `totalUs` when one is given. */
export function formatDspStatusTime(timeUs: number | null, totalUs?: number | null): string {
  if (timeUs === null || !Number.isFinite(timeUs)) {
    return "—";
  }
  const share = typeof totalUs === "number" && totalUs > 0 ? ` (${((timeUs / totalUs) * 100).toFixed(1)}%)` : "";
  return `${timeUs.toFixed(1)} μs${share}`;
}

/** Samples and milliseconds. A node that adds none reads as a dash; with `showZero`, a whole
 * chain's 0 is shown, since there it is a figure worth seeing. */
export function formatDspStatusLatency(latencySamples: number | null, sampleRate?: number | null, showZero = false): string {
  if (latencySamples === null || !Number.isFinite(latencySamples) || latencySamples < 0 || (latencySamples === 0 && !showZero)) {
    return "—";
  }
  const milliseconds = typeof sampleRate === "number" && sampleRate > 0
    ? ` (${((latencySamples / sampleRate) * 1000).toFixed(2)} ms)`
    : "";
  return `${Math.round(latencySamples)} smp${milliseconds}`;
}

export function formatDspStatusPercent(value: number | null | undefined): string {
  return typeof value === "number" && Number.isFinite(value) ? `${value.toFixed(1)}%` : "—";
}

export function formatDspStatusLufs(value: number | null | undefined): string {
  return typeof value === "number" && Number.isFinite(value) ? `${value.toFixed(1)} LUFS` : "—";
}

function formatDuration(seconds: number): string {
  const whole = Math.max(0, Math.floor(seconds));
  return `${Math.floor(whole / 60)}:${String(whole % 60).padStart(2, "0")}`;
}

/** The integrated tile's tooltip once it has run for `seconds`, or before it has a reading. */
export function integratedLoudnessTitle(seconds: number | null): string {
  return seconds === null
    ? "Integrated loudness since this readout opened"
    : `Integrated loudness over the last ${formatDuration(seconds)}, since this readout opened. Close and reopen it to start again.`;
}
