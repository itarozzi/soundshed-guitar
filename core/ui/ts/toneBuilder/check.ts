/**
 * Checks a proposal the way it will be built, without saving anything: the shape rules (key,
 * name, description, at most three scenes of one rig), a full build against the inventory, and
 * warnings where a capture's scope looks wrong for its place in the chain — a full-rig capture
 * followed by a cab, an amp-only capture with no cab after it, a pedal capture in the amp slot.
 */

import { buildPreset } from "./build.js";
import type { BuildRegistry } from "./build.js";
import type { ChainItemSpec, InventoryEffect, PresetSpec, ResourceSpec, SplitSpec } from "./types.js";

/** Most scenes a preset may have. Scenes are one rig's variations; another rig is another preset. */
export const MAX_SCENES = 3;

const AMP_NAM_TYPES = new Set([
  "49ea214c-91e6-41f9-bd27-ad6eec0ae90a", // Neural Amp (NAM)
  "2eb53b40-6139-4696-8820-387ac56ffa91", // its retired type, still in older specs
]);
const FX_NAM_TYPE = "c3263344-65e4-4b7e-b102-ea625700e12f"; // Neural FX (NAM)

/** What a capture includes, as far as the chain rules care. */
export type CaptureScope = "full" | "amp" | "preamp" | "pedal" | null;

export interface ProposalCheckResult {
  ok: boolean;
  problems: string[];
  warnings: string[];
  /** Factory effect presets the build used, "<effect>/<preset>". */
  fxPresets: string[];
}

export interface ProposalCheckOptions {
  /** Scope of a resource spec; defaults to the library resource's `captureScope`. */
  scopeOf?: (resource: ResourceSpec) => CaptureScope;
}

/** Maps a provider-neutral capture scope ("amp+cab", "poweramp", …) onto the chain rules' scopes. */
export function normaliseCaptureScope(scope: string | null | undefined): CaptureScope {
  const value = (scope ?? "").trim().toLowerCase();
  if (!value) return null;
  if (value === "amp+cab" || value === "full" || value.startsWith("full rig") || value === "full-rig") return "full";
  if (value === "preamp" || value.startsWith("preamp")) return "preamp";
  if (value === "amp" || value === "poweramp" || value.startsWith("amp only")) return "amp";
  if (value === "pedal" || value.startsWith("pedal")) return "pedal";
  return null;
}

function isSplit(item: ChainItemSpec): item is SplitSpec {
  return Array.isArray((item as SplitSpec).split);
}

function effectOrNull(registry: BuildRegistry, fx: string): InventoryEffect | null {
  try {
    return registry.effect(fx);
  } catch {
    return null;
  }
}

function scopeWarnings(
  chain: ChainItemSpec[],
  where: string,
  registry: BuildRegistry,
  scopeOf: (resource: ResourceSpec) => CaptureScope,
  warnings: string[],
): void {
  chain.forEach((item, i) => {
    if (isSplit(item)) {
      item.split.forEach((branch, b) => scopeWarnings(branch, `${where} branch ${b + 1}`, registry, scopeOf, warnings));
      return;
    }
    const effect = effectOrNull(registry, item.fx);
    if (!item.resource || !effect || (!AMP_NAM_TYPES.has(effect.type) && effect.type !== FX_NAM_TYPE)) {
      return;
    }
    const inAmpSlot = AMP_NAM_TYPES.has(effect.type);
    const scope = scopeOf(item.resource);
    const cabAfter = chain.slice(i + 1).some((next) => !isSplit(next) && effectOrNull(registry, next.fx)?.category === "cab");
    const label = item.label ?? JSON.stringify(item.resource);
    if (scope === "pedal" && inAmpSlot) {
      warnings.push(`${where}: ${label} is a pedal capture in ${effect.displayName}; pedals go in the FX capture slot`);
    }
    if (scope === "full" && cabAfter) {
      warnings.push(`${where}: ${label} looks like a full rig (cab included) but a cab follows it`);
    }
    if ((scope === "amp" || scope === "preamp") && inAmpSlot && !cabAfter) {
      warnings.push(`${where}: ${label} looks amp-only${scope === "preamp" ? " (preamp)" : ""} but no cab follows it`);
    }
  });
}

export function checkProposal(proposal: PresetSpec, registry: BuildRegistry, options: ProposalCheckOptions = {}): ProposalCheckResult {
  const problems: string[] = [];
  const warnings: string[] = [];
  let fxPresets: string[] = [];
  for (const key of ["key", "name", "description", "scenes"] as const) {
    if (!proposal[key]) problems.push(`no "${key}"`);
  }
  const scenes = proposal.scenes ?? [];
  if (scenes.length > MAX_SCENES) {
    problems.push(`${scenes.length} scenes; at most ${MAX_SCENES === 3 ? "three" : MAX_SCENES}, all one rig`);
  }
  const scopeOf = options.scopeOf ?? ((spec: ResourceSpec): CaptureScope => {
    const resource = spec.id ? registry.resources.find((r) => r.id === spec.id) : undefined;
    return normaliseCaptureScope(resource?.captureScope);
  });
  for (const scene of scenes) {
    if (!scene.key || !scene.title || !Array.isArray(scene.chain)) {
      problems.push(`scene ${scene.key ?? "?"} needs key, title and chain`);
    } else {
      scopeWarnings(scene.chain, scene.key, registry, scopeOf, warnings);
    }
  }
  if (!problems.length) {
    try {
      const { used } = buildPreset({ idScope: "check", category: "check" }, proposal, registry);
      fxPresets = used.fxPresets;
    } catch (error) {
      problems.push((error as Error).message);
    }
  }
  return { ok: problems.length === 0, problems, warnings, fxPresets };
}
