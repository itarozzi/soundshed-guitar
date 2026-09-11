/**
 * Mixer slot state, and the deep link that opens the tone sharing panel on a
 * shared item.
 */

import { uiState } from "../state.js";
import { handleToneSharingDeepLink } from "../toneSharingPanel.js";
import type { IncomingPayload } from "./types.js";

export function onNavigateToToneSharingDeepLink(payload: IncomingPayload): void {
  const deepLink = payload as { deepLink?: string };
  if (deepLink.deepLink) {
    handleToneSharingDeepLink(deepLink.deepLink);
  }
}

// Optional: handle full mixer state sync from plugin
export function handleMixerStateMessage(message: Record<string, unknown>): void {
  const mixer = message as { activePresetIds?: string[]; presets?: Record<string, unknown>; masterGain?: number; mixGainDb?: number };
  uiState.mixer = uiState.mixer ?? { activePresetIds: [], presets: {}, masterGain: 1.0, mixGainDb: 0 };
  if (Array.isArray(mixer.activePresetIds)) {
    uiState.mixer.activePresetIds = mixer.activePresetIds.slice();
  }
  if (typeof mixer.masterGain === "number") {
    uiState.mixer.masterGain = mixer.masterGain as number;
  }
  if (typeof mixer.mixGainDb === "number") {
    uiState.mixer.mixGainDb = mixer.mixGainDb;
  }
  // Merge per-preset states if provided
  if (mixer.presets && typeof mixer.presets === "object") {
    for (const [pid, st] of Object.entries(mixer.presets)) {
      const ps = st as { mix?: number; pan?: number; mute?: boolean; solo?: boolean };
      const cur = uiState.mixer.presets[pid] || { id: pid, mix: 1.0, pan: 0.0, mute: false, solo: false };
      uiState.mixer.presets[pid] = {
        id: pid,
        mix: typeof ps.mix === "number" ? ps.mix : cur.mix,
        pan: typeof ps.pan === "number" ? ps.pan : cur.pan,
        mute: typeof ps.mute === "boolean" ? ps.mute : cur.mute,
        solo: typeof ps.solo === "boolean" ? ps.solo : cur.solo,
      };
    }
  }
}

/**
 * Send a global signal chain parameter change to the plugin.
 * @param paramPath - Dot-notation path to the parameter (e.g., "postChainGraph.global_eq.params.lowGain")
 * @param value - The new value for the parameter
 */
