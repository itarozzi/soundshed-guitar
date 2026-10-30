/**
 * The Scale Guide's neck: every scale or chord tone on a horizontal fretboard, drawn as SVG
 * markup from the engine's view model (types.ts). The lowest string is at the bottom, as in tab;
 * a left-handed neck runs from right to left. Colours come from CSS (scale-guide.css), so the
 * themes apply.
 */

import { escapeHtml } from "../utils.js";
import type { ScaleGuideDot, ScaleGuideLabels, ScaleGuideView } from "./types.js";

export interface NeckOptions {
  labels: ScaleGuideLabels;
  leftHanded: boolean;
}

const WIDTH = 1000;
const NAME_X = 14;
const OPEN_X = 46;
const NUT_X = 66;
const END_X = 992;
const TOP = 26;
const STRING_GAP = 34;
const DOT_RADIUS = 13;
const INLAYS = [3, 5, 7, 9, 15, 17, 19, 21];
const DOUBLE_INLAYS = [12, 24];
const NUMBERED = [3, 5, 7, 9, 12, 15, 17, 19, 21, 24];

/** Half real fret spacing, half even, so the top frets still hold a label. */
export function fretX(fret: number, frets: number): number {
  if (fret <= 0) {
    return NUT_X;
  }
  const real = (1 - Math.pow(2, -fret / 12)) / (1 - Math.pow(2, -frets / 12));
  return NUT_X + (END_X - NUT_X) * (0.55 * real + 0.45 * (fret / frets));
}

/** Where a note on `fret` sits: mid-way between its fret wires, or left of the nut when open. */
export function noteX(fret: number, frets: number): number {
  return fret <= 0 ? OPEN_X : (fretX(fret - 1, frets) + fretX(fret, frets)) / 2;
}

export function dotLabel(dot: ScaleGuideDot, labels: ScaleGuideLabels): string {
  switch (labels) {
    case "interval":
      return dot.interval;
    case "degree":
      return String(dot.degree);
    case "none":
      return "";
    default:
      return dot.name;
  }
}

/** One decimal place: enough for a 1000-unit drawing, and a number, so markup stays plain. */
function fmt(value: number): number {
  return Math.round(value * 10) / 10;
}

export function renderNeckSvg(view: ScaleGuideView, options: NeckOptions): string {
  const strings = view.strings.length;
  const frets = view.frets;
  const height = TOP + (strings - 1) * STRING_GAP + 54;
  const x = (value: number): number => (options.leftHanded ? WIDTH - value : value);
  const y = (string: number): number => TOP + (strings - 1 - string) * STRING_GAP;
  const middle = TOP + ((strings - 1) * STRING_GAP) / 2;
  const top = TOP - 18;
  const bottom = y(0) + 18;
  const parts: string[] = [];

  const boardLeft = Math.min(x(NUT_X), x(END_X));
  parts.push(
    '<defs><linearGradient id="sg-wood" x1="0" y1="0" x2="0" y2="1"><stop offset="0" class="sg-wood-top"/><stop offset="0.5" class="sg-wood-mid"/><stop offset="1" class="sg-wood-bottom"/></linearGradient></defs>',
  );
  parts.push(`<rect class="sg-board" x="${fmt(boardLeft)}" y="${top}" width="${fmt(END_X - NUT_X)}" height="${bottom - top}" rx="2" fill="url(#sg-wood)"/>`);

  for (const fret of INLAYS.filter((f) => f <= frets)) {
    parts.push(`<circle class="sg-inlay" cx="${fmt(x(noteX(fret, frets)))}" cy="${middle}" r="6"/>`);
  }

  for (const fret of DOUBLE_INLAYS.filter((f) => f <= frets)) {
    const cx = fmt(x(noteX(fret, frets)));
    const offset = ((strings - 1) * STRING_GAP) / 4;
    parts.push(`<circle class="sg-inlay" cx="${cx}" cy="${middle - offset}" r="6"/>`);
    parts.push(`<circle class="sg-inlay" cx="${cx}" cy="${middle + offset}" r="6"/>`);
  }

  for (let fret = 1; fret <= frets; fret += 1) {
    parts.push(`<rect class="sg-fret" x="${fmt(x(fretX(fret, frets)) - 1.25)}" y="${top}" width="2.5" height="${bottom - top}"/>`);
  }

  parts.push(`<rect class="sg-nut" x="${fmt(x(NUT_X) - 5)}" y="${top - 1}" width="10" height="${bottom - top + 2}" rx="2"/>`);

  if (view.capo > 0) {
    const cx = x(noteX(view.capo, frets));
    parts.push(`<rect class="sg-capo" x="${fmt(cx - 7)}" y="${top - 4}" width="14" height="${bottom - top + 8}" rx="5"/>`);
  }

  for (let s = 0; s < strings; s += 1) {
    // Thicker towards the low strings; the wound ones read lighter.
    const weight = 0.9 + 1.9 * (1 - s / Math.max(1, strings - 1));
    const wound = strings > 5 ? s < strings - 3 : s < strings - 2;
    const from = fmt(x(OPEN_X - 14));
    const to = fmt(x(END_X));
    parts.push(`<line class="sg-string${wound ? " sg-string-wound" : ""}" x1="${from}" x2="${to}" y1="${y(s)}" y2="${y(s)}" stroke-width="${weight.toFixed(2)}"/>`);
    const nameX = fmt(x(NAME_X));
    parts.push(`<text class="sg-string-name" x="${nameX}" y="${y(s) + 5}" text-anchor="middle">${escapeHtml(view.strings[s]?.name ?? "")}</text>`);
    // The open string's place, so an empty one still reads as a string you could play.
    parts.push(`<circle class="sg-open-slot" cx="${fmt(x(OPEN_X))}" cy="${y(s)}" r="9"/>`);
  }

  for (const fret of NUMBERED.filter((f) => f <= frets)) {
    parts.push(`<text class="sg-fret-number" x="${fmt(x(noteX(fret, frets)))}" y="${bottom + 28}" text-anchor="middle">${fret}</text>`);
  }

  const shape = view.mode === "chord" && view.position > 0 ? view.shapes?.[view.position - 1] : undefined;

  for (const dot of view.dots) {
    const muted = shape !== undefined && shape.frets[dot.string] < 0;

    if (muted && dot.fret <= view.capo) {
      continue;
    }

    const cx = fmt(x(noteX(dot.fret, frets)));
    const label = dotLabel(dot, options.labels);
    const classes = ["sg-dot", dot.root ? "sg-dot-root" : "", dot.inPosition ? "" : "sg-dot-dim"].filter(Boolean).join(" ");
    const size = label.length > 2 ? 10 : 12.5;
    parts.push(
      `<g class="${escapeHtml(classes)}"><circle cx="${cx}" cy="${y(dot.string)}" r="${DOT_RADIUS}"/>` +
        (label ? `<text x="${cx}" y="${y(dot.string) + size * 0.36}" text-anchor="middle" font-size="${size}">${escapeHtml(label)}</text>` : "") +
        "</g>",
    );
  }

  if (shape) {
    shape.frets.forEach((fret, s) => {
      if (fret >= 0) {
        return;
      }
      const cx = x(OPEN_X);
      const cy = y(s);
      parts.push(`<path class="sg-mute" d="M${fmt(cx - 6)} ${cy - 6}l12 12m0 -12l-12 12"/>`);
    });
  }

  return `<svg class="sg-neck-svg" viewBox="0 0 ${WIDTH} ${height}" role="img" aria-label="${escapeHtml(view.title)} on the neck">${parts.join("")}</svg>`;
}
