import { uiState } from "../state.js";
import { showNotification } from "../notifications.js";
import { getAppSetting } from "../appSettingsStore.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { EffectGuids } from "../effectGuids.js";
import { FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING } from "../settings/keys.js";
import type { BlendDefinition, ResourceRef, StoredEffectPreset } from "../types.js";

/**
 * The user's effect presets, riding along in a preset collection archive: how factory
 * effect presets that choose a model, IR or blend are authored. Bundled as a factory
 * archive, its `effectPresets` become factory presets for those effects, the engine moving
 * their references onto the ids it registers the archive's resources and blends under
 * (core/src/controller/internal/EffectPresetArchiveSupport.h).
 */
export type EffectPresetExport = {
  byEffectType: Record<string, StoredEffectPreset[]>;
  /** The blends they play, so the archive carries the definitions and their models. */
  blendIds: string[];
  /** The models and IRs they choose, so the archive carries the files. */
  refs: ResourceRef[];
};

const refType = (ref: ResourceRef): string => ref.resourceType ?? ref.type ?? "";
const refId = (ref: ResourceRef): string => ref.resourceId ?? ref.id ?? "";

/**
 * Empty unless the Factory Preset Archive Tools are on and so is the setting to export effect
 * presets. Hosted plugins are left out: their state never reaches the UI, and the machine the
 * archive is installed on may not have the plugin.
 */
export function collectEffectPresetsForExport(): EffectPresetExport {
  const collected: EffectPresetExport = { byEffectType: {}, blendIds: [], refs: [] };
  if (!isFeatureEnabled(Features.FactoryPresetArchives) || getAppSetting(FACTORY_ARCHIVE_EXPORT_EFFECT_PRESETS_SETTING) !== true) {
    return collected;
  }
  for (const [effectType, entries] of Object.entries(uiState.effectPresets ?? {})) {
    if (effectType === EffectGuids.kPluginHost || entries.length === 0) continue;
    collected.byEffectType[effectType] = entries;
    for (const entry of entries) {
      collected.refs.push(...(entry.resources ?? []));
      if (entry.config?.blendId) collected.blendIds.push(entry.config.blendId);
    }
  }
  return collected;
}

/**
 * The archive's `effectPresets` field, each model and IR moved to the id the archive files it
 * under (`idMap`: library id to content hash). An entry choosing a file outside the library, a
 * resource that could not be read, or a blend the archive does not carry is left out, and
 * counted in a warning: a factory archive would only skip it on load.
 */
export function finishEffectPresetsForArchive(
  collected: EffectPresetExport,
  idMap: Map<string, string>,
  blends: BlendDefinition[],
): { effectPresets?: Record<string, StoredEffectPreset[]> } {
  const carriedBlendIds = new Set(blends.map((blend) => blend.id));
  const effectPresets: Record<string, StoredEffectPreset[]> = {};
  let leftOut = 0;

  for (const [effectType, entries] of Object.entries(collected.byEffectType)) {
    const kept = entries.flatMap((entry): StoredEffectPreset[] => {
      const resources = (entry.resources ?? []).map((ref): ResourceRef | null => {
        const mapped = idMap.get(refId(ref));
        // The rest is a blend model's place on the sweep; a path or embedded id names a file
        // on this machine.
        const { type: _type, id: _id, filePath: _filePath, embeddedId: _embeddedId, ...rest } = ref;
        return mapped && refType(ref) ? { ...rest, resourceType: refType(ref), resourceId: mapped } : null;
      });
      const blendId = entry.config?.blendId;
      if (resources.includes(null) || (blendId && !carriedBlendIds.has(blendId))) {
        leftOut += 1;
        return [];
      }
      return [{
        id: entry.id,
        name: entry.name,
        parameters: { ...entry.parameters },
        ...(entry.resources ? { resources: resources as ResourceRef[] } : {}),
        ...(entry.config ? { config: { ...entry.config } } : {}),
      }];
    });
    if (kept.length > 0) effectPresets[effectType] = kept;
  }

  if (leftOut > 0) {
    showNotification("Export warning", `${leftOut} effect presets were left out: each uses a model, IR or blend the archive could not carry`);
  }
  return Object.keys(effectPresets).length > 0 ? { effectPresets } : {};
}
