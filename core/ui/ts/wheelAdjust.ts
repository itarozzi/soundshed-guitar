/**
 * Whether the mouse wheel changes a control's value.
 *
 * On by default. Turned off, a wheel over a knob, slider or the metronome's
 * tempo readout is left alone, so it scrolls the page instead of nudging a
 * parameter the pointer happened to pass over.
 */

import { getAppSetting } from "./appSettingsStore.js";

export const WHEEL_ADJUST_SETTING = "ui.mouseWheelAdjustsControls";

export function isWheelAdjustEnabled(): boolean {
  const stored = getAppSetting(WHEEL_ADJUST_SETTING);
  return stored === null ? true : Boolean(stored);
}
