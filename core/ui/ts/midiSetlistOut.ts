/**
 * The MIDI panel's switch for telling the MIDI device about a bank or setlist slot
 * picked in the app, with its channel and bank select mode. The engine reads the
 * three settings when a pick is made (its SetlistMidiOut); nothing is sent for a
 * pick that came from the device or the DAW.
 */

import { getAppSetting, updateAppSetting } from "./appSettingsStore.js";

/** Absent means off. */
export const SETLIST_MIDI_OUT_ENABLED_SETTING = "midi.setlistOut.enabled";
/** 1..16; absent means 1. */
export const SETLIST_MIDI_OUT_CHANNEL_SETTING = "midi.setlistOut.channel";
/** "msbLsb" (absent), "msb", "lsb" or "none". */
export const SETLIST_MIDI_OUT_BANK_SELECT_SETTING = "midi.setlistOut.bankSelect";

const BANK_SELECT_MODES = ["msbLsb", "msb", "lsb", "none"] as const;
const MIDI_CHANNEL_COUNT = 16;

export interface SetlistMidiOutSettings {
  enabled: boolean;
  channel: number;
  bankSelect: (typeof BANK_SELECT_MODES)[number];
}

/** The stored settings, with the engine's defaults for anything absent or out of range. */
export function readSetlistMidiOutSettings(): SetlistMidiOutSettings {
  const channel = getAppSetting(SETLIST_MIDI_OUT_CHANNEL_SETTING);
  const bankSelect = getAppSetting(SETLIST_MIDI_OUT_BANK_SELECT_SETTING);
  return {
    enabled: getAppSetting(SETLIST_MIDI_OUT_ENABLED_SETTING) === true,
    channel:
      typeof channel === "number" && Number.isInteger(channel) && channel >= 1 && channel <= MIDI_CHANNEL_COUNT
        ? channel
        : 1,
    bankSelect: BANK_SELECT_MODES.find((mode) => mode === bankSelect) ?? "msbLsb",
  };
}

interface Controls {
  enabled: HTMLInputElement;
  channel: HTMLSelectElement;
  bankSelect: HTMLSelectElement;
}

function getControls(): Controls | null {
  const enabled = document.getElementById("midi-setlist-out-enabled");
  const channel = document.getElementById("midi-setlist-out-channel");
  const bankSelect = document.getElementById("midi-setlist-out-bank-select");
  if (
    !(enabled instanceof HTMLInputElement) ||
    !(channel instanceof HTMLSelectElement) ||
    !(bankSelect instanceof HTMLSelectElement)
  ) {
    return null;
  }
  return { enabled, channel, bankSelect };
}

/** Shows the stored settings; the channel and mode are greyed out while the switch is off. */
export function syncSetlistMidiOutControls(): void {
  const controls = getControls();
  if (!controls) return;

  const settings = readSetlistMidiOutSettings();
  controls.enabled.checked = settings.enabled;
  controls.channel.value = String(settings.channel);
  controls.bankSelect.value = settings.bankSelect;
  controls.channel.disabled = !settings.enabled;
  controls.bankSelect.disabled = !settings.enabled;
}

export function wireSetlistMidiOutControls(): void {
  const controls = getControls();
  if (!controls) return;

  for (let channel = 1; channel <= MIDI_CHANNEL_COUNT; channel++) {
    controls.channel.add(new Option(String(channel), String(channel)));
  }
  syncSetlistMidiOutControls();

  // Each releases focus once changed: a focused control would hold keys the keyboard
  // mappings are listening for.
  controls.enabled.addEventListener("change", () => {
    updateAppSetting(SETLIST_MIDI_OUT_ENABLED_SETTING, controls.enabled.checked);
    syncSetlistMidiOutControls();
    controls.enabled.blur();
  });
  controls.channel.addEventListener("change", () => {
    updateAppSetting(SETLIST_MIDI_OUT_CHANNEL_SETTING, Number.parseInt(controls.channel.value, 10));
    controls.channel.blur();
  });
  controls.bankSelect.addEventListener("change", () => {
    updateAppSetting(SETLIST_MIDI_OUT_BANK_SELECT_SETTING, controls.bankSelect.value);
    controls.bankSelect.blur();
  });
}
