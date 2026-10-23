/**
 * Knobs drawn from images: a face that turns, or a filmstrip of pre-rendered frames.
 *
 * A rotating face spins one picture, so its lighting spins with it; a filmstrip shows the frame
 * a render made for each position, which is how skeuomorphic knobs keep their shading. Frames run
 * top to bottom in one column, or left to right then down in a sheet of `columns` (a sheet keeps
 * a long strip under GPU texture limits). See docs/plans/layout-designer-hires.md §4.
 */

/** A knob's image setup, as a layout control's style.knobImage holds it. */
export interface LayoutKnobImage {
  mode: "rotate" | "filmstrip";
  /** The turning face (rotate) or the frames (filmstrip). */
  imageId: string;
  /** Filmstrip: how many frames, and how many columns they are laid out in (default 1). */
  frames?: number;
  columns?: number;
  /** Rotate: the face's angle at the knob's minimum and maximum (degrees); default -135 and 135. */
  minAngle?: number;
  maxAngle?: number;
  /** Drawn under the face and not turned: a skirt, a shadow. */
  baseImageId?: string;
  /** Drawn over the face and not turned: a glare that keeps the light where it is. */
  glareImageId?: string;
  /** Clip the face to a circle inset 4 px, as a custom knob face always was; default true. */
  clip?: boolean;
}

const MAX_FRAMES = 1024;

/** A whole number of frames from 2 to 1024, or 0 when `frames` is not one. */
export function filmstripFrameCount(frames: unknown): number {
  return typeof frames === "number" && Number.isInteger(frames) && frames >= 2 && frames <= MAX_FRAMES ? frames : 0;
}

/** The frame (0-based) for a knob `position` of 0..1 across `frames` frames. */
export function filmstripFrame(position: number, frames: number): number {
  const clamped = Math.max(0, Math.min(1, Number.isFinite(position) ? position : 0));
  return Math.round(clamped * (frames - 1));
}

/** CSS background-size and background-position that show `frame` of a strip or sheet. */
export function filmstripBackground(frame: number, frames: number, columns = 1): { size: string; position: string } {
  const cols = Math.max(1, Math.min(frames, Number.isInteger(columns) && columns > 0 ? columns : 1));
  const rows = Math.ceil(frames / cols);
  const col = frame % cols;
  const row = Math.floor(frame / cols);
  const x = cols > 1 ? (col / (cols - 1)) * 100 : 0;
  const y = rows > 1 ? (row / (rows - 1)) * 100 : 0;
  return { size: `${cols * 100}% ${rows * 100}%`, position: `${round(x)}% ${round(y)}%` };
}

/** A rotating face's angle (degrees) for a knob `position` of 0..1. */
export function knobFaceAngle(position: number, minAngle = -135, maxAngle = 135): number {
  const clamped = Math.max(0, Math.min(1, Number.isFinite(position) ? position : 0));
  const from = Number.isFinite(minAngle) ? minAngle : -135;
  const to = Number.isFinite(maxAngle) ? maxAngle : 135;
  return from + clamped * (to - from);
}

function round(value: number): number {
  return Math.round(value * 10000) / 10000;
}
