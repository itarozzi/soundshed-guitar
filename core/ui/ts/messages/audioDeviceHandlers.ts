/**
 * The standalone app's audio device settings: the snapshot the engine sends after
 * every request and every device change, and the input meter feed.
 */

import { markAudioDeviceSettingsUnavailable, normalizeAudioDeviceState, showAudioDeviceLevels, showAudioDeviceState } from "../settings/audioDevice.js";
import type { IncomingPayload } from "./types.js";

export function onAudioDeviceState(payload: IncomingPayload): void {
  if (payload.available === false) {
    markAudioDeviceSettingsUnavailable();
    return;
  }
  const state = normalizeAudioDeviceState(payload.state);
  if (state) {
    showAudioDeviceState(state, typeof payload.error === "string" ? payload.error : "");
  }
}

export function onAudioDeviceLevels(payload: IncomingPayload): void {
  showAudioDeviceLevels(Number(payload.input), Number(payload.xruns));
}
