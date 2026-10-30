/**
 * The properties panel's "Image & Size" group for a control: the size it is drawn at, and the
 * pictures an image knob, toggle or slider is drawn from (layoutControlImages.ts).
 *
 * Every field writes nothing until it is used, and an emptied field removes its setting, so a
 * control nobody touched here stays exactly as older layouts stored it.
 */

import type { LayoutImageCategory } from "../layoutImageCategories.js";
import { squareFrameStrip, stripFor } from "../layoutKnobImages.js";
import type { LayoutControl, LayoutImageStrip, LayoutKnobImage, LayoutRectangleOverlay } from "../layoutTypes.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

type ImageOptions = (purpose: LayoutImageCategory, selectedImageId?: string) => string;

function numberRow(id: string, label: string, value: unknown, placeholder: string, min: number, max: number, step = 1, title = ""): string {
  const shown = typeof value === "number" && Number.isFinite(value) ? String(value) : "";
  return `
        <div class="layout-property-row">
          <span class="layout-property-label">${escapeHtml(label)}</span>
          <div class="layout-property-input">
            <input type="number" id="${escapeHtml(id)}" min="${min}" max="${max}" step="${step}" placeholder="${escapeHtml(placeholder)}" value="${escapeHtml(shown)}" title="${escapeHtml(title)}">
          </div>
        </div>`;
}

function imageRow(id: string, label: string, selected: string | undefined, options: ImageOptions, purpose: LayoutImageCategory = "knob"): string {
  return `
        <div class="layout-property-row">
          <span class="layout-property-label">${escapeHtml(label)}</span>
          <div class="layout-property-input">
            <select id="${escapeHtml(id)}">${options(purpose, selected)}</select>
          </div>
        </div>`;
}

/**
 * The image knob rows for `knobImage`, ids starting `prefix`: the mode, then what that mode
 * takes. `noneLabel` names the mode that draws no image of its own.
 */
export function renderKnobImageRows(knobImage: LayoutKnobImage | undefined, options: ImageOptions, prefix: string, noneLabel = "None"): string {
  const mode = knobImage?.mode ?? "";
  let rows = `
        <div class="layout-property-row">
          <span class="layout-property-label">Image Knob</span>
          <div class="layout-property-input">
            <select id="${escapeHtml(prefix)}-mode" title="Draw the knob from images: a face that turns, or a filmstrip of rendered frames">
              <option value="" ${mode === "" ? "selected" : ""}>${escapeHtml(noneLabel)}</option>
              <option value="rotate" ${mode === "rotate" ? "selected" : ""}>Turning face</option>
              <option value="filmstrip" ${mode === "filmstrip" ? "selected" : ""}>Filmstrip</option>
            </select>
          </div>
        </div>`;
  if (knobImage) {
    rows += imageRow(`${prefix}-image`, mode === "filmstrip" ? "Frames Image" : "Face Image", knobImage.imageId, options);
    if (mode === "filmstrip") {
      rows += numberRow(`${prefix}-frames`, "Frames", knobImage.frames, "64", 2, 1024);
      rows += numberRow(`${prefix}-columns`, "Columns", knobImage.columns, "1", 1, 1024, 1, "Frames per row in a sheet; 1 for a vertical strip");
    } else {
      rows += numberRow(`${prefix}-min-angle`, "Min Angle", knobImage.minAngle, "-135", -360, 360);
      rows += numberRow(`${prefix}-max-angle`, "Max Angle", knobImage.maxAngle, "135", -360, 360);
      rows += `
        <div class="layout-property-row">
          <span class="layout-property-label">Clip Circle</span>
          <div class="layout-property-input">
            <input type="checkbox" id="${escapeHtml(prefix)}-clip" ${knobImage.clip !== false ? "checked" : ""}>
          </div>
        </div>`;
    }
    rows += imageRow(`${prefix}-base`, "Base Image", knobImage.baseImageId, options);
    rows += imageRow(`${prefix}-glare`, "Glare Image", knobImage.glareImageId, options);
  }
  return rows;
}

/** The library's pixel size for `imageId`, when it knows one. */
function imageSize(imageId: string | undefined): { width?: number; height?: number } {
  const image = imageId ? uiState.layoutLibrary?.images.find((img) => img.imageId === imageId) : undefined;
  return { width: image?.width, height: image?.height };
}

/** `knobImage` with the frames and columns of its picture, when that is a strip of square frames. */
function fitFramesToImage(knobImage: LayoutKnobImage): void {
  const { width, height } = imageSize(knobImage.imageId);
  const strip = knobImage.mode === "filmstrip" ? squareFrameStrip(width, height) : null;
  if (!strip) return;
  knobImage.frames = strip.frames;
  if (strip.columns > 1) knobImage.columns = strip.columns;
  else delete knobImage.columns;
}

/** What holds a knob image: a control's style, or the layout for its knobs' default. */
interface KnobImageOwner {
  knobImage?: LayoutKnobImage;
  knobImageId?: string;
}

/**
 * Wires renderKnobImageRows' fields. `edit` applies a change to the owner, records undo and
 * redraws, and redraws the sidebar too when `rerenderSidebar` is set.
 */
export function bindKnobImageRows(prefix: string, edit: (change: (owner: KnobImageOwner) => void, rerenderSidebar?: boolean) => void): void {
  const setOrDelete = <K extends keyof LayoutKnobImage>(knobImage: LayoutKnobImage, key: K, value: LayoutKnobImage[K] | undefined) => {
    if (value === undefined) delete knobImage[key];
    else knobImage[key] = value;
  };
  const onKnobImage = (suffix: string, apply: (knobImage: LayoutKnobImage, element: HTMLInputElement & HTMLSelectElement) => void) => {
    const element = document.getElementById(`${prefix}-${suffix}`) as (HTMLInputElement & HTMLSelectElement) | null;
    element?.addEventListener("change", () => edit((owner) => {
      if (owner.knobImage) apply(owner.knobImage, element);
    }));
  };
  const onNumber = (suffix: string, key: "frames" | "columns" | "minAngle" | "maxAngle", min: number, max: number) =>
    onKnobImage(suffix, (knobImage, input) => {
      const value = parseFloat(input.value);
      setOrDelete(knobImage, key, input.value.trim() === "" || !Number.isFinite(value) ? undefined : Math.max(min, Math.min(max, Math.round(value))));
    });

  const modeSelect = document.getElementById(`${prefix}-mode`) as HTMLSelectElement | null;
  modeSelect?.addEventListener("change", () => edit((owner) => {
    const mode = modeSelect.value;
    if (mode !== "rotate" && mode !== "filmstrip") {
      delete owner.knobImage;
      return;
    }
    // Start from the knob's custom face, if it had one, so switching on keeps the picture.
    const imageId = owner.knobImage?.imageId ?? owner.knobImageId ?? "";
    owner.knobImage = { ...(owner.knobImage ?? {}), mode, imageId, ...(mode === "filmstrip" ? { frames: owner.knobImage?.frames ?? 64 } : {}) } as LayoutKnobImage;
    fitFramesToImage(owner.knobImage);
  }, true));

  // A filmstrip counts its own frames when it is a strip of square frames; the rows show them.
  const imageSelect = document.getElementById(`${prefix}-image`) as HTMLSelectElement | null;
  imageSelect?.addEventListener("change", () => edit((owner) => {
    if (!owner.knobImage) return;
    owner.knobImage.imageId = imageSelect.value;
    fitFramesToImage(owner.knobImage);
  }, true));
  onKnobImage("base", (knobImage, select) => setOrDelete(knobImage, "baseImageId", select.value || undefined));
  onKnobImage("glare", (knobImage, select) => setOrDelete(knobImage, "glareImageId", select.value || undefined));
  onNumber("frames", "frames", 2, 1024);
  onNumber("columns", "columns", 1, 1024);
  onNumber("min-angle", "minAngle", -360, 360);
  onNumber("max-angle", "maxAngle", -360, 360);
  onKnobImage("clip", (knobImage, input) => {
    if (input.checked) delete knobImage.clip;
    else knobImage.clip = false;
  });
}

/** A strip's picture and, once it has one, its frame count. */
function stripRows(id: string, label: string, strip: LayoutImageStrip | undefined, options: ImageOptions, purpose: LayoutImageCategory, framesTitle: string): string {
  return imageRow(id, label, strip?.imageId, options, purpose)
    + (strip ? numberRow(`${id}-frames`, "Strip Frames", strip.frames, "2", 2, 1024, 1, framesTitle) : "");
}

/** The group's markup for `control`, or empty for a resource selector. */
export function renderControlImageGroup(control: LayoutControl, isResource: boolean, options: ImageOptions, layoutHasKnobImage = false): string {
  if (isResource) return "";
  const style = control.style ?? {};
  let rows = "";

  if (control.type === "toggle") {
    rows += numberRow("prop-ci-toggle-width", "Width", style.toggleWidth, "36", 8, 512);
    rows += numberRow("prop-ci-toggle-height", "Height", style.toggleHeight, "20", 8, 512);
    rows += imageRow("prop-ci-toggle-off", "Off Image", style.toggleImage?.offImageId, options, "switch");
    rows += imageRow("prop-ci-toggle-on", "On Image", style.toggleImage?.onImageId, options, "switch");
    rows += stripRows("prop-ci-toggle-strip", "Switch Strip", style.toggleImage?.strip, options, "switch",
      "Frames in the strip: off is the first, on the last");
  } else if (control.type === "slider") {
    rows += numberRow("prop-ci-slider-length", "Length", style.sliderLength, "80", 24, 2048, 8);
    rows += imageRow("prop-ci-slider-track", "Track Image", style.sliderImage?.trackImageId, options, "general");
    rows += imageRow("prop-ci-slider-thumb", "Thumb Image", style.sliderImage?.thumbImageId, options, "general");
    rows += stripRows("prop-ci-slider-strip", "Slider Strip", style.sliderImage?.strip, options, "general",
      "Frames in the strip, first at the bottom of the travel; set it to the number the strip was rendered with");
  } else {
    rows += numberRow("prop-ci-knob-diameter", "Diameter", style.knobDiameter, "38", 16, 512, 1, "Knob size in px; empty is the standard 38 px");
    rows += renderKnobImageRows(style.knobImage, options, "prop-ci-knob", layoutHasKnobImage ? "Layout's knob" : "None");
  }

  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Image &amp; Size</div>${rows}
      </div>`;
}

/** Wires the group's fields to `control`. */
export function bindControlImageGroup(control: LayoutControl, host: LayoutPropertiesHost): void {
  const edit = (change: (style: NonNullable<LayoutControl["style"]>) => void, rerenderSidebar = false) => {
    host.pushSidebarUndoOnce();
    control.style ??= {};
    change(control.style);
    host.renderCanvas();
    if (rerenderSidebar) host.renderSidebar();
  };
  const onNumber = (id: string, min: number, max: number, apply: (style: NonNullable<LayoutControl["style"]>, value: number | undefined) => void) => {
    const input = document.getElementById(id) as HTMLInputElement | null;
    input?.addEventListener("change", () => edit((style) => {
      const value = parseFloat(input.value);
      apply(style, input.value.trim() === "" || !Number.isFinite(value) ? undefined : Math.max(min, Math.min(max, Math.round(value))));
    }));
  };
  const onImage = (id: string, apply: (style: NonNullable<LayoutControl["style"]>, imageId: string | undefined) => void) => {
    const select = document.getElementById(id) as HTMLSelectElement | null;
    select?.addEventListener("change", () => edit((style) => apply(style, select.value || undefined)));
  };
  const setOrDelete = <T extends object, K extends keyof T>(owner: T, key: K, value: T[K] | undefined) => {
    if (value === undefined) delete owner[key];
    else owner[key] = value;
  };
  // A two-picture object is kept only while it holds a picture.
  const pair = <K extends "toggleImage" | "sliderImage">(style: NonNullable<LayoutControl["style"]>, key: K, field: string, imageId: string | undefined) => {
    const next = { ...(style[key] ?? {}) } as Record<string, string | undefined>;
    setOrDelete(next, field, imageId);
    if (Object.values(next).some(Boolean)) (style as Record<string, unknown>)[key] = next;
    else delete style[key];
  };

  onNumber("prop-ci-knob-diameter", 16, 512, (style, value) => setOrDelete(style, "knobDiameter", value));
  onNumber("prop-ci-toggle-width", 8, 512, (style, value) => setOrDelete(style, "toggleWidth", value));
  onNumber("prop-ci-toggle-height", 8, 512, (style, value) => setOrDelete(style, "toggleHeight", value));
  onNumber("prop-ci-slider-length", 24, 2048, (style, value) => setOrDelete(style, "sliderLength", value));
  onImage("prop-ci-toggle-on", (style, id) => pair(style, "toggleImage", "onImageId", id));
  onImage("prop-ci-toggle-off", (style, id) => pair(style, "toggleImage", "offImageId", id));
  onImage("prop-ci-slider-track", (style, id) => pair(style, "sliderImage", "trackImageId", id));
  onImage("prop-ci-slider-thumb", (style, id) => pair(style, "sliderImage", "thumbImageId", id));

  // A strip takes its layout and frame shape from its picture. Its frames are counted by hand:
  // a switch's or slider's frames are seldom square, so the picture's shape cannot count them.
  // A toggle's starts at two, a slider's at 64, and a new picture keeps the count.
  const stripOwner = (style: NonNullable<LayoutControl["style"]>, key: "toggleImage" | "sliderImage") =>
    (style[key] ??= {}) as { strip?: LayoutImageStrip };
  const tidy = (style: NonNullable<LayoutControl["style"]>, key: "toggleImage" | "sliderImage") => {
    if (!Object.values(style[key] ?? {}).some(Boolean)) delete style[key];
  };
  const bindStrip = (id: string, key: "toggleImage" | "sliderImage", defaultFrames: number) => {
    const select = document.getElementById(id) as HTMLSelectElement | null;
    select?.addEventListener("change", () => edit((style) => {
      const owner = stripOwner(style, key);
      const imageId = select.value;
      if (!imageId) delete owner.strip;
      else {
        const { width, height } = imageSize(imageId);
        const frames = owner.strip?.frames ?? defaultFrames;
        owner.strip = stripFor(imageId, width, height, frames);
      }
      tidy(style, key);
    }, true));
    const framesInput = document.getElementById(`${id}-frames`) as HTMLInputElement | null;
    framesInput?.addEventListener("change", () => edit((style) => {
      const owner = stripOwner(style, key);
      const frames = parseInt(framesInput.value, 10);
      if (!owner.strip || !Number.isFinite(frames)) return;
      const { width, height } = imageSize(owner.strip.imageId);
      owner.strip = stripFor(owner.strip.imageId, width, height, Math.max(2, Math.min(1024, frames)));
    }));
  };
  bindStrip("prop-ci-toggle-strip", "toggleImage", 2);
  bindStrip("prop-ci-slider-strip", "sliderImage", 64);

  bindKnobImageRows("prop-ci-knob", (change, rerenderSidebar) => edit((style) => change(style), rerenderSidebar));
}

/** The rectangle overlay's image rows: the picture drawn in it and how it fills the rectangle. */
export function renderOverlayImageGroup(overlay: LayoutRectangleOverlay, options: ImageOptions): string {
  const fit = overlay.style?.imageFit ?? "contain";
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Image</div>${imageRow("prop-ci-overlay-image", "Image", overlay.style?.imageId, options, "background")}
        <div class="layout-property-row">
          <span class="layout-property-label">Fit</span>
          <div class="layout-property-input">
            <select id="prop-ci-overlay-fit">
              <option value="contain" ${fit === "contain" ? "selected" : ""}>Contain</option>
              <option value="cover" ${fit === "cover" ? "selected" : ""}>Cover</option>
              <option value="stretch" ${fit === "stretch" ? "selected" : ""}>Stretch</option>
            </select>
          </div>
        </div>
      </div>`;
}

/**
 * Wires the overlay's image rows. The first image a rectangle gets clears its fill and border:
 * older apps ignore the image and would otherwise draw a grey box where the picture goes.
 */
export function bindOverlayImageGroup(overlay: LayoutRectangleOverlay, host: LayoutPropertiesHost): void {
  const image = document.getElementById("prop-ci-overlay-image") as HTMLSelectElement | null;
  image?.addEventListener("change", () => {
    host.pushSidebarUndoOnce();
    overlay.style ??= {};
    if (image.value) {
      if (!overlay.style.imageId) {
        overlay.style.backgroundOpacity = 0;
        overlay.style.borderWidth = 0;
      }
      overlay.style.imageId = image.value;
    } else {
      delete overlay.style.imageId;
      delete overlay.style.imageFit;
    }
    host.renderCanvas();
    host.renderSidebar();
  });
  const fit = document.getElementById("prop-ci-overlay-fit") as HTMLSelectElement | null;
  fit?.addEventListener("change", () => {
    host.pushSidebarUndoOnce();
    overlay.style ??= {};
    if (fit.value === "cover" || fit.value === "stretch") overlay.style.imageFit = fit.value;
    else delete overlay.style.imageFit;
    host.renderCanvas();
  });
}
