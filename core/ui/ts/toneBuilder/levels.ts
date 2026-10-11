/**
 * Balancing built presets to the level every Soundshed preset pack plays at: each scene's
 * integrated loudness over the demo DI at -18 LUFS (within 0.5 LU), its peak under -1 dBFS, as the
 * engine measures it (measurePresetLevel; the mix's centre pan included, as the packs were
 * measured).
 *
 * The same rules as the content pipeline's balancing: a scene with a NAM capture on its main path
 * is corrected on that capture's Output Gain — the last Neural Amp, else the last Neural FX — so
 * everything after it sees a nominal level; a capture feeding a split of captures is never used
 * (its output drives them); anything left over, and every other scene, goes to the scene's output
 * trim. Effects after a capture are not all linear, so it measures again after each correction.
 */

import { buildPreset } from "./build.js";
import type { BuildRegistry } from "./build.js";
import type { ChainItemSpec, NodeSpec, PresetCollection, PresetSpec, SceneSpec, SplitSpec } from "./types.js";

export const TARGET_LUFS = -18;
export const TOLERANCE_LU = 0.5;
export const PEAK_CEILING_DBFS = -1;
const NAM_GAIN_LIMIT_DB = 24;
const MAX_PASSES = 3;

const AMP_NAM_TYPES = new Set(["49ea214c-91e6-41f9-bd27-ad6eec0ae90a", "2eb53b40-6139-4696-8820-387ac56ffa91"]);
const FX_NAM_TYPE = "c3263344-65e4-4b7e-b102-ea625700e12f";

/** One scene measured: what the engine reports for a preset object and scene id. */
export type SceneMeasure = (preset: Record<string, unknown>, sceneId: string) => Promise<{ integratedLufs: number | null; peakDbfs: number | null }>;

export interface SceneBalance {
  presetKey: string;
  sceneKey: string;
  lufs: number | null;
  peakDbfs: number | null;
  /** Total correction applied, dB. */
  correctionDb: number;
  balanced: boolean;
  note?: string;
}

function isSplit(item: ChainItemSpec): item is SplitSpec {
  return Array.isArray((item as SplitSpec).split);
}

function effectType(registry: BuildRegistry, fx: string): string | null {
  try {
    return registry.effect(fx).type;
  } catch {
    return null;
  }
}

/** The chain index of the capture that stages the scene's level, or -1 for an output trim. */
export function stagingCaptureIndex(chain: ChainItemSpec[], registry: BuildRegistry): number {
  const isOnCapture = (item: ChainItemSpec, types: (type: string) => boolean): boolean =>
    !isSplit(item) && !item.off && Boolean(item.resource) && types(effectType(registry, item.fx) ?? "");
  const isCapture = (item: ChainItemSpec): boolean => isOnCapture(item, (t) => AMP_NAM_TYPES.has(t) || t === FX_NAM_TYPE);
  const holdsCapture = (item: ChainItemSpec): boolean => isSplit(item) && item.split.some((branch) => branch.some((x) => isCapture(x) || holdsCapture(x)));
  for (const types of [(t: string) => AMP_NAM_TYPES.has(t), (t: string) => t === FX_NAM_TYPE]) {
    for (let i = chain.length - 1; i >= 0; i -= 1) {
      if (isOnCapture(chain[i], types)) {
        return chain.slice(i + 1).some(holdsCapture) ? -1 : i;
      }
    }
  }
  return -1;
}

/** The correction for a measurement, in 0.5 dB steps, never lifting the peak over the ceiling. */
export function correctionFor(lufs: number, peakDbfs: number | null, target = TARGET_LUFS): number {
  let delta = Math.round((target - lufs) * 2) / 2;
  if (peakDbfs !== null && peakDbfs + delta > PEAK_CEILING_DBFS) {
    delta = Math.floor((PEAK_CEILING_DBFS - peakDbfs) * 2) / 2;
  }
  return delta;
}

function withScenes(spec: PresetSpec): PresetSpec {
  if (spec.scenes?.length) return structuredClone(spec);
  const { chain, ...rest } = structuredClone(spec);
  return { ...rest, scenes: [{ key: "main", title: "Main", chain: chain ?? [] }] };
}

function applyCorrection(scene: SceneSpec, delta: number, registry: BuildRegistry): void {
  const at = stagingCaptureIndex(scene.chain, registry);
  let remainder = delta;
  if (at >= 0) {
    const capture = scene.chain[at] as NodeSpec;
    const now = typeof capture.set?.outputGain === "number" ? capture.set.outputGain : 0;
    const next = Math.max(-NAM_GAIN_LIMIT_DB, Math.min(NAM_GAIN_LIMIT_DB, now + delta));
    scene.chain[at] = { ...capture, set: { ...(capture.set ?? {}), outputGain: next } };
    remainder = delta - (next - now);
  }
  if (remainder !== 0) {
    scene.outputGainDb = (scene.outputGainDb ?? 0) + remainder;
  }
}

/**
 * Measures every scene of the spec as built, corrects the ones off target and measures again, up
 * to three passes. Returns the corrected spec and a report per scene. A scene that cannot be
 * measured (silent, or the engine refuses) is left as it was, with a note.
 */
export async function balancePresetSpec(
  collection: PresetCollection,
  original: PresetSpec,
  registry: BuildRegistry,
  measure: SceneMeasure,
): Promise<{ spec: PresetSpec; report: SceneBalance[] }> {
  const spec = withScenes(original);
  const scenes = spec.scenes as SceneSpec[];
  const report = new Map<string, SceneBalance>(scenes.map((scene) => [scene.key, {
    presetKey: spec.key, sceneKey: scene.key, lufs: null, peakDbfs: null, correctionDb: 0, balanced: false,
  }]));
  for (let pass = 0; pass < MAX_PASSES; pass += 1) {
    const { preset } = buildPreset(collection, spec, registry);
    let corrected = false;
    for (const [index, scene] of scenes.entries()) {
      const entry = report.get(scene.key) as SceneBalance;
      if (entry.balanced || entry.note) continue;
      let level: { integratedLufs: number | null; peakDbfs: number | null };
      try {
        level = await measure(preset as unknown as Record<string, unknown>, preset.scenes[index].id);
      } catch (error) {
        entry.note = `not measured: ${(error as Error).message}`;
        continue;
      }
      entry.lufs = level.integratedLufs;
      entry.peakDbfs = level.peakDbfs;
      if (level.integratedLufs === null) {
        entry.note = "silent over the demo DI";
        continue;
      }
      const off = level.integratedLufs - TARGET_LUFS;
      const peakOk = level.peakDbfs === null || level.peakDbfs <= PEAK_CEILING_DBFS;
      if (Math.abs(off) <= TOLERANCE_LU && peakOk) {
        entry.balanced = true;
        continue;
      }
      const delta = correctionFor(level.integratedLufs, level.peakDbfs);
      if (delta === 0) {
        entry.note = "held below the target by its peak";
        continue;
      }
      applyCorrection(scene, delta, registry);
      entry.correctionDb += delta;
      corrected = true;
    }
    if (!corrected) break;
  }
  return { spec, report: [...report.values()] };
}
