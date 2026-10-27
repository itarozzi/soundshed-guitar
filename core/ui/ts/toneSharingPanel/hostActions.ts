/**
 * Tone Sharing's calls into app-level preset and navigation workflows. The app
 * composes these at bootstrap so this feature does not import its consumers.
 */

import type {
  buildImportSummaryMessage,
  buildToneSharingPresetArchiveBlobs,
  importPresetArchive,
} from "../presets/archive.js";
import type { importArchiveRemoteResources, readArchiveRemoteRefs } from "../presets/remoteArchiveResources.js";
import type { populatePresetDropdown, renderActivePreset } from "../presets/filter.js";
import type { switchMainPanel } from "../navigation.js";

export interface ToneSharingHostActions {
  buildImportSummaryMessage: typeof buildImportSummaryMessage;
  buildToneSharingPresetArchiveBlobs: typeof buildToneSharingPresetArchiveBlobs;
  importArchiveRemoteResources: typeof importArchiveRemoteResources;
  importPresetArchive: typeof importPresetArchive;
  populatePresetDropdown: typeof populatePresetDropdown;
  readArchiveRemoteRefs: typeof readArchiveRemoteRefs;
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
