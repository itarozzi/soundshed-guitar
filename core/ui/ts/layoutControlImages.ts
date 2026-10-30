/**
 * Markup for controls drawn from a layout's own images: image knobs (a turning face or a
 * filmstrip, with an optional base and glare that stay still), image toggles and image sliders,
 * and the size a control is drawn at.
 *
 * Every function here returns nothing for a control that does not use the feature, so a control
 * from a layout made before these existed renders exactly as it always did. Image URLs come from
 * `resolve`, which applies the renderer's own checks; anything it refuses draws nothing.
 */

import { filmstripBackground, filmstripFrame, filmstripFrameCount, knobFaceAngle, usableStrip, type LayoutImageStrip, type LayoutKnobImage } from "./layoutKnobImages.js";
import { parseParamTaper, valueToTaperPosition } from "./paramTaper.js";
import { escapeHtml } from "./utils.js";

/** The knob size every knob was drawn at before knobDiameter existed. */
export const DEFAULT_KNOB_DIAMETER = 38;

/** Resolves an image id to a CSS url(...) value for something drawn `drawnWidth` px across, or null. */
export type ImageCssResolver = (imageId: string, drawnWidth: number) => string | null;

/** A size (px) from a layout within [min, max], or undefined when it is not a finite number. */
export function layoutSize(value: unknown, min: number, max: number): number | undefined {
  return typeof value === "number" && Number.isFinite(value) ? Math.max(min, Math.min(max, Math.round(value))) : undefined;
}

/** A knob's image setup when it is usable, else null (the knob then draws as it always did). */
export function usableKnobImage(value: unknown): LayoutKnobImage | null {
  if (!value || typeof value !== "object") return null;
  const knobImage = value as LayoutKnobImage;
  if (typeof knobImage.imageId !== "string" || !knobImage.imageId) return null;
  if (knobImage.mode === "filmstrip") return filmstripFrameCount(knobImage.frames) ? knobImage : null;
  return knobImage.mode === "rotate" ? knobImage : null;
}

/**
 * The attributes, style and children of an image knob at `diameter` px showing `position` (0..1),
 * or null when `knobImage` is not usable or its face image cannot be drawn.
 */
export function imageKnobMarkup(
  knobImage: LayoutKnobImage,
  diameter: number,
  position: number,
  resolve: ImageCssResolver,
): { attributes: string; style: string; children: string } | null {
  // A filmstrip's picture is as wide as its columns of frames.
  const columns = Number.isInteger(knobImage.columns) && knobImage.columns! > 0 ? knobImage.columns! : 1;
  const face = resolve(knobImage.imageId, knobImage.mode === "filmstrip" ? diameter * columns : diameter);
  if (!face) return null;
  const layer = (className: string, imageId: string | undefined) => {
    const url = imageId ? resolve(imageId, diameter) : null;
    return url ? `<div class="${escapeHtml(className)}" style="${escapeHtml(`background-image: ${url};`)}"></div>` : "";
  };

  let attributes = ` data-custom-knob-image="true" data-knob-image="${knobImage.mode}"`;
  let faceStyle = `background-image: ${face};`;
  if (knobImage.mode === "filmstrip") {
    const frames = filmstripFrameCount(knobImage.frames);
    const { size, position: at } = filmstripBackground(filmstripFrame(position, frames), frames, columns);
    attributes += ` data-frames="${frames}" data-columns="${columns}"`;
    faceStyle += ` background-size: ${size}; background-position: ${at};`;
  } else {
    const minAngle = Number.isFinite(knobImage.minAngle) ? knobImage.minAngle! : -135;
    const maxAngle = Number.isFinite(knobImage.maxAngle) ? knobImage.maxAngle! : 135;
    attributes += ` data-min-angle="${minAngle}" data-max-angle="${maxAngle}"`;
    faceStyle += ` transform: rotate(${knobFaceAngle(position, minAngle, maxAngle)}deg);`;
  }
  const faceClass = `custom-knob-face${knobImage.mode === "filmstrip" ? " is-filmstrip" : ""}${knobImage.clip === false ? " no-clip" : ""}`;

  return {
    attributes,
    style: `background-image: none; width: ${diameter}px; height: ${diameter}px;`,
    children: layer("custom-knob-base", knobImage.baseImageId)
      + `<div class="${escapeHtml(faceClass)}" style="${escapeHtml(faceStyle)}"></div>`
      + layer("custom-knob-glare", knobImage.glareImageId),
  };
}

/**
 * Style that draws a styled (not image) knob at `diameter` px: the whole knob, ring and pointer
 * included, zoomed from its 38 px design, so it keeps its proportions. Empty at 38 px.
 */
export function knobDiameterStyle(diameter: number): string {
  return diameter === DEFAULT_KNOB_DIAMETER ? "" : ` zoom: ${Math.round((diameter / DEFAULT_KNOB_DIAMETER) * 10000) / 10000};`;
}

/** Style that shows `frame` of `strip` drawn from `url`. */
function stripFrameStyle(strip: LayoutImageStrip, url: string, frame: number): string {
  const { size, position } = filmstripBackground(frame, strip.frames, strip.columns);
  return `background-image: ${url}; background-size: ${size}; background-position: ${position};`;
}

/**
 * The on and off pictures of an image toggle, or empty when it has none it can draw. A strip wins
 * over two pictures: off is its first frame and on its last.
 */
export function imageToggleChildren(
  toggleImage: { onImageId?: unknown; offImageId?: unknown; strip?: unknown } | undefined,
  width: number,
  resolve: ImageCssResolver,
): string {
  if (!toggleImage || typeof toggleImage !== "object") return "";
  const strip = usableStrip(toggleImage.strip);
  const stripUrl = strip ? resolve(strip.imageId, width * (strip.columns ?? 1)) : null;
  if (strip && stripUrl) {
    return `<span class="custom-toggle-image custom-toggle-off" style="${escapeHtml(stripFrameStyle(strip, stripUrl, 0))}"></span>`
      + `<span class="custom-toggle-image custom-toggle-on" style="${escapeHtml(stripFrameStyle(strip, stripUrl, strip.frames - 1))}"></span>`;
  }
  const on = typeof toggleImage.onImageId === "string" ? resolve(toggleImage.onImageId, width) : null;
  const off = typeof toggleImage.offImageId === "string" ? resolve(toggleImage.offImageId, width) : null;
  if (!on || !off) return "";
  return `<span class="custom-toggle-image custom-toggle-off" style="${escapeHtml(`background-image: ${off};`)}"></span>`
    + `<span class="custom-toggle-image custom-toggle-on" style="${escapeHtml(`background-image: ${on};`)}"></span>`;
}

/**
 * Custom properties that draw an image slider's track and thumb, or empty when it has neither.
 * The pseudo-elements read them through var(), so each URL is made absolute first: a relative
 * url() inside a custom property would resolve against the stylesheet, not the page.
 */
export function imageSliderStyle(
  sliderImage: { trackImageId?: unknown; thumbImageId?: unknown } | undefined,
  length: number,
  resolve: ImageCssResolver,
): string {
  if (!sliderImage || typeof sliderImage !== "object") return "";
  const absolute = (css: string | null) => {
    const url = css?.match(/^url\("(.*)"\)$/)?.[1];
    if (!url) return null;
    try {
      return `url("${new URL(url, document.baseURI).href}")`;
    } catch {
      return null;
    }
  };
  const track = typeof sliderImage.trackImageId === "string" ? absolute(resolve(sliderImage.trackImageId, length)) : null;
  const thumb = typeof sliderImage.thumbImageId === "string" ? absolute(resolve(sliderImage.thumbImageId, 48)) : null;
  return `${track ? ` --slider-track-image: ${track};` : ""}${thumb ? ` --slider-thumb-image: ${thumb};` : ""}`;
}

// ---- What the renderer calls: each returns nothing for a control without the feature ----

/** The parts of a layout control's style these read. */
interface ControlImageStyle {
  knobDiameter?: unknown;
  sliderLength?: unknown;
  toggleWidth?: unknown;
  toggleHeight?: unknown;
  knobImage?: unknown;
  toggleImage?: { onImageId?: unknown; offImageId?: unknown; strip?: unknown };
  sliderImage?: { trackImageId?: unknown; thumbImageId?: unknown; strip?: unknown };
}

/** The knob's drawn diameter: its knobDiameter (16 to 512 px), else the original 38. */
export function knobDiameter(style: ControlImageStyle | undefined): number {
  return layoutSize(style?.knobDiameter, 16, 512) ?? DEFAULT_KNOB_DIAMETER;
}

/** An image knob's markup at `position` (0..1), or null to draw the knob as it always was. */
export function imageKnob(style: ControlImageStyle | undefined, position: number, resolve: ImageCssResolver) {
  const knobImage = usableKnobImage(style?.knobImage);
  return knobImage ? imageKnobMarkup(knobImage, knobDiameter(style), position, resolve) : null;
}

/**
 * `style` with the layout's knobImage in it when the knob takes the layout's: it has no usable
 * knobImage of its own and its knobStyle is "default" or unset (Pedal, Amp, Minimal or a custom
 * face is a choice of its own). Otherwise `style` unchanged.
 */
export function withLayoutKnobImage<T extends ControlImageStyle & { knobStyle?: unknown }>(
  style: T | undefined,
  layoutKnobImage: unknown,
): T | undefined {
  if (!usableKnobImage(layoutKnobImage) || usableKnobImage(style?.knobImage)) return style;
  if (style?.knobStyle !== undefined && style.knobStyle !== "default") return style;
  return { ...(style ?? {}), knobImage: layoutKnobImage } as T;
}

/**
 * `html` with each standard knob in it (.node-param-knob without an image of its own) drawn
 * from `knobImage` at the original 38 px, turned to the value its data attributes hold. The
 * default controls of a useDefaultControls layout are built by the params panel, which knows
 * nothing of layouts, so the image goes in here. `html` unchanged when `knobImage` is not usable.
 */
export function applyKnobImageToKnobs(html: string, knobImage: unknown, resolve: ImageCssResolver): string {
  const usable = usableKnobImage(knobImage);
  if (!usable || !html.includes("node-param-knob")) return html;
  const template = document.createElement("template");
  template.innerHTML = html;
  let changed = false;
  template.content.querySelectorAll<HTMLElement>(".node-param-knob:not([data-custom-knob-image])").forEach((knob) => {
    const parts = imageKnobMarkup(usable, DEFAULT_KNOB_DIAMETER, knobPosition(knob), resolve);
    if (!parts) return;
    // The markup is built as a string for the positioned controls; parse it once to copy it over.
    const probe = document.createElement("template");
    probe.innerHTML = `<div${parts.attributes}>${parts.children}</div>`;
    const source = probe.content.firstElementChild as HTMLElement;
    for (const attribute of Array.from(source.attributes)) knob.setAttribute(attribute.name, attribute.value);
    knob.setAttribute("style", `${knob.getAttribute("style") ?? ""} ${parts.style}`.trim());
    knob.prepend(...Array.from(source.childNodes));
    changed = true;
  });
  return changed ? template.innerHTML : html;
}

/** A knob element's position (0..1) from its data-value, data-min, data-max and data-taper. */
function knobPosition(knob: HTMLElement): number {
  const value = parseFloat(knob.dataset.value ?? "");
  const min = parseFloat(knob.dataset.min ?? "0");
  const max = parseFloat(knob.dataset.max ?? "1");
  if (![value, min, max].every(Number.isFinite)) return 0;
  return valueToTaperPosition(value, min, max, parseParamTaper(knob.dataset.taper));
}

/** Extra style for a styled knob drawn at its knobDiameter; empty at the original size. */
export function styledKnobSizeStyle(style: ControlImageStyle | undefined): string {
  return knobDiameterStyle(knobDiameter(style));
}

/** An image toggle's drawn size: its own, else 36 wide and, from a strip, the frame's shape, else 20 high. */
export function imageToggleSize(style: ControlImageStyle | undefined): { width: number; height: number } {
  const width = layoutSize(style?.toggleWidth, 8, 512) ?? 36;
  const aspect = usableStrip(style?.toggleImage?.strip)?.aspect;
  const height = layoutSize(style?.toggleHeight, 8, 512)
    ?? (typeof aspect === "number" && aspect > 0 ? Math.max(8, Math.min(512, Math.round(width * aspect))) : 20);
  return { width, height };
}

/** An image toggle's class, size and pictures, or null to draw the original switch. */
export function imageToggle(style: ControlImageStyle | undefined, resolve: ImageCssResolver): { style: string; children: string } | null {
  const { width, height } = imageToggleSize(style);
  const children = imageToggleChildren(style?.toggleImage, width, resolve);
  return children ? { style: `width: ${width}px; height: ${height}px;`, children } : null;
}

/** ` data-image-slider style="…"` for a slider with a length or images of its own, else empty. */
export function sliderAttributes(style: ControlImageStyle | undefined, resolve: ImageCssResolver): string {
  const length = layoutSize(style?.sliderLength, 24, 2048);
  const images = imageSliderStyle(style?.sliderImage, length ?? 80, resolve);
  const css = `${length ? `width: ${length}px;` : ""}${images}`;
  return css ? `${images ? " data-image-slider" : ""} style="${escapeHtml(css.trim())}"` : "";
}

/** A filmstrip slider's size: its length along its travel, which runs up a tall frame and across a wide one. */
export function stripSliderSize(style: ControlImageStyle | undefined, strip: LayoutImageStrip): { width: number; height: number; vertical: boolean } {
  const length = layoutSize(style?.sliderLength, 24, 2048) ?? 80;
  const aspect = typeof strip.aspect === "number" && strip.aspect > 0 ? strip.aspect : 0.25;
  const vertical = aspect > 1;
  return vertical
    ? { width: Math.max(4, Math.round(length / aspect)), height: length, vertical }
    : { width: length, height: Math.max(4, Math.round(length * aspect)), vertical };
}

/**
 * A slider drawn from a strip: the frame for `position` (0..1), with the range input laid over it
 * unseen, so the slider is dragged as before. `open` and `close` wrap the input, which takes
 * `attributes`; null for a slider without a strip it can draw.
 */
export function stripSlider(
  style: ControlImageStyle | undefined,
  position: number,
  resolve: ImageCssResolver,
): { open: string; close: string; attributes: string } | null {
  const strip = usableStrip(style?.sliderImage?.strip);
  if (!strip) return null;
  const { width, height, vertical } = stripSliderSize(style, strip);
  const url = resolve(strip.imageId, width * (strip.columns ?? 1));
  if (!url) return null;
  bindStripSliders();
  const columns = Math.max(1, Math.min(strip.frames, Number.isInteger(strip.columns) && strip.columns! > 0 ? strip.columns! : 1));
  return {
    open: `<span class="custom-slider-strip${vertical ? " is-vertical" : ""}" style="${escapeHtml(`width: ${width}px; height: ${height}px;`)}">`
      + `<span class="custom-slider-strip-frame" style="${escapeHtml(stripFrameStyle(strip, url, filmstripFrame(position, strip.frames)))}"></span>`,
    close: "</span>",
    attributes: ` data-slider-strip data-frames="${strip.frames}" data-columns="${columns}"`,
  };
}

let stripSlidersBound = false;

/**
 * Keep each filmstrip slider's frame on its value as it moves: one listener for the document, as
 * the drag, the wheel and the keys all end in an input event on the range.
 */
function bindStripSliders(): void {
  if (stripSlidersBound || typeof document === "undefined") return;
  stripSlidersBound = true;
  document.addEventListener("input", (event) => {
    const input = event.target;
    if (!(input instanceof HTMLInputElement) || !input.hasAttribute("data-slider-strip")) return;
    const frame = input.previousElementSibling;
    if (!(frame instanceof HTMLElement) || !frame.classList.contains("custom-slider-strip-frame")) return;
    const min = parseFloat(input.min || "0");
    const max = parseFloat(input.max || "1");
    const position = max > min ? (parseFloat(input.value) - min) / (max - min) : 0;
    const frames = filmstripFrameCount(parseInt(input.dataset.frames ?? "", 10));
    if (!frames) return;
    frame.style.backgroundPosition = filmstripBackground(filmstripFrame(position, frames), frames, parseInt(input.dataset.columns ?? "1", 10)).position;
  });
}

/** Style that draws a rectangle overlay's image, or empty for one without an image. */
export function overlayImageStyle(
  overlay: { size: { width: number }; style?: { imageId?: unknown; imageFit?: unknown } },
  resolve: ImageCssResolver,
): string {
  const imageId = overlay.style?.imageId;
  const url = typeof imageId === "string" && imageId ? resolve(imageId, overlay.size.width) : null;
  if (!url) return "";
  const fit = overlay.style?.imageFit === "cover" ? "cover" : overlay.style?.imageFit === "stretch" ? "100% 100%" : "contain";
  return ` background-image: ${url}; background-size: ${fit}; background-position: center; background-repeat: no-repeat;`;
}
