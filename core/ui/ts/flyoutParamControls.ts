/**
 * Parameter rows for a settings flyout, built from the engine's own parameter definitions:
 * a parameter added to an effect arrives in the catalogue with its range, unit and labels, and
 * appears in every flyout built here with no UI change. Each row is label | control | value.
 * The control is a knob, a switch for a two-way parameter, or a picker for one the engine marks
 * `control: "picker"` (enumPicker.ts), which spans the control and value columns.
 *
 * The rows hold no state of their own. A binding says where values come from and where a change
 * goes, so the same rows drive the global noise gate (gateSettings.ts) and Pickup Match's tab in
 * the input calibration flyout (pickupMatchGlobal.ts). Styles: css/flyout-params.css.
 */

import { GenericKnob } from "./knob.js";
import { enumLabel } from "./paramLabels.js";
import type { ParameterDef } from "./presetV2.js";
import { buildEnumPickerButtonHtml, isPickerParam } from "./signalPath/paramsPanel/enumPicker.js";
import { escapeHtml } from "./utils.js";

/** Pixels of drag for a full sweep, whatever the parameter's span. */
export const KNOB_SWEEP_PIXELS = 200;

export interface FlyoutParamBinding {
  /** Identifies the knobs to MIDI learn and the pickers to their node. */
  idPrefix: string;
  /** The current value of a parameter. */
  value(def: ParameterDef): number;
  /** Sends a change: `commit` is false while a knob is moving and true when it is let go. */
  apply(key: string, value: number, commit: boolean): void;
}

/**
 * A two-label 0..1 parameter is a switch however the engine spells it: the gate's Stereo Link
 * is `{0,1}` with `{"Independent","Linked"}` rather than unit "toggle".
 */
export function isSwitchParam(def: ParameterDef): boolean {
  return def.unit === "toggle" || (def.labels?.length === 2 && def.min === 0 && def.max === 1);
}

/**
 * Decimals from the span, not the unit: 0.1–50 ms wants one place where 0–500 ms reads better
 * with none, and both are "ms".
 */
function valueDecimals(def: ParameterDef): number {
  const span = Math.abs(def.max - def.min);
  if (span <= 2) return 2;
  if (span <= 60) return 1;
  return 0;
}

export function knobStep(def: ParameterDef): number {
  return def.step && def.step > 0 ? def.step : Math.pow(10, -valueDecimals(def));
}

export function formatFlyoutParamValue(value: number, def: ParameterDef): string {
  if (isSwitchParam(def)) {
    return def.labels?.[value >= 0.5 ? 1 : 0] ?? (value >= 0.5 ? "On" : "Off");
  }
  if (def.labels?.length) {
    return enumLabel(value, def.labels, def.min, def.step) ?? `${Math.round(value)}`;
  }
  const text = value.toFixed(valueDecimals(def));
  // "amount" is the catalogue's word for no unit, as the params panel reads it.
  return def.unit && def.unit !== "amount" ? `${text} ${def.unit}` : text;
}

/** One row's markup. */
export function buildFlyoutParamRow(def: ParameterDef, binding: FlyoutParamBinding): string {
  const label = escapeHtml(def.name || def.key);
  const value = binding.value(def);
  const display = escapeHtml(formatFlyoutParamValue(value, def));
  const key = escapeHtml(def.key);

  if (isPickerParam(def)) {
    return `
      <div class="flyout-param-row is-picker">
        <span class="flyout-param-label">${label}</span>
        ${buildEnumPickerButtonHtml(binding.idPrefix, def, value)}
      </div>`;
  }

  if (isSwitchParam(def)) {
    return `
      <div class="flyout-param-row is-switch">
        <span class="flyout-param-label">${label}</span>
        <label class="toggle-switch">
          <input type="checkbox" class="flyout-param-toggle" data-param-key="${key}" ${value >= 0.5 ? "checked" : ""} />
          <span class="toggle-slider"></span>
        </label>
        <span class="flyout-param-value" data-param-key="${key}">${display}</span>
      </div>`;
  }

  return `
    <div class="flyout-param-row">
      <span class="flyout-param-label">${label}</span>
      <div class="knob flyout-param-knob" data-param-key="${key}" data-value="${value}">
        <div class="knob-indicator"></div>
      </div>
      <span class="flyout-param-value" data-param-key="${key}">${display}</span>
    </div>`;
}

export function buildFlyoutParamRows(defs: readonly ParameterDef[], binding: FlyoutParamBinding): string {
  return defs.map((def) => buildFlyoutParamRow(def, binding)).join("");
}

/**
 * Brings the knobs and switches under `root` to life. Pickers are bound by their owner
 * (bindEnumPickerTriggers), which knows what choosing one means. Returns the knobs, by key.
 */
export function bindFlyoutParamRows(
  root: ParentNode,
  defs: readonly ParameterDef[],
  binding: FlyoutParamBinding,
  onKnobChange?: (key: string, value: number) => void,
): Map<string, GenericKnob> {
  const knobs = new Map<string, GenericKnob>();

  defs.forEach((def) => {
    const valueEl = root.querySelector<HTMLElement>(`.flyout-param-value[data-param-key="${def.key}"]`);

    if (isSwitchParam(def)) {
      const input = root.querySelector<HTMLInputElement>(`.flyout-param-toggle[data-param-key="${def.key}"]`);
      input?.addEventListener("change", () => {
        const value = input.checked ? 1 : 0;
        binding.apply(def.key, value, true);
        if (valueEl) {
          valueEl.textContent = formatFlyoutParamValue(value, def);
        }
      });
      return;
    }

    const knobEl = root.querySelector<HTMLElement>(`.flyout-param-knob[data-param-key="${def.key}"]`);
    if (!knobEl) {
      return;
    }

    const span = Math.abs(def.max - def.min);
    knobs.set(def.key, new GenericKnob({
      knobElement: knobEl,
      paramId: `${binding.idPrefix}_${def.key}`,
      minValue: def.min,
      maxValue: def.max,
      defaultValue: def.default,
      stepValue: knobStep(def),
      sensitivity: span > 0 ? span / KNOB_SWEEP_PIXELS : 0.5,
      displayFormat: (value) => formatFlyoutParamValue(value, def),
      valueDisplay: valueEl,
      sendParameter: false,
      onValueChange: (value) => {
        binding.apply(def.key, value, false);
        onKnobChange?.(def.key, value);
      },
      onValueCommit: (value) => binding.apply(def.key, value, true),
    }));
  });

  return knobs;
}

/** Pushes the binding's current values back into the rows under `root`. */
export function refreshFlyoutParamRows(
  root: ParentNode,
  defs: readonly ParameterDef[],
  knobs: ReadonlyMap<string, GenericKnob>,
  binding: FlyoutParamBinding,
): void {
  defs.forEach((def) => {
    const value = binding.value(def);
    const knob = knobs.get(def.key);
    if (knob) {
      knob.setValue(value);
      return;
    }

    const input = root.querySelector<HTMLInputElement>(`.flyout-param-toggle[data-param-key="${def.key}"]`);
    if (input) {
      input.checked = value >= 0.5;
    }
    const valueEl = root.querySelector<HTMLElement>(`.flyout-param-value[data-param-key="${def.key}"]`);
    if (valueEl) {
      valueEl.textContent = formatFlyoutParamValue(value, def);
    }
  });
}
