import { addLibraryPresetIfMissing, cachePreset } from "../presetLibraryStore.js";
import type { Preset } from "../types.js";
import { normalizePresetScenes } from "../presetScenes.js";
import { stripLegacyGlobals } from "./sanitize.js";
export function cachePresetInMemory(preset: Preset): void {
  const cleanedPreset = stripLegacyGlobals(preset);
  normalizePresetScenes(cleanedPreset);
  cachePreset(cleanedPreset);
  addLibraryPresetIfMissing(cleanedPreset);
}
