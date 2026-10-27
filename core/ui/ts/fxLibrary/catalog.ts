/**
 * What the FX library offers: the registry's effects, blends, custom effects and composites,
 * each as an FxLibraryItem, and the categories they fall in. The docked library and the
 * chooser a + in the chain opens both list these (fxBrowser.ts renders them).
 */

import { EffectTypeRegistry, type EffectTypeInfo } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { uiState } from "../state.js";
import { BLEND_CATEGORIES, CATEGORIES, CATEGORY_ORDER, DEFAULT_BLEND_CATEGORY } from "../generated/effectPresentation.js";
import { getCompositeEffectEntries } from "../compositeEffects.js";
import { getCustomEffectLibrary } from "../customEffects.js";

/** Which library entry: its effect type, and the blend or custom effect it carries. */
export type FxItemIdentity = { effectType: string; blendId?: string; customEffectId?: string };

// Category display metadata — id → { name, color }, in the FX library's order. It comes from
// core/ui/data/effect-presentation.json, which Soundshed Nano Q reads too; the actual
// category list is derived at render time from what the effect registry contains.
export const CATEGORY_METADATA: Record<string, { name: string; color: string }> = Object.fromEntries(
  CATEGORY_ORDER.map((id) => [id, { name: CATEGORIES[id].name, color: CATEGORIES[id].color }]),
);

export type FxLibraryItem = EffectTypeInfo & {
  blendId?: string;
  blendCategory?: string;
  compositeId?: string;
  customEffectId?: string;
  moduleResourceType?: string;
  moduleResourceId?: string;
  defaultParams?: Record<string, number>;
  description?: string;
};

export type FxPointerDragPayload = {
  effectType: string;
  blendId?: string;
  blendName?: string;
  blendCategory?: string;
  customEffect?: {
    customEffectId: string;
    name: string;
    category: string;
    moduleResourceType: string;
    moduleResourceId: string;
    defaultParams: Record<string, number>;
  };
};

export function isSameFxItem(a: FxItemIdentity, b: FxItemIdentity): boolean {
  return EffectTypeRegistry.resolve(a.effectType) === EffectTypeRegistry.resolve(b.effectType)
    && (a.blendId ?? "") === (b.blendId ?? "")
    && (a.customEffectId ?? "") === (b.customEffectId ?? "");
}

export function fxItemIdentity(item: FxLibraryItem): FxItemIdentity {
  return { effectType: item.type, blendId: item.blendId, customEffectId: item.customEffectId };
}

export function fxPayloadIdentity(payload: FxPointerDragPayload): FxItemIdentity {
  return { effectType: payload.effectType, blendId: payload.blendId, customEffectId: payload.customEffect?.customEffectId };
}

export function getCatalogEffects(options?: { excludeTypes?: string[] }): EffectTypeInfo[] {
  const excludedTypes = new Set([EffectGuids.kMixer, ...(options?.excludeTypes ?? [])]);
  const experimentalEffectTypes = new Set<string>([EffectGuids.kTransposeStft, EffectGuids.kTransposeHybrid]);
  return EffectTypeRegistry.getAll().filter((effect) => {
    const resolvedType = EffectTypeRegistry.resolve(effect.type);
    if (effect.catalogHidden) return false;
    if (resolvedType === EffectGuids.kAmpNamBlend) return false;
    if (resolvedType === EffectGuids.kWasmHost && !isFeatureEnabled(Features.CustomEffects)) return false;
    if (experimentalEffectTypes.has(resolvedType) && !isFeatureEnabled(Features.ExperimentalEffects)) return false;
    return !excludedTypes.has(resolvedType);
  });
}

export function getOrderedFxCategories(items: Array<{ category: string }>): string[] {
  const categoriesWithContent = new Set(items.map((item) => item.category).filter(Boolean));
  const metadataOrder = Object.keys(CATEGORY_METADATA);
  return [
    ...metadataOrder.filter((id) => categoriesWithContent.has(id)),
    ...[...categoriesWithContent].filter((id) => !CATEGORY_METADATA[id]).sort(),
  ];
}

/** Every entry, in the order the library lists them: effects, blends, custom effects, composites. */
export function getFxLibraryItems(options?: { excludeTypes?: string[] }): FxLibraryItem[] {
  return [
    ...getCatalogEffects(options),
    ...getBlendFxItems(),
    ...getCustomEffectFxItems(),
    ...getCompositeFxItems(),
  ].filter((item) => !(options?.excludeTypes ?? []).includes(item.type));
}

type BlendFxItem = EffectTypeInfo & { blendId: string; blendCategory: string };

function getBlendFxItems(): BlendFxItem[] {
  const blends = uiState.blendLibrary ?? [];

  return blends.map((blend) => {
    const mappedCategory = BLEND_CATEGORIES[blend.category] ?? DEFAULT_BLEND_CATEGORY;
    return {
      type: EffectGuids.kAmpNamBlend,
      displayName: blend.name || "Custom Blend",
      category: mappedCategory,
      requiresResource: true,
      resourceType: "nam",
      parameters: EffectTypeRegistry.get(EffectGuids.kAmpNamBlend)?.parameters ?? [],
      blendId: blend.id,
      blendCategory: blend.category,
    };
  });
}

type CustomEffectFxItem = EffectTypeInfo & {
  customEffectId: string;
  moduleResourceType: string;
  moduleResourceId: string;
  defaultParams?: Record<string, number>;
};

function getCustomEffectFxItems(): CustomEffectFxItem[] {
  if (!isFeatureEnabled(Features.CustomEffects)) {
    return [];
  }
  const wasmInfo = EffectTypeRegistry.get(EffectGuids.kWasmHost);
  return getCustomEffectLibrary().map((entry) => ({
    type: EffectGuids.kWasmHost,
    displayName: entry.name || "Custom Effect",
    category: entry.category || "utility",
    description: entry.description ?? "",
    thumbnailDataUrl: entry.thumbnailDataUrl,
    requiresResource: false,
    parameters: wasmInfo?.parameters ?? [],
    customEffectId: entry.id,
    moduleResourceType: entry.moduleResourceType,
    moduleResourceId: entry.moduleResourceId,
    defaultParams: entry.defaultParams,
  }));
}

type CompositeFxItem = EffectTypeInfo & { compositeId: string; description: string };

function getCompositeFxItems(): CompositeFxItem[] {
  return getCompositeEffectEntries().map((entry) => ({
    type: entry.type,
    displayName: entry.displayName,
    category: entry.category,
    requiresResource: false,
    parameters: [],
    compositeId: entry.type.replace("composite:", ""),
    description: entry.description,
  }));
}
