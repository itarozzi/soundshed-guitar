/**
 * Suppressing the echo of a preset this window just saved.
 *
 * The host broadcasts state to every instance, so a save comes straight back as
 * a state message. Applying it would overwrite the edit in progress, so the id
 * is marked for a short window and the next state carrying it is ignored.
 */

import type { Preset } from "../types.js";

let ignoreNextStatePresetId: string | null = null;

let ignoreNextStatePresetExpiresAtMs = 0;

export function markIgnoreNextStatePreset(id: string): void {
  const presetId = id.trim();
  if (!presetId) {
    return;
  }
  ignoreNextStatePresetId = presetId;
  ignoreNextStatePresetExpiresAtMs = Date.now() + 1500;
}

export function shouldIgnoreStatePreset(incoming: Preset): boolean {
  const incomingId = incoming.id?.trim() ?? "";
  if (!incomingId || !ignoreNextStatePresetId) {
    return false;
  }

  if (incomingId !== ignoreNextStatePresetId) {
    return false;
  }

  const stillValid = Date.now() <= ignoreNextStatePresetExpiresAtMs;
  // One-shot guard: clear once a matching state payload is observed.
  ignoreNextStatePresetId = null;
  ignoreNextStatePresetExpiresAtMs = 0;
  return stillValid;
}
