/**
 * The "Use multiple CPU cores" toggle in Settings → Performance.
 *
 * The settings store is shared by the standalone app and every plugin instance, and the two want
 * different defaults: on in the standalone, off in a DAW, which already spreads its tracks over
 * its own audio threads, so ours only compete with them. So there are two keys, and the toggle
 * shows and writes the one for where this UI is running. The engine reads the same pair
 * (SettingsKeys.h).
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { uiState } from "../state.js";
import { getSettingValue } from "./values.js";

export const MULTI_CORE_STANDALONE_SETTING = "audio.processing.multiCore.standalone";
export const MULTI_CORE_PLUGIN_SETTING = "audio.processing.multiCore.plugin";

function isStandalone(): boolean {
  return Boolean(uiState.environment?.standalone);
}

function multiCoreSettingKey(): string {
  return isStandalone() ? MULTI_CORE_STANDALONE_SETTING : MULTI_CORE_PLUGIN_SETTING;
}

function isMultiCoreEnabled(): boolean {
  const stored = getSettingValue(multiCoreSettingKey());
  return typeof stored === "boolean" ? stored : isStandalone();
}

function multiCoreToggle(): HTMLInputElement | null {
  return document.getElementById("multi-core-processing-toggle") as HTMLInputElement | null;
}

export function initMultiCoreToggle(): void {
  const toggle = multiCoreToggle();
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.checked = isMultiCoreEnabled();
  toggle.addEventListener("change", () => {
    // The key is chosen at the moment of the change: the environment arrives with the state,
    // after the panel binds.
    updateAppSetting(multiCoreSettingKey(), Boolean(toggle.checked));
  });
}

/** Shows the stored value for this host; settings and environment arrive after the panel binds. */
export function refreshMultiCoreToggle(): void {
  const toggle = multiCoreToggle();
  if (toggle) {
    toggle.checked = isMultiCoreEnabled();
  }
}
