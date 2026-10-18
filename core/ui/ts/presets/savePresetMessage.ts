/**
 * The `savePreset` message, and what its `saveMode` asks of the engine.
 *
 * "overwrite", "save-as" and "save-new" save the working copy (or `preset`, when given) and
 * make the saved preset the active one; the engine answers `presetSaved`. "import" stores the
 * `preset` it must carry under `presetId` and leaves the preset playing alone; the engine
 * answers `presetImported`. A preset archive, a pack and the Tone Builder import.
 */

import type { GlobalSignalChainConfig, Preset } from "../types.js";

export type SavePresetMode = "overwrite" | "save-as" | "save-new" | "import";

export interface SavePresetMessage {
  type: "savePreset";
  saveMode?: SavePresetMode;
  /** The id to save under; the engine mints one when it is absent. */
  presetId?: string;
  name: string;
  category?: string;
  description?: string;
  /** The preset to save in place of the working copy; what an import stores. */
  preset?: Preset;
  /** "save-as": the preset copied from, which the saved one must not share an id with. */
  sourcePresetId?: string;
  requireNewPresetId?: boolean;
  /** The scene to make active in the saved preset. */
  sceneId?: string;
  includeGlobalSignalChain?: boolean;
  globalSignalChain?: GlobalSignalChainConfig;
}
