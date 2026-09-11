/**
 * Which library resources are actually referenced by a preset — directly, or
 * through a blend.
 *
 * Both the cleanup tool and the export need this, and they must agree: cleanup
 * deletes what export would have left out.
 */

import { clonePreset, uiState } from "../state.js";
import type { BlendDefinition, Preset, ResourceRef } from "../types.js";

export function collectPresetBlendIds(preset: Preset): string[] {
  if (!preset.graph?.nodes) {
    return [];
  }

  const ids = new Set<string>();
  preset.graph.nodes.forEach((node) => {
    const blendId = node.config?.blendId ?? "";
    if (blendId) {
      ids.add(blendId);
    }
  });

  return Array.from(ids);
}

export function collectPresetResourceRefs(preset: Preset, blendDefs: BlendDefinition[]): ResourceRef[] {
  const refs: ResourceRef[] = [];
  const seen = new Set<string>();

  const addRef = (type: string | undefined, id: string | undefined, filePath?: string): void => {
    if (!type || !id) {
      return;
    }
    const key = `${type}:${id}`;
    if (seen.has(key)) {
      return;
    }
    seen.add(key);
    refs.push({ type, id, filePath });
  };

  if (preset.graph?.nodes) {
    preset.graph.nodes.forEach((node) => {
      if (Array.isArray(node.resources)) {
        node.resources.forEach((res) => addRef(res.type, res.id, res.filePath));
      }
    });
  }

  if (preset.audioFxModelId) {
    addRef("nam", preset.audioFxModelId ?? undefined);
  }
  if (preset.irId) {
    addRef("ir", preset.irId ?? undefined);
  }

  preset.attachments?.forEach((attachment) => {
    if (!attachment.id) {
      return;
    }
    const type = attachment.type === "audiofx" ? "nam" : attachment.type === "ir" ? "ir" : attachment.type;
    addRef(type, attachment.id, attachment.filePath);
  });

  blendDefs.forEach((blend) => {
    (blend.models ?? []).forEach((modelId) => addRef("nam", modelId));
  });

  return refs;
}

export function buildUsedResourceSet(): Set<string> {
  const used = new Set<string>();
  const presets = uiState.presets.map((preset) => clonePreset(uiState.presetCache.get(preset.id) ?? preset));
  const blends = (uiState.blendLibrary ?? []).map((blend) => JSON.parse(JSON.stringify(blend)) as BlendDefinition);
  const blendIds = new Set<string>();
  presets.forEach((preset) => {
    collectPresetBlendIds(preset).forEach((id) => blendIds.add(id));
  });
  const referencedBlends = blends.filter((blend) => blendIds.has(blend.id));

  presets.forEach((preset) => {
    collectPresetResourceRefs(preset, referencedBlends).forEach((ref) => {
      if (ref.type && ref.id) {
        used.add(`${ref.type}:${ref.id}`);
      }
    });
  });

  blends.forEach((blend) => {
    (blend.models ?? []).forEach((modelId) => {
      if (modelId) {
        used.add(`nam:${modelId}`);
      }
    });
  });

  return used;
}
