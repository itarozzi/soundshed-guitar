/**
 * A chord box: the classic vertical diagram of one shape, low string on the left (on the right for
 * a left-handed player), five frets from the nut or from the shape's starting fret, × for a muted
 * string, o for an open one, finger numbers in the dots and a bar for a barre. Drawn from the
 * engine's shape (types.ts) as SVG markup.
 */

import { escapeHtml } from "../utils.js";
import type { ScaleGuideShape, ScaleGuideView } from "./types.js";

export interface ChordBoxOptions {
  leftHanded: boolean;
  selected?: boolean;
  /** The chord's name, when the box is not the view's own chord (a progression's strip). */
  chordName?: string;
  /** Its root, to mark; the view's own root otherwise. */
  rootPitchClass?: number;
  /** Text under the box in place of the CAGED name. */
  caption?: string;
}

const STRING_GAP = 16;
const FRET_GAP = 20;
const PAD_X = 22;
const TOP = 30;
const MIN_ROWS = 5;

/** The first fret the box shows: 1 when the shape fits from the nut, else its lowest fret. */
export function boxBaseFret(shape: ScaleGuideShape, capo: number): number {
  const fretted = shape.frets.filter((fret) => fret > capo);
  if (fretted.length === 0) {
    return capo + 1;
  }
  const highest = Math.max(...fretted);
  return highest - capo <= MIN_ROWS ? capo + 1 : Math.min(...fretted);
}

export function renderChordBoxSvg(shape: ScaleGuideShape, view: ScaleGuideView, options: ChordBoxOptions): string {
  const strings = shape.frets.length;
  const capo = view.capo;
  const base = boxBaseFret(shape, capo);
  const highest = Math.max(base, ...shape.frets);
  const rows = Math.max(MIN_ROWS, highest - base + 1);
  const width = PAD_X * 2 + (strings - 1) * STRING_GAP;
  const height = TOP + rows * FRET_GAP + 22;
  const rootPitchClass = options.rootPitchClass ?? view.tones[0]?.pitchClass ?? -1;
  const x = (string: number): number => PAD_X + (options.leftHanded ? strings - 1 - string : string) * STRING_GAP;
  const rowY = (fret: number): number => TOP + (fret - base + 0.5) * FRET_GAP;
  const parts: string[] = [];
  const left = x(options.leftHanded ? strings - 1 : 0);
  const right = x(options.leftHanded ? 0 : strings - 1);

  for (let row = 0; row <= rows; row += 1) {
    const y = TOP + row * FRET_GAP;
    const nut = row === 0 && base === capo + 1;
    parts.push(`<line class="${nut ? "sg-box-nut" : "sg-box-fret"}" x1="${left}" x2="${right}" y1="${y}" y2="${y}"/>`);
  }

  for (let s = 0; s < strings; s += 1) {
    parts.push(`<line class="sg-box-string" x1="${x(s)}" x2="${x(s)}" y1="${TOP}" y2="${TOP + rows * FRET_GAP}"/>`);
  }

  if (base > capo + 1) {
    const labelX = options.leftHanded ? right + 12 : left - 12;
    parts.push(`<text class="sg-box-base" x="${labelX}" y="${rowY(base) + 4}" text-anchor="middle">${base}</text>`);
  }

  for (const barre of shape.barres) {
    const from = Math.min(x(barre.fromString), x(barre.toString));
    const to = Math.max(x(barre.fromString), x(barre.toString));
    parts.push(`<rect class="sg-box-barre" x="${from - 6}" y="${rowY(barre.fret) - 6}" width="${to - from + 12}" height="12" rx="6"/>`);
  }

  shape.frets.forEach((fret, s) => {
    const cx = x(s);

    if (fret < 0) {
      parts.push(`<path class="sg-box-mute" d="M${cx - 4} ${TOP - 14}l8 8m0 -8l-8 8"/>`);
      return;
    }

    const midi = (view.strings[s]?.openMidi ?? 0) + fret;
    const root = ((midi % 12) + 12) % 12 === rootPitchClass;

    if (fret <= capo) {
      parts.push(`<circle class="sg-box-open${root ? " sg-box-root" : ""}" cx="${cx}" cy="${TOP - 10}" r="4.5"/>`);
      return;
    }

    const finger = shape.fingers[s] ?? 0;
    const inBarre = shape.barres.some((b) => b.fret === fret && s >= Math.min(b.fromString, b.toString) && s <= Math.max(b.fromString, b.toString));
    parts.push(`<circle class="sg-box-dot${root ? " sg-box-root" : ""}${inBarre ? " sg-box-in-barre" : ""}" cx="${cx}" cy="${rowY(fret)}" r="6.5"/>`);

    if (finger > 0) {
      parts.push(`<text class="sg-box-finger" x="${cx}" y="${rowY(fret) + 3.5}" text-anchor="middle">${finger}</text>`);
    }
  });

  const caption = options.caption ?? (shape.caged ? `${shape.caged} shape` : "");

  if (caption) {
    parts.push(`<text class="sg-box-caged" x="${width / 2}" y="${height - 4}" text-anchor="middle">${escapeHtml(caption)}</text>`);
  }

  const label = `${options.chordName ?? view.title} at fret ${shape.position}`;
  return `<svg class="sg-box-svg${options.selected ? " selected" : ""}" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="${escapeHtml(label)}">${parts.join("")}</svg>`;
}
