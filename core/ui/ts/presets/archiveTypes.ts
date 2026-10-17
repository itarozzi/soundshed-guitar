import type { BlendDefinition, Preset, StoredEffectPreset } from "../types.js";
import type { InstalledPackMetadata } from "../toneSharingPanel.js";
import type { RemoteResourceRef } from "../remoteResources.js";
export type PresetArchiveResource = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  fileName: string;
  hash?: string;
  metadata?: Record<string, string>;
};

/**
 * Reference to a tone3000-sourced resource that must be re-downloaded by the
 * recipient using their own API key, as redistribution is prohibited by tone3000 terms.
 */
export type Tone3000ResourceRef = {
  id: string;
  name?: string;
  category?: string;
  type: string;
  modelUrl?: string;
  toneId?: string;
  modelId?: string;
  creatorId?: string;
  creatorName?: string;
};

export type PresetArchiveFolder = {
  name: string;
  presetIds?: string[];
  children?: PresetArchiveFolder[];
};

export type PresetArchive = {
  formatVersion: number;
  preset: Preset;
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
  /** Remote resources named rather than carried, from any provider (remoteArchiveResources.ts). */
  remoteResources?: RemoteResourceRef[];
  /** The Tone3000 ones again, for app versions that read only this list. */
  tone3000Resources?: Tone3000ResourceRef[];
};

export type PresetCollectionArchive = {
  formatVersion: number;
  createdAt: string;
  presets: Preset[];
  resources: PresetArchiveResource[];
  blends?: BlendDefinition[];
  presetFolders?: PresetArchiveFolder[];
  /** Remote resources named rather than carried, from any provider (remoteArchiveResources.ts). */
  remoteResources?: RemoteResourceRef[];
  /** The Tone3000 ones again, for app versions that read only this list. */
  tone3000Resources?: Tone3000ResourceRef[];
  /**
   * Effect presets by effect type, on the archive's resource and blend ids. A factory archive's
   * become factory presets for those effects; imports and archive sessions ignore them.
   */
  effectPresets?: Record<string, StoredEffectPreset[]>;
};

export type ImportPackSource = "zipImport" | "toneSharingApi" | "generatedPack";

export type ImportPackContext = {
  source: ImportPackSource;
  packId?: string;
  itemId?: string;
  creatorId?: string;
  creatorHandle?: string;
  titleHint?: string;
};

export type ImportPackSummary = {
  format: "generatedPack" | "presetArchive";
  title: string;
  presetCount: number;
  resourceCount: number;
  blendCount: number;
  tone3000ResourceCount: number;
  packId?: string;
};

export type ImportPackWithConfirmationOptions = {
  skipConfirmation?: boolean;
};

export type ArchiveImportContext = {
  source: InstalledPackMetadata["source"];
  packId?: string;
  itemId?: string;
  creatorId?: string;
  creatorHandle?: string;
  titleHint?: string;
};

/** A model or IR a preview plays from a file the engine stages for it, never from the library. */
export interface PreviewResource {
  resourceType: string;
  /** The id the preset's nodes name it by. */
  resourceId: string;
  name: string;
  /** The file, base64. */
  data: string;
}

/** What a preview needs besides the preset: the files the library lacks, the archive's blends, and what it could not get. */
export interface ArchivePreview {
  resources: PreviewResource[];
  /** The archive's blends under their own ids, their models on the library's ids where it has them. */
  blends: BlendDefinition[];
  /** Names of the remote models and IRs that could not be fetched; the preview plays without them. */
  unavailable: string[];
}

export type ArchiveImportOptions = {
  previewOnly?: boolean;
  /**
   * With previewOnly: filled with the archive's files the library lacks, its remote ones fetched
   * from their providers, and its blends, for the engine to play ("previewPreset"). Nothing is
   * imported or saved.
   */
  preview?: ArchivePreview;
  suppressNotifications?: boolean;
  topLevelFolderName?: string;
};

export interface GeneratorPackManifest {
  packId?: string;
  packVersion?: string;
  formatVersion?: number;
}

export interface GeneratorResourceIndex {
  items: GeneratorResourceIndexItem[];
}

export interface GeneratorPresetV2 {
  id: string;
  name: string;
  category?: string;
  description?: string;
  tags?: string[];
  global?: { inputTrim?: number; outputTrim?: number };
  graph: {
    nodes: GeneratorPackNode[];
    edges: Array<{ from: string; to: string }>;
  };
}

export interface GeneratorResourceIndexItem {
  resourceId: string;
  resourceType: "nam" | "ir";
  provider: string;
  contentHash: string;
  fileExt: string;
  filePath: string;
  displayName: string;
  originalFileName: string;
}

export interface GeneratorPackNode {
  id: string;
  type: string;
  params?: Record<string, number>;
  resource?: { resourceType: string; resourceId: string };
}
