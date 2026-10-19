/**
 * The "Fast NAM Engine" toggle in Settings → DSP Performance.
 *
 * On (the default, and what an absent setting means), NAM models run on Soundshed's fused engine
 * wherever their shape allows; off, every model runs on the standard NAM engine. One key for the
 * standalone and plugins: the engine is process-wide, so every instance in a DAW shares it. The
 * engine rebuilds the running presets when it changes (SettingsKeys.h, kNamFusedEngineSettingKey).
 */

import { updateAppSetting } from "../appSettingsStore.js";
import { NAM_FUSED_ENGINE_SETTING } from "./keys.js";
import { getSettingValue } from "./values.js";

function isNamFusedEngineEnabled(): boolean {
  const stored = getSettingValue(NAM_FUSED_ENGINE_SETTING);
  return typeof stored === "boolean" ? stored : true;
}

function namEngineToggle(): HTMLInputElement | null {
  return document.getElementById("nam-fused-engine-toggle") as HTMLInputElement | null;
}

export function initNamEngineToggle(): void {
  const toggle = namEngineToggle();
  if (!toggle || toggle.dataset.bound === "true") return;
  toggle.dataset.bound = "true";
  toggle.checked = isNamFusedEngineEnabled();
  toggle.addEventListener("change", () => {
    updateAppSetting(NAM_FUSED_ENGINE_SETTING, Boolean(toggle.checked));
  });
}

/** Shows the stored value; settings arrive after the panel binds. */
export function refreshNamEngineToggle(): void {
  const toggle = namEngineToggle();
  if (toggle) {
    toggle.checked = isNamFusedEngineEnabled();
  }
}
