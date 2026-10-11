/**
 * Shapes the tone builder reads and writes. Types only.
 *
 * A *proposal* describes presets as chains of effects by name, with parameter values, factory
 * effect presets and the resources (NAM captures, IRs) each node uses. The builder turns one into
 * a Preset V2 against an *inventory*: the effect catalogue and resource library it may use.
 *
 * Nothing here is tied to one resource provider. A resource is named by its library id
 * (`<provider>:<key>`), its content hash, or its name; where it came from, who made it and its
 * licence ride along as data.
 */

/** A parameter as the effect catalogue describes it. */
export interface InventoryParameter {
  key: string;
  name?: string;
  min: number;
  max: number;
  default: number;
  unit?: string | null;
  step?: number | null;
  /** Enum labels, indexed from `min`. */
  labels?: string[] | null;
}

/** A factory effect preset: a named set of parameter values. */
export interface InventoryEffectPreset {
  id?: string;
  name: string;
  source?: string | null;
  parameters?: Record<string, number | string | boolean>;
}

export interface InventoryEffect {
  type: string;
  displayName: string;
  category: string;
  catalogHidden?: boolean;
  requiresResource?: boolean;
  resourceType?: string | null;
  parameters: InventoryParameter[];
  presets: InventoryEffectPreset[];
}

/** A resource in the library, whatever provider it came from. */
export interface InventoryResource {
  /** Library id, `<provider>:<key>` for remote resources. */
  id: string;
  /** "nam", "ir", … */
  type: string;
  name: string;
  category?: string;
  fileMissing?: boolean;
  filePath?: string | null;
  /** Where it came from: "local", "presetArchive", a remote provider id, … */
  provider?: string | null;
  creator?: string | null;
  /** Content hash, when known. */
  hash?: string | null;
  /** Licence id (e.g. "cc-by"), when known. */
  licence?: string | null;
  /** "amp", "preamp", "amp+cab", "pedal", "cab", "room", when known. */
  captureScope?: string | null;
  /** Provider-specific detail, left opaque. */
  metadata?: Record<string, string | number | boolean | null>;
}

export interface BuildInventory {
  effects: InventoryEffect[];
  resources: InventoryResource[];
}

/** How a chain item names the resource it uses. */
export interface ResourceSpec {
  /** Library id. */
  id?: string;
  /** Content hash. */
  hash?: string;
  /** Library name (a local file is preferred when several share it). */
  name?: string;
  /** Resource type; defaults to what the effect takes. */
  type?: string;
  /** Lookup keys a caller understands (the content pipeline's curated picks, for one). */
  [extra: string]: unknown;
}

export interface NodeSpec {
  /** Effect display name or type id. */
  fx: string;
  /** Factory effect preset name, applied before `set`. */
  preset?: string;
  /** Parameter values by key: numbers, booleans or enum labels. */
  set?: Record<string, number | string | boolean>;
  label?: string;
  /** Build the node bypassed. */
  off?: boolean;
  resource?: ResourceSpec;
}

export interface SplitSpec {
  /** One chain per branch; an empty chain is a dry branch. */
  split: ChainItemSpec[][];
  /** Per-branch gain into the mixer (1 = unity, omitted). */
  gains?: number[];
  masterLevel?: number;
  label?: string;
}

export type ChainItemSpec = NodeSpec | SplitSpec;

export interface SceneSpec {
  key: string;
  title: string;
  chain: ChainItemSpec[];
  outputGainDb?: number;
}

export interface PresetSpec {
  key: string;
  /** Keeps a preset's id when its key changes. */
  idKey?: string;
  name: string;
  description: string;
  tags?: string[];
  /** One rig per preset: at most three scenes. */
  scenes?: SceneSpec[];
  /** Shorthand for a single "Main" scene. */
  chain?: ChainItemSpec[];
  outputGainDb?: number;
  [extra: string]: unknown;
}

/** Where built presets belong: the id scope, the category and the tags every preset gets. */
export interface PresetCollection {
  /** Scopes the ids, e.g. "soundshed-preset-packs/<slug>". */
  idScope: string;
  category: string;
  /** Added as a `pack:<slug>` tag when set. */
  slug?: string;
  tags?: string[];
}

export interface BuiltNode {
  id: string;
  type: string;
  category: string;
  label: string;
  params: Record<string, number>;
  enabled?: boolean;
  resources?: Array<{ resourceType: string; resourceId: string }>;
}

export interface BuiltEdge {
  from: string;
  to: string;
  fromPort?: number;
  toPort?: number;
  gain?: number;
}

export interface BuiltGraph {
  nodes: BuiltNode[];
  edges: BuiltEdge[];
}

export interface BuiltPreset {
  id: string;
  version: 2;
  name: string;
  category: string;
  description: string;
  tags: string[];
  scenes: Array<{ id: string; title: string; graph: BuiltGraph }>;
}

/** What a build drew on, for guides and credits. */
export interface BuildUsage {
  effects: string[];
  fxPresets: string[];
  /** One credit line per resource. */
  resources: string[];
}

/** A resource the builder matched, with its credit line and the short name used in node labels. */
export interface ResolvedResource {
  resource: InventoryResource;
  note: string;
  short: string;
}
