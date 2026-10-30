/**
 * The designer's settings for generated control shadows (layoutShadows.ts): the layout's light,
 * in the canvas panel's Shadows group, and each knob's or toggle's Cast Shadow switch.
 */

import { CONTROL_SHADOW_DEFAULTS, usableControlShadow } from "../layoutShadows.js";
import type { EffectLayout, LayoutControl, LayoutControlShadow } from "../layoutTypes.js";
import { escapeHtml } from "../utils.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

const DIRECTIONS: ReadonlyArray<[number, string]> = [
  [0, "Top"], [45, "Top right"], [90, "Right"], [135, "Bottom right"],
  [180, "Bottom"], [225, "Bottom left"], [270, "Left"], [315, "Top left"],
];

function numberRow(id: string, label: string, value: number, min: number, max: number, step: number, title: string): string {
  return `
        <div class="layout-property-row">
          <span class="layout-property-label">${escapeHtml(label)}</span>
          <div class="layout-property-input">
            <input type="number" id="${escapeHtml(id)}" min="${min}" max="${max}" step="${step}" value="${value}" title="${escapeHtml(title)}">
          </div>
        </div>`;
}

/** The canvas panel's Shadows group: on or off, then where the light is and how the shadows look. */
export function renderShadowGroup(layout: EffectLayout): string {
  const light = usableControlShadow(layout.controlShadow);
  let rows = `
        <div class="layout-property-row">
          <span class="layout-property-label">Cast Shadows</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-shadow-on" ${light ? "checked" : ""} title="Knobs and toggles cast shadows from one light">
          </div>
        </div>`;
  if (light) {
    const angle = Math.round(light.angle);
    const options = DIRECTIONS.some(([value]) => value === angle) ? DIRECTIONS : [...DIRECTIONS, [angle, `${angle}°`] as [number, string]];
    rows += `
        <div class="layout-property-row">
          <span class="layout-property-label">Light From</span>
          <div class="layout-property-input">
            <select id="prop-shadow-angle" title="Where the light comes from; shadows fall the other way">
              ${options.map(([value, label]) => `<option value="${value}" ${value === angle ? "selected" : ""}>${escapeHtml(label)}</option>`).join("")}
            </select>
          </div>
        </div>`
      + numberRow("prop-shadow-elevation", "Light Height (°)", Math.round(light.elevation), 15, 80, 5, "How high the light is above the panel: lower casts longer shadows")
      + numberRow("prop-shadow-softness", "Softness (%)", Math.round(light.softness * 100), 0, 100, 5, "How blurred the shadows are")
      + numberRow("prop-shadow-opacity", "Darkness (%)", Math.round(light.opacity * 100), 0, 100, 5, "How dark the shadows are");
  }
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Shadows</div>
        <div class="layout-property-hint">
          Each knob and toggle stands as tall as it is wide and casts a shadow away from the light.
        </div>${rows}
      </div>`;
}

export function bindShadowGroup(layout: EffectLayout, host: LayoutPropertiesHost): void {
  const edit = (change: (light: LayoutControlShadow) => void) => {
    host.pushSidebarUndoOnce();
    layout.controlShadow ??= {};
    change(layout.controlShadow);
    host.renderCanvas();
  };
  const toggle = document.getElementById("prop-shadow-on") as HTMLInputElement | null;
  toggle?.addEventListener("change", () => {
    host.pushSidebarUndoOnce();
    if (toggle.checked) layout.controlShadow = { ...CONTROL_SHADOW_DEFAULTS };
    else delete layout.controlShadow;
    host.renderCanvas();
    host.renderSidebar();
  });
  const angle = document.getElementById("prop-shadow-angle") as HTMLSelectElement | null;
  angle?.addEventListener("change", () => edit((light) => { light.angle = Number(angle.value); }));
  const onNumber = (id: string, min: number, max: number, apply: (light: LayoutControlShadow, value: number) => void) => {
    const input = document.getElementById(id) as HTMLInputElement | null;
    input?.addEventListener("change", () => {
      const value = parseFloat(input.value);
      if (!Number.isFinite(value)) return;
      edit((light) => apply(light, Math.max(min, Math.min(max, value))));
    });
  };
  onNumber("prop-shadow-elevation", 15, 80, (light, value) => { light.elevation = value; });
  onNumber("prop-shadow-softness", 0, 100, (light, value) => { light.softness = value / 100; });
  onNumber("prop-shadow-opacity", 0, 100, (light, value) => { light.opacity = value / 100; });
}

/** Whether `control` is drawn as a knob or a toggle, the controls that cast shadows. */
function castsShadows(control: LayoutControl, isResourceControl: boolean): boolean {
  return !isResourceControl && control.type !== "slider";
}

/** A knob's or toggle's Cast Shadow switch, when the layout casts shadows. */
export function renderControlShadowRow(control: LayoutControl, layout: EffectLayout, isResourceControl: boolean): string {
  if (!usableControlShadow(layout.controlShadow) || !castsShadows(control, isResourceControl)) return "";
  return `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Shadow</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Cast Shadow</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-cast-shadow" ${control.style?.castShadow !== false ? "checked" : ""} title="Cast a shadow from the layout's light">
          </div>
        </div>
      </div>`;
}

export function bindControlShadowRow(control: LayoutControl, host: LayoutPropertiesHost): void {
  const check = document.getElementById("prop-cast-shadow") as HTMLInputElement | null;
  check?.addEventListener("change", () => {
    host.pushSidebarUndoOnce();
    control.style ??= {};
    if (check.checked) delete control.style.castShadow;
    else control.style.castShadow = false;
    host.renderCanvas();
  });
}
