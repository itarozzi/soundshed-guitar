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

/**
 * A toggle or slider drawn from one strip of frames, laid out as a filmstrip knob's are: a toggle
 * shows the first frame when off and the last when on; a slider shows the frame for its position.
 */
export interface LayoutImageStrip {
  imageId: string;
  frames: number;
  columns?: number;
  /** One frame's height over its width, so the control is drawn at the frame's shape. */
  aspect?: number;
}

const MAX_FRAMES = 1024;

/** A whole number of frames from 2 to 1024, or 0 when `frames` is not one. */
export function filmstripFrameCount(frames: unknown): number {
  return typeof frames === "number" && Number.isInteger(frames) && frames >= 2 && frames <= MAX_FRAMES ? frames : 0;
}

/**
 * The frames and columns of a strip of square frames, the way KnobMan and most knob renderers
 * export them: one column when the image is taller than wide, one row when wider. Null for an
 * image that is not one, whose frames must be counted by hand.
 */
export function squareFrameStrip(width: unknown, height: unknown): { frames: number; columns: number } | null {
  if (typeof width !== "number" || typeof height !== "number" || !Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0) {
    return null;
  }
  if (height > width && height % width === 0) {
    const frames = filmstripFrameCount(height / width);
    return frames ? { frames, columns: 1 } : null;
  }
  if (width > height && width % height === 0) {
    const frames = filmstripFrameCount(width / height);
    return frames ? { frames, columns: frames } : null;
  }
  return null;
}

/**
 * The strip an image `width` x `height` makes holding `frames` frames: in one column when it is
 * at least as tall as wide, else in one row, with the frame's shape when the size is known.
 */
export function stripFor(imageId: string, width: number | undefined, height: number | undefined, frames: number): LayoutImageStrip {
  const count = filmstripFrameCount(frames) || 2;
  const columns = width && height && width > height ? count : 1;
  const rows = Math.ceil(count / columns);
  const aspect = width && height ? (height / rows) / (width / columns) : undefined;
  return {
    imageId,
    frames: count,
    ...(columns > 1 ? { columns } : {}),
    ...(aspect && Number.isFinite(aspect) ? { aspect: Math.round(aspect * 10000) / 10000 } : {}),
  };
}

/** `value` as a strip when it names an image and a frame count, else null. */
export function usableStrip(value: unknown): LayoutImageStrip | null {
  if (!value || typeof value !== "object") return null;
  const strip = value as LayoutImageStrip;
  return typeof strip.imageId === "string" && strip.imageId !== "" && filmstripFrameCount(strip.frames) ? strip : null;
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
