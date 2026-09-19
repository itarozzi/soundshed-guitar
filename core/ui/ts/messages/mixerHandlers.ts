/**
 * Mixer slot state, and the deep link that opens the tone sharing panel on a
 * shared item.
 */

import { mergeMixerState } from "../mixerStore.js";
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
  mergeMixerState({
    activePresetIds: Array.isArray(mixer.activePresetIds) ? mixer.activePresetIds : undefined,
    masterGain: mixer.masterGain,
    mixGainDb: mixer.mixGainDb,
    // Per-preset states, merged over what each slot already has.
    presets: mixer.presets && typeof mixer.presets === "object"
      ? (mixer.presets as Record<string, { mix?: number; pan?: number; mute?: boolean; solo?: boolean }>)
      : undefined,
  });
}

/**
 * Send a global signal chain parameter change to the plugin.
 * @param paramPath - Dot-notation path to the parameter (e.g., "postChainGraph.global_eq.params.lowGain")
 * @param value - The new value for the parameter
 */
