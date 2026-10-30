/**
 * The canvas panel's Switches group: the switch every toggle in the layout is drawn from unless it
 * has images of its own (layout.switchImage), the default controls' toggles included. An off and
 * an on picture, or a strip of frames, which wins, and the size it is drawn at.
 */

import { stripFor } from "../layoutKnobImages.js";
import type { LayoutImageCategory } from "../layoutImageCategories.js";
import type { EffectLayout, LayoutSwitchImage } from "../layoutTypes.js";
import { uiState } from "../state.js";
import { escapeHtml } from "../utils.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

type ImageOptions = (purpose: LayoutImageCategory, selectedImageId?: string) => string;

function row(label: string, input: string): string {
  return `
        <div class="layout-property-row">
          <span class="layout-property-label">${escapeHtml(label)}</span>
          <div class="layout-property-input">${input}</div>
        </div>`;
}

function numberInput(id: string, value: number | undefined, placeholder: string, min: number, max: number, title: string): string {
  return `<input type="number" id="${escapeHtml(id)}" min="${min}" max="${max}" step="1" placeholder="${escapeHtml(placeholder)}" value="${value ?? ""}" title="${escapeHtml(title)}">`;
}

export function renderSwitchGroup(layout: EffectLayout, options: ImageOptions, isBackdrop: boolean): string {
  const switchImage = layout.switchImage;
  const strip = switchImage?.strip;
  let rows = row("Off Image", `<select id="prop-layout-switch-off">${options("switch", switchImage?.offImageId)}</select>`)
    + row("On Image", `<select id="prop-layout-switch-on">${options("switch", switchImage?.onImageId)}</select>`)
    + row("Switch Strip", `<select id="prop-layout-switch-strip">${options("switch", strip?.imageId)}</select>`);
  if (strip) {
    rows += row("Strip Frames", numberInput("prop-layout-switch-strip-frames", strip.frames, "2", 2, 1024, "Frames in the strip: off is the first, on the last"));
  }
  if (switchImage) {
    rows += row("Width", numberInput("prop-layout-switch-width", switchImage.width, "36", 8, 512, "The switch's drawn width, px"))
      + row("Height", numberInput("prop-layout-switch-height", switchImage.height, "Auto", 8, 512, "The switch's drawn height, px; empty keeps the picture's shape"));
  }
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Switches</div>
        <div class="layout-property-hint">
          The switch every toggle is drawn from${isBackdrop ? ", the default controls' included" : ""}, unless a toggle sets its own images. Pick an off and an on picture, or a strip.
        </div>${rows}
      </div>`;
}

/** The library's pixel size for `imageId`, when it knows one. */
function imageSize(imageId: string | undefined): { width?: number; height?: number } {
  const image = imageId ? uiState.layoutLibrary?.images.find((img) => img.imageId === imageId) : undefined;
  return { width: image?.width, height: image?.height };
}

/** The layout's switch, dropped once it has no picture left. */
function tidy(layout: EffectLayout): void {
  const switchImage = layout.switchImage;
  if (switchImage && !switchImage.offImageId && !switchImage.onImageId && !switchImage.strip) delete layout.switchImage;
}

export function bindSwitchGroup(layout: EffectLayout, host: LayoutPropertiesHost): void {
  const edit = (change: (switchImage: LayoutSwitchImage) => void, rerenderSidebar = false) => {
    host.pushSidebarUndoOnce();
    layout.switchImage ??= {};
    change(layout.switchImage);
    tidy(layout);
    host.renderCanvas();
    if (rerenderSidebar) host.renderSidebar();
  };
  const onSelect = (id: string, apply: (switchImage: LayoutSwitchImage, imageId: string | undefined) => void) => {
    const select = document.getElementById(id) as HTMLSelectElement | null;
    select?.addEventListener("change", () => edit((switchImage) => apply(switchImage, select.value || undefined), true));
  };
  const onNumber = (id: string, apply: (switchImage: LayoutSwitchImage, value: number | undefined) => void) => {
    const input = document.getElementById(id) as HTMLInputElement | null;
    input?.addEventListener("change", () => edit((switchImage) => {
      const value = parseInt(input.value, 10);
      apply(switchImage, input.value.trim() === "" || !Number.isFinite(value) ? undefined : value);
    }));
  };

  onSelect("prop-layout-switch-off", (switchImage, imageId) => {
    if (imageId) switchImage.offImageId = imageId;
    else delete switchImage.offImageId;
    // The off picture sets the switch's shape.
    const { width, height } = imageSize(imageId);
    if (width && height) switchImage.aspect = Math.round((height / width) * 10000) / 10000;
    else delete switchImage.aspect;
  });
  onSelect("prop-layout-switch-on", (switchImage, imageId) => {
    if (imageId) switchImage.onImageId = imageId;
    else delete switchImage.onImageId;
  });
  onSelect("prop-layout-switch-strip", (switchImage, imageId) => {
    if (!imageId) {
      delete switchImage.strip;
      return;
    }
    const { width, height } = imageSize(imageId);
    switchImage.strip = stripFor(imageId, width, height, switchImage.strip?.frames ?? 2);
  });
  onNumber("prop-layout-switch-strip-frames", (switchImage, value) => {
    if (!switchImage.strip || value === undefined) return;
    const { width, height } = imageSize(switchImage.strip.imageId);
    switchImage.strip = stripFor(switchImage.strip.imageId, width, height, Math.max(2, Math.min(1024, value)));
  });
  onNumber("prop-layout-switch-width", (switchImage, value) => {
    if (value === undefined) delete switchImage.width;
    else switchImage.width = Math.max(8, Math.min(512, value));
  });
  onNumber("prop-layout-switch-height", (switchImage, value) => {
    if (value === undefined) delete switchImage.height;
    else switchImage.height = Math.max(8, Math.min(512, value));
  });
}
