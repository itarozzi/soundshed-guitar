/**
 * The blend editor's data: what is being edited, kept apart from the page that shows it.
 *
 * A blend places each model either at values of one or more parameters (Gain, Tone, ...),
 * which the node turns into knobs, or, on the Sweep axis, at its place in the list, which the
 * node plays with its single Blend knob. Values are kept normalised (0..1), as definitions
 * store them; the table shows them on each parameter's own scale.
 *
 * Everything here is a plain function of the state and the resource library, so it is
 * tested without a page (tests/blendEditState.test.ts).
 */

import {
  BLEND_PARAM_SPECS,
  buildBlendModelMappingsFromIds,
  buildParameterMapFromLegacy,
  denormalizeBlendValue,
  getBlendParamSpec,
  inferParamValueFromName,
  stripParamTokensFromName,
} from "../blendUtils.js";
import type { BlendDefinition, BlendMode, BlendModelMapping, LibraryResource, ResourceLibrary } from "../types.js";
import { findResourceById } from "../utils.js";

export type BlendAxis = "params" | "sweep";

export interface BlendEditRow {
  id: string;
  /** Normalised captured values, by parameter id. Kept through a switch to Sweep and back. */
  values: Record<string, number>;
  levelTrimDb: number;
}

export interface BlendEditState {
  id: string;
  name: string;
  /** The definition's category, kept when the models do not say one. */
  category: string;
  mode: BlendMode;
  axis: BlendAxis;
  params: string[];
  rows: BlendEditRow[];
}

export const MIN_LEVEL_TRIM_DB = -24;
export const MAX_LEVEL_TRIM_DB = 24;
/** Positions this close count as the same: the knobs cannot tell such models apart. */
const SAME_POSITION_EPS = 1e-4;
const BLEND_CATEGORIES = ["pedal", "preamp", "amp", "full-rig", "cab"];

export function libraryModel(library: ResourceLibrary, id: string): LibraryResource | undefined {
  return id ? findResourceById(library.nam ?? [], id) : undefined;
}

function modelName(library: ResourceLibrary, id: string): string {
  return libraryModel(library, id)?.name?.trim() || id;
}

/** "A1" or "A2" from a model's metadata, which stores either "2" or "A2"; "" when unknown. */
export function modelArchitecture(resource: LibraryResource | undefined): string {
  const raw = (resource?.metadata?.architectureVersion ?? "").trim().toUpperCase();
  return raw === "1" || raw === "A1" ? "A1" : raw === "2" || raw === "A2" ? "A2" : "";
}

export function clampTrim(value: number): number {
  return Number.isFinite(value) ? Math.min(MAX_LEVEL_TRIM_DB, Math.max(MIN_LEVEL_TRIM_DB, value)) : 0;
}

export function editStateFromDefinition(blend: BlendDefinition, library: ResourceLibrary): BlendEditState {
  const mappings: BlendModelMapping[] = blend.modelMappings?.length
    ? blend.modelMappings
    : buildBlendModelMappingsFromIds(blend.models ?? [], library);

  const rows = mappings
    .filter((mapping) => mapping.id)
    .map((mapping) => ({
      id: mapping.id,
      values: { ...buildParameterMapFromLegacy(mapping) },
      levelTrimDb: clampTrim(mapping.levelTrimDb ?? 0),
    }));

  // The definition's parameters first, then any a row is captured at; a parameter no row has
  // a value for is dropped, as the node shows no knob for it.
  const params: string[] = [];
  [...(blend.parameters ?? []), ...rows.flatMap((row) => Object.keys(row.values))].forEach((paramId) => {
    if (!params.includes(paramId) && rows.some((row) => typeof row.values[paramId] === "number")) {
      params.push(paramId);
    }
  });

  return {
    id: blend.id,
    name: blend.name ?? "",
    category: blend.category ?? "",
    mode: blend.blendMode === "snap" ? "snap" : "interpolate",
    axis: params.length ? "params" : "sweep",
    params,
    rows,
  };
}

/** The models' gear, by majority; the definition's own category when they do not say. */
export function inferBlendCategory(state: BlendEditState, library: ResourceLibrary): string {
  const counts = new Map<string, number>();
  state.rows.forEach((row) => {
    const resource = libraryModel(library, row.id);
    const gear = (resource?.metadata?.gear || resource?.category || "").toLowerCase();
    const category = gear === "outboard" ? "preamp" : gear;
    if (BLEND_CATEGORIES.includes(category)) {
      counts.set(category, (counts.get(category) ?? 0) + 1);
    }
  });
  let best = "";
  counts.forEach((count, category) => {
    if (!best || count > (counts.get(best) ?? 0)) {
      best = category;
    }
  });
  return best || (BLEND_CATEGORIES.includes(state.category) ? state.category : "amp");
}

/** The tone group every model comes from, or none: a blend mixing packs belongs to no one pack. */
export function sharedToneGroup(state: BlendEditState, library: ResourceLibrary): { id: string; title: string } | null {
  let shared: { id: string; title: string } | null = null;
  for (const row of state.rows) {
    const metadata = libraryModel(library, row.id)?.metadata ?? {};
    const id = metadata.toneId || metadata.groupId || "";
    if (!id || (shared && shared.id !== id)) {
      return null;
    }
    shared ??= { id, title: metadata.toneTitle || metadata.groupName || id };
  }
  return shared;
}

export function definitionFromEditState(state: BlendEditState, library: ResourceLibrary): BlendDefinition {
  const params = state.axis === "params" ? state.params : [];
  const modelMappings: BlendModelMapping[] = state.rows
    .filter((row) => row.id)
    .map((row) => {
      const mapping: BlendModelMapping = { id: row.id };
      const parameters: Record<string, number> = {};
      params.forEach((paramId) => {
        const value = row.values[paramId];
        if (typeof value === "number" && Number.isFinite(value)) {
          parameters[paramId] = value;
        }
      });
      // The engine places a model on the sweep by its first parameter's value; it reads it
      // from parameterId/parameterValue as well as from the map.
      const primary = params[0];
      if (primary && typeof parameters[primary] === "number") {
        mapping.parameterId = primary;
        mapping.parameterValue = parameters[primary];
      }
      if (Object.keys(parameters).length) {
        mapping.parameters = parameters;
      }
      const trim = clampTrim(row.levelTrimDb);
      if (trim !== 0) {
        mapping.levelTrimDb = trim;
      }
      return mapping;
    });

  const group = sharedToneGroup(state, library);
  return {
    id: state.id,
    name: state.name.trim(),
    category: inferBlendCategory(state, library),
    parameters: [...params],
    models: modelMappings.map((mapping) => mapping.id),
    modelMappings,
    blendMode: state.mode,
    toneGroupId: group?.id,
    toneGroupTitle: group?.title,
  };
}

/** "Tone" on a pedal, "Treble" on anything else: the two share the letter T in model names. */
function nameParamCandidates(category: string): string[] {
  const skip = category === "pedal" ? "treble" : "tone";
  return BLEND_PARAM_SPECS.map((spec) => spec.id).filter((id) => id !== skip && !id.startsWith("custom_"));
}

/**
 * The parameters the models' names give values for, in the usual knob order, keeping only
 * those that differ between models: a pack captured at "P5 B5 M5" throughout says nothing
 * the knobs could use.
 */
export function inferParamsFromNames(state: BlendEditState, library: ResourceLibrary): string[] {
  const category = inferBlendCategory(state, library);
  return nameParamCandidates(category).filter((paramId) => {
    const values = new Set<number>();
    state.rows.forEach((row) => {
      const value = inferParamValueFromName(modelName(library, row.id), paramId);
      if (value !== null) {
        values.add(value);
      }
    });
    return values.size >= 2;
  });
}

/** Fills each row's values from its model's name: only empty cells unless `overwrite`. */
export function fillValuesFromNames(state: BlendEditState, library: ResourceLibrary, overwrite = false): void {
  state.rows.forEach((row) => {
    const name = modelName(library, row.id);
    state.params.forEach((paramId) => {
      if (!overwrite && typeof row.values[paramId] === "number") {
        return;
      }
      const value = inferParamValueFromName(name, paramId);
      if (value !== null) {
        row.values[paramId] = value;
      }
    });
  });
}

/** Rows in knob order: by the first parameter, then the next. A row missing a value goes last. */
export function sortRows(state: BlendEditState): void {
  if (state.axis !== "params" || !state.params.length) {
    return;
  }
  state.rows.sort((a, b) => {
    for (const paramId of state.params) {
      const av = a.values[paramId];
      const bv = b.values[paramId];
      if (typeof av !== "number" || typeof bv !== "number") {
        if (typeof av === "number" || typeof bv === "number") {
          return typeof av === "number" ? -1 : 1;
        }
        continue;
      }
      if (Math.abs(av - bv) > SAME_POSITION_EPS) {
        return av - bv;
      }
    }
    return 0;
  });
}

/**
 * Adds models to the blend, skipping ones already in it. On the Parameters axis their values
 * come from their names; the first models into an empty blend also choose the parameters.
 * Returns how many were added.
 */
export function addModels(state: BlendEditState, ids: string[], library: ResourceLibrary): number {
  const fresh = ids.filter((id, index) => id && ids.indexOf(id) === index && !state.rows.some((row) => row.id === id));
  fresh.forEach((id) => state.rows.push({ id, values: {}, levelTrimDb: 0 }));
  if (!fresh.length) {
    return 0;
  }
  if (!state.params.length) {
    state.params = inferParamsFromNames(state, library);
    state.axis = state.params.length ? "params" : "sweep";
  }
  if (state.axis === "params") {
    fillValuesFromNames(state, library);
    sortRows(state);
  }
  return fresh.length;
}

export function addParam(state: BlendEditState, paramId: string, library: ResourceLibrary): void {
  if (!getBlendParamSpec(paramId) || state.params.includes(paramId)) {
    return;
  }
  state.params.push(paramId);
  state.axis = "params";
  fillValuesFromNames(state, library);
}

export function removeParam(state: BlendEditState, paramId: string): void {
  state.params = state.params.filter((id) => id !== paramId);
  if (!state.params.length) {
    state.axis = "sweep";
  }
}

/** Switching to Parameters with none chosen yet picks them from the names, else starts with Gain. */
export function setAxis(state: BlendEditState, axis: BlendAxis, library: ResourceLibrary): void {
  state.axis = axis;
  if (axis === "params" && !state.params.length) {
    const inferred = inferParamsFromNames(state, library);
    state.params = inferred.length ? inferred : ["gain"];
    fillValuesFromNames(state, library);
    sortRows(state);
  }
}

function positionLabel(state: BlendEditState, row: BlendEditRow): string {
  return state.params
    .map((paramId) => {
      const spec = getBlendParamSpec(paramId);
      const value = row.values[paramId];
      const shown = denormalizeBlendValue(value, spec);
      return `${spec?.label ?? paramId} ${Number(shown.toFixed(1))}`;
    })
    .join(" · ");
}

/** Rows that sit at the same position, in groups of two or more. */
export function sharedPositions(state: BlendEditState): BlendEditRow[][] {
  if (state.axis !== "params" || !state.params.length) {
    return [];
  }
  const groups = new Map<string, BlendEditRow[]>();
  state.rows.forEach((row) => {
    if (!state.params.every((paramId) => typeof row.values[paramId] === "number")) {
      return;
    }
    const key = state.params.map((paramId) => Math.round(row.values[paramId] / SAME_POSITION_EPS)).join("|");
    groups.set(key, [...(groups.get(key) ?? []), row]);
  });
  return [...groups.values()].filter((group) => group.length > 1);
}

export interface BlendWarning {
  text: string;
  /** Set when the warning comes with a fix the editor can offer: split the blend by voicing. */
  action?: "split-voicings";
}

export function blendWarnings(state: BlendEditState, library: ResourceLibrary): BlendWarning[] {
  const warnings: BlendWarning[] = [];
  if (state.rows.length < 2) {
    warnings.push({ text: "A blend needs at least two models." });
  }

  const missing = state.rows.filter((row) => !libraryModel(library, row.id)).length;
  if (missing) {
    warnings.push({ text: `${missing} model${missing === 1 ? " is" : "s are"} not in your library, so ${missing === 1 ? "it" : "they"} will not play.` });
  }

  if (state.axis === "params") {
    state.params.forEach((paramId) => {
      const blank = state.rows.filter((row) => typeof row.values[paramId] !== "number").length;
      if (blank) {
        const label = getBlendParamSpec(paramId)?.label ?? paramId;
        warnings.push({ text: `${blank} model${blank === 1 ? " has" : "s have"} no ${label} value.` });
      }
    });

    const shared = sharedPositions(state);
    if (shared.length) {
      const first = shared[0];
      const more = shared.length > 1 ? `, and ${shared.length - 1} more position${shared.length === 2 ? "" : "s"}` : "";
      const voicings = groupByVoicing(state, library);
      warnings.push({
        text: `${first.length} models share ${positionLabel(state, first[0])}${more}: the knobs reach only one model at each.`,
        action: voicings.length > 1 ? "split-voicings" : undefined,
      });
    }
  }

  return warnings;
}

export interface BlendVoicing {
  label: string;
  rows: BlendEditRow[];
}

/**
 * The models grouped by voicing: their names with the parameter tokens taken out ("Alpha-BITE
 * G7" and "Alpha-BITE G1" are one voicing). Each label is what is left once the part every
 * name shares is removed.
 */
export function groupByVoicing(state: BlendEditState, library: ResourceLibrary): BlendVoicing[] {
  const keyed = state.rows.map((row) => ({ row, key: stripParamTokensFromName(modelName(library, row.id)) }));
  const keys = [...new Set(keyed.map((entry) => entry.key))];
  if (keys.length < 2) {
    return keys.length ? [{ label: keys[0], rows: state.rows.slice() }] : [];
  }

  let prefix = keys[0];
  keys.forEach((key) => {
    while (prefix && !key.toLowerCase().startsWith(prefix.toLowerCase())) {
      prefix = prefix.slice(0, -1);
    }
  });
  // Cut the shared part back to a space, so "Alpha-BITE" and "Alpha-Bite-Growl" keep their words.
  const shared = prefix.slice(0, prefix.lastIndexOf(" ") + 1);

  // "Omega - Omega-Growl" says it twice: the last " - " part is the voicing.
  const labelOf = (key: string): string => {
    const rest = key.slice(shared.length).replace(/^[\s\-_|·,]+/u, "").trim() || key;
    return rest.split(" - ").pop()?.trim() || rest;
  };
  return keys.map((key) => ({
    label: labelOf(key),
    rows: keyed.filter((entry) => entry.key === key).map((entry) => entry.row),
  }));
}

/** Reads a model file's own loudness (dB), as NAM writes it in its metadata. */
export function readNamLoudness(fileText: string): number | null {
  try {
    const parsed = JSON.parse(fileText) as { metadata?: { loudness?: unknown } };
    const loudness = parsed.metadata?.loudness;
    return typeof loudness === "number" && Number.isFinite(loudness) ? loudness : null;
  } catch {
    return null;
  }
}

/**
 * Trims that bring every model with a known loudness to their median, to the nearest half dB.
 * Models without one keep their trim. Returns how many were matched.
 */
export function matchLevels(state: BlendEditState, loudnessById: Map<string, number>): number {
  const known = state.rows.filter((row) => loudnessById.has(row.id));
  if (known.length < 2) {
    return 0;
  }
  const sorted = known.map((row) => loudnessById.get(row.id)!).sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  const target = sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
  known.forEach((row) => {
    row.levelTrimDb = clampTrim(Math.round((target - loudnessById.get(row.id)!) * 2) / 2);
  });
  return known.length;
}
