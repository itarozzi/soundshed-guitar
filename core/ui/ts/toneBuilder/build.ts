/**
 * Builds Preset V2 objects from proposals (see ./types.ts) and validates them against an
 * inventory: effect names and type ids, parameter keys, ranges and enum labels, factory effect
 * preset names, and that every resource is in the library with its file present.
 *
 * It is pure: no DOM, no app state, no I/O. The app feeds it the live effect catalogue and
 * library (./appInventory.ts); scripts and the API feed it a saved inventory. The same spec
 * always builds the same ids, so a rebuild updates a saved preset rather than copying it.
 */

import { sha1Hex, uuidv5 } from "./ids.js";
import type {
  BuildInventory,
  BuildUsage,
  BuiltEdge,
  BuiltGraph,
  BuiltNode,
  BuiltPreset,
  ChainItemSpec,
  InventoryEffect,
  InventoryParameter,
  InventoryResource,
  NodeSpec,
  PresetCollection,
  PresetSpec,
  ResolvedResource,
  ResourceSpec,
  SplitSpec,
} from "./types.js";

const SPLITTER_TYPE = "f5f2541b-fcea-4cfd-9e62-eeddf583ef4e";
const MIXER_TYPE = "d7d1e40f-9c79-4582-9a82-d5fa5bbbfb97";
const SHORT_NAME_MAX = 80;

export class ProposalError extends Error {}

export interface BuildRegistryOptions {
  /**
   * Other display names an effect has gone by (lower case, both directions), so a spec written
   * with an older name still builds.
   */
  effectAliases?: ReadonlyMap<string, string>;
  /**
   * Resolves resource specs the builder does not understand itself (keys other than id, hash
   * and name). Return undefined to fall through to the built-in lookups.
   */
  resolveResource?: (spec: ResourceSpec, effect: InventoryEffect, type: string, registry: BuildRegistry) => ResolvedResource | undefined;
  /** The credit line for a resource; defaults to name, creator, licence and provider. */
  describeResource?: (resource: InventoryResource) => string;
}

export function defaultResourceNote(resource: InventoryResource): string {
  const maker = resource.creator ? ` by ${resource.creator}` : "";
  const origin = resource.provider && resource.provider !== "local" ? resource.provider : "local file";
  const licence = resource.licence ? `, ${resource.licence}` : "";
  return `${resource.name}${maker} (${origin}${licence})`;
}

export function shortResourceName(name: string, creator?: string | null): string {
  const short = `${name.trim()}${creator ? ` (${creator})` : ""}`;
  return short.length > SHORT_NAME_MAX ? `${short.slice(0, SHORT_NAME_MAX - 3).trimEnd()}...` : short;
}

export class BuildRegistry {
  readonly effects: InventoryEffect[];
  readonly resources: InventoryResource[];
  private readonly byName = new Map<string, InventoryEffect | null>();
  private readonly byType: Map<string, InventoryEffect>;
  private readonly options: BuildRegistryOptions;

  constructor(inventory: BuildInventory, options: BuildRegistryOptions = {}) {
    this.options = options;
    this.effects = inventory.effects;
    for (const effect of this.effects) {
      const key = effect.displayName.toLowerCase();
      // null marks a name two effects share; it can only be used by type id.
      this.byName.set(key, this.byName.has(key) ? null : effect);
    }
    this.byType = new Map(this.effects.map((effect) => [effect.type, effect]));
    this.resources = inventory.resources;
  }

  effect(ref: string): InventoryEffect {
    const name = String(ref).toLowerCase();
    const alias = this.options.effectAliases?.get(name);
    const effect = this.byType.get(ref) ?? this.byName.get(name) ?? (alias ? this.byName.get(alias) : undefined);
    if (!effect) {
      throw new ProposalError(`Unknown or ambiguous effect "${ref}"`);
    }
    return effect;
  }

  resource(spec: ResourceSpec, effect: InventoryEffect): ResolvedResource {
    const type = spec.type ?? effect.resourceType ?? "nam";
    const custom = this.options.resolveResource?.(spec, effect, type, this);
    let found: InventoryResource | null = custom?.resource ?? null;
    if (!custom) {
      if (spec.id) {
        found = this.resources.find((r) => r.id === spec.id && !r.fileMissing) ?? null;
      } else if (spec.hash) {
        found = this.resources.find((r) => r.type === type && r.hash === spec.hash && !r.fileMissing) ?? null;
      } else if (spec.name) {
        const named = this.resources.filter((r) => r.type === type && r.name === spec.name && !r.fileMissing);
        found = named.find((r) => r.provider === "local") ?? named[0] ?? null;
      }
    }
    if (!found) {
      throw new ProposalError(`Resource not found or file missing: ${JSON.stringify(spec)}`);
    }
    if (found.type !== type) {
      throw new ProposalError(`Resource ${found.id} is ${found.type}, the effect needs ${type}`);
    }
    if (custom) {
      return custom;
    }
    const describe = this.options.describeResource ?? defaultResourceNote;
    return { resource: found, note: describe(found), short: shortResourceName(found.name, found.creator) };
  }
}

interface UsageSets {
  effects: Set<string>;
  fxPresets: Set<string>;
  resources: Set<string>;
}

function paramValue(param: InventoryParameter, raw: number | string | boolean, where: string): number {
  let value: number | string | boolean = raw;
  if (typeof raw === "string") {
    const index = (param.labels ?? []).findIndex((label) => label.toLowerCase() === raw.toLowerCase());
    if (index < 0) {
      throw new ProposalError(`${where}: "${raw}" is not one of ${JSON.stringify(param.labels)}`);
    }
    value = param.min + index;
  }
  if (typeof value === "boolean") {
    value = value ? 1 : 0;
  }
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new ProposalError(`${where}: not a number`);
  }
  const epsilon = 1e-9;
  if (value < param.min - epsilon || value > param.max + epsilon) {
    throw new ProposalError(`${where}: ${value} outside ${param.min}..${param.max}`);
  }
  if (param.labels && !Number.isInteger(value)) {
    throw new ProposalError(`${where}: enum value ${value} is not an integer`);
  }
  return value;
}

function isSplit(item: ChainItemSpec): item is SplitSpec {
  return Array.isArray((item as SplitSpec).split);
}

function buildNode(item: NodeSpec, registry: BuildRegistry, nextId: () => string, used: UsageSets): BuiltNode {
  const effect = registry.effect(item.fx);
  const params: Record<string, number> = Object.fromEntries(effect.parameters.map((p) => [p.key, p.default]));
  const byKey = new Map(effect.parameters.map((p) => [p.key, p]));
  let presetName: string | null = null;
  if (item.preset) {
    const wanted = item.preset.toLowerCase();
    const preset = effect.presets.find((p) => p.name.toLowerCase() === wanted);
    if (!preset) {
      throw new ProposalError(`${effect.displayName} has no preset "${item.preset}" (has: ${effect.presets.map((p) => p.name).join(", ")})`);
    }
    for (const [key, value] of Object.entries(preset.parameters ?? {})) {
      const param = byKey.get(key);
      if (param) {
        params[key] = paramValue(param, value, `${effect.displayName}/${preset.name}.${key}`);
      }
    }
    presetName = preset.name;
    used.fxPresets.add(`${effect.displayName}/${preset.name}`);
  }
  for (const [key, value] of Object.entries(item.set ?? {})) {
    const param = byKey.get(key);
    if (!param) {
      throw new ProposalError(`${effect.displayName} has no parameter "${key}" (has: ${[...byKey.keys()].join(", ")})`);
    }
    params[key] = paramValue(param, value, `${effect.displayName}.${key}`);
  }
  if (effect.requiresResource && !item.resource) {
    throw new ProposalError(`${effect.displayName} needs a resource`);
  }
  const resources: Array<{ resourceType: string; resourceId: string }> = [];
  let resourceName: string | null = null;
  if (item.resource) {
    const resolved = registry.resource(item.resource, effect);
    resources.push({ resourceType: resolved.resource.type, resourceId: resolved.resource.id });
    used.resources.add(resolved.note);
    resourceName = resolved.short;
  }
  used.effects.add(effect.displayName);
  const node: BuiltNode = {
    id: nextId(),
    type: effect.type,
    category: effect.category,
    label: item.label ?? (presetName
      ? `${effect.displayName} · ${presetName}`
      : resourceName ? `${effect.displayName} · ${resourceName}` : effect.displayName),
    params,
  };
  if (item.off) {
    node.enabled = false;
  }
  if (resources.length) {
    node.resources = resources;
  }
  return node;
}

interface BuiltChain {
  nodes: BuiltNode[];
  edges: BuiltEdge[];
  first: string | null;
  last: string | null;
}

function buildChain(items: ChainItemSpec[], registry: BuildRegistry, nextId: () => string, used: UsageSets): BuiltChain {
  const nodes: BuiltNode[] = [];
  const edges: BuiltEdge[] = [];
  let first: string | null = null;
  let last: string | null = null;
  const link = (from: string, to: string, extra: Partial<BuiltEdge> = {}): void => {
    edges.push({ from, to, ...extra });
  };
  for (const item of items) {
    if (isSplit(item)) {
      const splitter: BuiltNode = { id: nextId(), type: SPLITTER_TYPE, category: "utility", label: item.label ? `${item.label} split` : "Split", params: {} };
      const mixer: BuiltNode = {
        id: nextId(), type: MIXER_TYPE, category: "utility", label: item.label ? `${item.label} mix` : "Mix",
        params: { masterLevel: item.masterLevel ?? 0 },
      };
      used.effects.add("Splitter").add("Mixer");
      nodes.push(splitter);
      item.split.forEach((branch, port) => {
        const portOut = port ? { fromPort: port } : {};
        const portIn = port ? { toPort: port } : {};
        const gain = item.gains?.[port];
        const gainProp = gain !== undefined && gain !== 1 ? { gain } : {};
        if (!branch.length) {
          link(splitter.id, mixer.id, { ...portOut, ...portIn, ...gainProp });
          return;
        }
        const built = buildChain(branch, registry, nextId, used);
        nodes.push(...built.nodes);
        edges.push(...built.edges);
        link(splitter.id, built.first as string, portOut);
        link(built.last as string, mixer.id, { ...portIn, ...gainProp });
      });
      nodes.push(mixer);
      if (last) {
        link(last, splitter.id);
      }
      first = first ?? splitter.id;
      last = mixer.id;
    } else {
      const node = buildNode(item, registry, nextId, used);
      nodes.push(node);
      if (last) {
        link(last, node.id);
      }
      first = first ?? node.id;
      last = node.id;
    }
  }
  return { nodes, edges, first, last };
}

/** Throws unless every node is reachable from the input, the output is reachable, and there is no cycle. */
export function validateGraph(graph: BuiltGraph, where: string): void {
  const ids = graph.nodes.map((n) => n.id);
  if (new Set(ids).size !== ids.length) {
    throw new ProposalError(`${where}: duplicate node ids`);
  }
  const known = new Set(ids);
  for (const edge of graph.edges) {
    if (!known.has(edge.from) || !known.has(edge.to)) {
      throw new ProposalError(`${where}: edge ${edge.from} -> ${edge.to} names a missing node`);
    }
  }
  const out = new Map<string, string[]>(ids.map((id) => [id, []]));
  const inDegree = new Map<string, number>(ids.map((id) => [id, 0]));
  for (const edge of graph.edges) {
    out.get(edge.from)?.push(edge.to);
    inDegree.set(edge.to, (inDegree.get(edge.to) ?? 0) + 1);
  }
  const queue = ids.filter((id) => inDegree.get(id) === 0);
  let visited = 0;
  while (queue.length) {
    const id = queue.shift() as string;
    visited += 1;
    for (const next of out.get(id) ?? []) {
      const remaining = (inDegree.get(next) ?? 0) - 1;
      inDegree.set(next, remaining);
      if (remaining === 0) {
        queue.push(next);
      }
    }
  }
  if (visited !== ids.length) {
    throw new ProposalError(`${where}: the graph has a cycle`);
  }
  const reach = new Set(["__input__"]);
  for (let changed = true; changed;) {
    changed = false;
    for (const edge of graph.edges) {
      if (reach.has(edge.from) && !reach.has(edge.to)) {
        reach.add(edge.to);
        changed = true;
      }
    }
  }
  const orphans = ids.filter((id) => !reach.has(id));
  if (orphans.length) {
    throw new ProposalError(`${where}: nodes not reachable from the input: ${orphans.join(", ")}`);
  }
  if (!reach.has("__output__")) {
    throw new ProposalError(`${where}: the output is not reachable`);
  }
}

/**
 * A short hash of a scene graph, to tell whether a measurement or check was made on it as built
 * now. Labels and categories are left out: they do not change the sound.
 */
export function graphHash(graph: BuiltGraph): string {
  const audible = {
    nodes: graph.nodes.map(({ id, type, enabled, params, resources }) => ({ id, type, enabled: enabled ?? true, params, resources: resources ?? [] })),
    edges: graph.edges,
  };
  return sha1Hex(JSON.stringify(audible)).slice(0, 16);
}

export function buildPreset(collection: PresetCollection, spec: PresetSpec, registry: BuildRegistry): { preset: BuiltPreset; used: BuildUsage } {
  const presetId = uuidv5(`${collection.idScope}/${spec.idKey ?? spec.key}`);
  const used: UsageSets = { effects: new Set(), fxPresets: new Set(), resources: new Set() };
  const scenesSpec = spec.scenes ?? [{ key: "main", title: "Main", chain: spec.chain ?? [] }];
  const scenes = scenesSpec.map((sceneSpec) => {
    const sceneId = uuidv5(`${presetId}/scene/${sceneSpec.key}`);
    let counter = 0;
    const nextId = (): string => uuidv5(`${sceneId}/node/${counter++}`);
    const input: BuiltNode = { id: "__input__", type: "input", category: "utility", label: "Input", params: { gainDb: 0 } };
    const output: BuiltNode = { id: "__output__", type: "output", category: "utility", label: "Output", params: { gainDb: sceneSpec.outputGainDb ?? spec.outputGainDb ?? 0 } };
    const chain = buildChain(sceneSpec.chain, registry, nextId, used);
    const where = `${collection.slug ?? collection.idScope}/${spec.key}/${sceneSpec.key}`;
    if (!chain.first || !chain.last) {
      throw new ProposalError(`${where}: the chain is empty`);
    }
    const graph: BuiltGraph = {
      nodes: [input, ...chain.nodes, output],
      edges: [{ from: "__input__", to: chain.first }, ...chain.edges, { from: chain.last, to: "__output__" }],
    };
    validateGraph(graph, where);
    return { id: sceneId, title: sceneSpec.title, graph };
  });
  const tags = [
    ...(collection.slug ? [`pack:${collection.slug}`] : []),
    ...(collection.tags ?? []),
    ...(spec.tags ?? []),
    ...[...used.fxPresets].map((p) => `fxpreset:${p}`),
  ];
  return {
    preset: {
      id: presetId, version: 2, name: spec.name, category: collection.category,
      description: spec.description, tags: [...new Set(tags)], scenes,
    },
    used: { effects: [...used.effects], fxPresets: [...used.fxPresets], resources: [...used.resources] },
  };
}
