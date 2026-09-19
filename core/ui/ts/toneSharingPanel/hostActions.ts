/**
 * Tone Sharing's calls into app-level preset and navigation workflows. The app
 * composes these at bootstrap so this feature does not import its consumers.
 */

import type {
  buildToneSharingPresetArchiveBlobs,
  importPackWithConfirmation,
  importPresetArchive,
} from "../presets/archive.js";
import type { populatePresetDropdown, renderActivePreset } from "../presets/library.js";
import type { switchMainPanel } from "../navigation.js";

export interface ToneSharingHostActions {
  buildToneSharingPresetArchiveBlobs: typeof buildToneSharingPresetArchiveBlobs;
  importPackWithConfirmation: typeof importPackWithConfirmation;
  importPresetArchive: typeof importPresetArchive;
  populatePresetDropdown: typeof populatePresetDropdown;
  renderActivePreset: typeof renderActivePreset;
  switchMainPanel: typeof switchMainPanel;
}

let hostActions: ToneSharingHostActions | null = null;

export function configureToneSharingHostActions(actions: ToneSharingHostActions): void {
  hostActions = actions;
}

export function getToneSharingHostActions(): ToneSharingHostActions {
  if (!hostActions) {
    throw new Error("Tone Sharing host actions were not configured");
  }
  return hostActions;
}
