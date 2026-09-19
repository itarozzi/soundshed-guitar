/**
 * Reading an app setting, and binding a control to one.
 *
 * Every numeric setting goes out through the same clamp, so a control that is
 * typed into, spun, or restored from disk cannot put a value past its bounds
 * into the engine.
 */

import { getAppSetting, updateAppSetting } from "../appSettingsStore.js";
import type { AppSettingValue } from "../types.js";

export function sanitizeNumericSetting(value: number, min: number, max: number, fallback: number): number {
  if (!Number.isFinite(value)) {
    return fallback;
  }
  return Math.min(max, Math.max(min, value));
}

export function bindImmediateNumericSetting(
  input: HTMLInputElement | null,
  key: string,
  min: number,
  max: number,
  fallback: number,
): void {
  if (!input || input.dataset.bound === "true") return;
  input.dataset.bound = "true";

  const applyValue = (normalizeText: boolean) => {
    const parsed = Number.parseFloat(input.value);
    if (!Number.isFinite(parsed)) {
      if (normalizeText) {
        const stored = Number(getSettingValue(key));
        input.value = sanitizeNumericSetting(stored, min, max, fallback).toFixed(1);
      }
      return;
    }

    const sanitized = sanitizeNumericSetting(parsed, min, max, fallback);
    updateAppSetting(key, sanitized);

    if (normalizeText) {
      input.value = sanitized.toFixed(1);
    }
  };

  input.addEventListener("input", () => applyValue(false));
  input.addEventListener("change", () => applyValue(true));
  input.addEventListener("blur", () => applyValue(true));
}

/// Binds a <select> whose option values are integer indices into an enum. The
/// stored setting is the index itself, matching what the DSP side expects.
export function bindIndexSelectSetting(
  select: HTMLSelectElement | null,
  key: string,
  min: number,
  max: number,
  fallback: number,
): void {
  if (!select || select.dataset.bound === "true") return;
  select.dataset.bound = "true";

  select.addEventListener("change", () => {
    const sanitized = Math.round(
      sanitizeNumericSetting(Number.parseInt(select.value, 10), min, max, fallback),
    );
    updateAppSetting(key, sanitized);
    select.value = String(sanitized);
  });
}

export function restoreIndexSelectSetting(
  select: HTMLSelectElement | null,
  key: string,
  min: number,
  max: number,
  fallback: number,
): void {
  if (!select) return;
  const sanitized = Math.round(
    sanitizeNumericSetting(Number(getSettingValue(key)), min, max, fallback),
  );
  select.value = String(sanitized);
}

export function getSettingValue(key: string): AppSettingValue {
  return getAppSetting(key);
}
