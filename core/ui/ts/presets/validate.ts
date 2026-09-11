/**
 * The gate every preset passes through before the UI trusts it.
 *
 * A preset can arrive from disk, from an archive, or from another user, so the
 * cleanup pass is allow-list based: an unknown key is dropped rather than carried
 * into the engine, and the report says what was dropped and why.
 */

import { sanitizePresetForArchive } from "../presets/sanitize.js";
import { DEFAULT_GLOBAL_SIGNAL_CHAIN, clonePreset } from "../state.js";
import type { GraphNode, Preset, ResourceRef } from "../types.js";

export const PRESET_ALLOWED_KEYS = new Set([
  "id",
  "name",
  "category",
  "description",
  "attachments",
  "fxChain",
  "audioFxModelId",
  "irId",
  "customModelPath",
  "customIrPath",
  "formatVersion",
  "graph",
  "scenes",
  "globalSignalChain",
  "embeddedResources",
  "version",
  "author",
  "tags",
  "createdAt",
  "modifiedAt",
]);

export const PRESET_OPTIONAL_STRING_KEYS = [
  "category",
  "description",
  "customModelPath",
  "customIrPath",
  "author",
];

export const PRESET_OPTIONAL_ARRAY_KEYS = [
  "attachments",
  "fxChain",
  "embeddedResources",
  "scenes",
  "tags",
];

export function stripGlobalSignalChainForSave(preset: Preset): Preset {
  const cleaned = sanitizePresetForArchive(preset);
  if (Array.isArray(cleaned.scenes) && cleaned.scenes.length > 0) {
    delete (cleaned as Record<string, unknown>).graph;
  }
  return cleaned;
}

export function summarizePresetForSaveLog(preset: Preset): string {
  const graphNodes = preset.graph?.nodes?.length ?? 0;
  const graphEdges = preset.graph?.edges?.length ?? 0;
  const scenes = preset.scenes ?? [];
  const sceneSummary = scenes.length
    ? scenes.map((scene) => `${scene.id}:${scene.graph?.nodes?.length ?? 0}`).join(",")
    : "none";
  return `graphNodes=${graphNodes}, graphEdges=${graphEdges}, scenes=${scenes.length}[${sceneSummary}]`;
}

export function validatePresetForUi(preset: Preset | null): string[] {
  if (!preset) {
    return ["No preset loaded."];
  }

  const issues: string[] = [];
  const unknownKeys = Object.keys(preset).filter((key) => !PRESET_ALLOWED_KEYS.has(key));
  if (unknownKeys.length) {
    issues.push(`Unknown fields: ${unknownKeys.sort().join(", ")}`);
  }

  if (typeof preset.id !== "string" || !preset.id.trim()) {
    issues.push("Missing or invalid preset id.");
  }

  if (typeof preset.name !== "string" || !preset.name.trim()) {
    issues.push("Missing or invalid preset name.");
  }

  if (preset.graph) {
    if (!Array.isArray(preset.graph.nodes)) {
      issues.push("Preset graph.nodes must be an array.");
    }
    if (!Array.isArray(preset.graph.edges)) {
      issues.push("Preset graph.edges must be an array.");
    }
    preset.graph.nodes?.forEach((node, index) => {
      if (!node || typeof node.id !== "string" || !node.id.trim()) {
        issues.push(`Graph node #${index + 1} is missing a valid id.`);
      }
      if (!node || typeof node.type !== "string" || !node.type.trim()) {
        issues.push(`Graph node #${index + 1} is missing a valid type.`);
      }
    });
    preset.graph.edges?.forEach((edge, index) => {
      if (!edge || typeof edge.from !== "string" || typeof edge.to !== "string") {
        issues.push(`Graph edge #${index + 1} is missing valid endpoints.`);
      }
    });
  }

  if (preset.scenes && !Array.isArray(preset.scenes)) {
    issues.push("Preset scenes must be an array when present.");
  }

  return issues;
}

export function cleanupPresetForUi(
  preset: Preset,
): { cleaned: Preset; removedKeys: string[]; normalizedAliases: number; removedGlobalEq: boolean } {
  const cleaned: Preset = clonePreset(preset);
  const removedKeys: string[] = [];
  let normalizedAliases = 0;
  let removedGlobalEq = false;

  const normalizeRef = (ref: ResourceRef): ResourceRef => {
    const normalized: ResourceRef = { ...ref };
    const legacyId = typeof normalized.id === "string" ? normalized.id : "";
    const legacyType = typeof normalized.type === "string" ? normalized.type : "";
    const resourceId = typeof normalized.resourceId === "string" ? normalized.resourceId : "";
    const resourceType = typeof normalized.resourceType === "string" ? normalized.resourceType : "";

    const finalId = resourceId || legacyId;
    const finalType = resourceType || legacyType;

    if (finalId) {
      normalized.resourceId = finalId;
    }
    if (finalType) {
      normalized.resourceType = finalType;
    }

    if (legacyId && legacyId === normalized.resourceId) {
      delete normalized.id;
      normalizedAliases += 1;
    }
    if (legacyType && legacyType === normalized.resourceType) {
      delete normalized.type;
      normalizedAliases += 1;
    }

    return normalized;
  };

  Object.keys(cleaned).forEach((key) => {
    if (!PRESET_ALLOWED_KEYS.has(key)) {
      delete (cleaned as Record<string, unknown>)[key];
      removedKeys.push(key);
    }
  });

  if ("globals" in cleaned) {
    delete (cleaned as Record<string, unknown>).globals;
    removedKeys.push("globals");
  }
  if ("global" in cleaned) {
    delete (cleaned as Record<string, unknown>).global;
    removedKeys.push("global");
  }
  if ("globalSignalChain" in cleaned) {
    delete (cleaned as Record<string, unknown>).globalSignalChain;
    removedKeys.push("globalSignalChain");
  }

  PRESET_OPTIONAL_STRING_KEYS.forEach((key) => {
    const value = cleaned[key] as string | null | undefined;
    if (typeof value === "string" && value.trim() === "") {
      delete (cleaned as Record<string, unknown>)[key];
      removedKeys.push(key);
    }
  });

  PRESET_OPTIONAL_ARRAY_KEYS.forEach((key) => {
    const value = cleaned[key] as unknown;
    if (Array.isArray(value) && value.length === 0) {
      delete (cleaned as Record<string, unknown>)[key];
      removedKeys.push(key);
    }
  });

  if (cleaned.graph?.nodes) {
    cleaned.graph.nodes = cleaned.graph.nodes.map((node) => {
      const nextNode = { ...node };
      const normalizedResources = Array.isArray(nextNode.resources)
        ? nextNode.resources.map((ref) => normalizeRef(ref))
        : [];
      if (normalizedResources.length || Array.isArray(nextNode.resources)) {
        nextNode.resources = normalizedResources;
      }
      return nextNode;
    });
  }

  if (cleaned.graph?.nodes?.length && cleaned.graph?.edges?.length) {
    const eqDefaults = (() => {
      const fallback = {
        lowGain: 0.0,
        lowFreq: 100.0,
        lowMidGain: 0.0,
        lowMidFreq: 400.0,
        lowMidQ: 1.0,
        highMidGain: 0.0,
        highMidFreq: 2000.0,
        highMidQ: 1.0,
        highGain: 0.0,
        highFreq: 8000.0,
      };
      const eqNode = DEFAULT_GLOBAL_SIGNAL_CHAIN.postChainGraph.nodes.find((node) => node.id === "global_eq" || node.type === "eq_parametric");
      if (!eqNode || !eqNode.params) {
        return fallback;
      }
      return {
        lowGain: eqNode.params.lowGain ?? fallback.lowGain,
        lowFreq: eqNode.params.lowFreq ?? fallback.lowFreq,
        lowMidGain: eqNode.params.lowMidGain ?? fallback.lowMidGain,
        lowMidFreq: eqNode.params.lowMidFreq ?? fallback.lowMidFreq,
        lowMidQ: eqNode.params.lowMidQ ?? fallback.lowMidQ,
        highMidGain: eqNode.params.highMidGain ?? fallback.highMidGain,
        highMidFreq: eqNode.params.highMidFreq ?? fallback.highMidFreq,
        highMidQ: eqNode.params.highMidQ ?? fallback.highMidQ,
        highGain: eqNode.params.highGain ?? fallback.highGain,
        highFreq: eqNode.params.highFreq ?? fallback.highFreq,
      };
    })();
    const isDefaultGlobalEqNode = (node: GraphNode): boolean => {
      const anyNode = node as unknown as {
        id?: unknown;
        type?: unknown;
        label?: unknown;
        enabled?: unknown;
        bypassed?: unknown;
        params?: Record<string, number>;
      };
      const nodeId = typeof anyNode.id === "string" ? anyNode.id : "";
      const nodeType = typeof anyNode.type === "string" ? anyNode.type : "";
      const nodeLabel = typeof anyNode.label === "string" ? anyNode.label : "";
      if (nodeId !== "global_eq" && nodeLabel !== "Global EQ") {
        return false;
      }
      if (nodeType && nodeType !== "eq_parametric") {
        return false;
      }
      const enabled = typeof anyNode.enabled === "boolean"
        ? anyNode.enabled
        : (typeof anyNode.bypassed === "boolean" ? !anyNode.bypassed : true);
      if (enabled) {
        return false;
      }
      const params = anyNode.params ?? {};
      return (
        params.lowGain === eqDefaults.lowGain
        && params.lowFreq === eqDefaults.lowFreq
        && params.lowMidGain === eqDefaults.lowMidGain
        && params.lowMidFreq === eqDefaults.lowMidFreq
        && params.lowMidQ === eqDefaults.lowMidQ
        && params.highMidGain === eqDefaults.highMidGain
        && params.highMidFreq === eqDefaults.highMidFreq
        && params.highMidQ === eqDefaults.highMidQ
        && params.highGain === eqDefaults.highGain
        && params.highFreq === eqDefaults.highFreq
      );
    };

    const eqNode = cleaned.graph.nodes.find((node) => isDefaultGlobalEqNode(node));
    if (eqNode) {
      const eqId = eqNode.id;
      const hasInputToEq = cleaned.graph.edges.some((edge) => edge.from === "__input__" && edge.to === eqId);
      const hasEqToDoubler = cleaned.graph.edges.some((edge) => edge.from === eqId && edge.to === "global_doubler");
      const hasEqToOutput = cleaned.graph.edges.some((edge) => edge.from === eqId && edge.to === "__output__");
      const hasDoubler = cleaned.graph.nodes.some((node) => node.id === "global_doubler");
      const hasOutput = cleaned.graph.nodes.some((node) => node.id === "__output__");

      cleaned.graph.nodes = cleaned.graph.nodes.filter((node) => node.id !== eqId);
      cleaned.graph.edges = cleaned.graph.edges.filter((edge) => edge.from !== eqId && edge.to !== eqId);

      if (hasInputToEq && hasEqToDoubler && hasDoubler) {
        const alreadyLinked = cleaned.graph.edges.some((edge) => edge.from === "__input__" && edge.to === "global_doubler");
        if (!alreadyLinked) {
          cleaned.graph.edges.push({ from: "__input__", to: "global_doubler", fromPort: 0, toPort: 0, gain: 1 });
        }
      } else if (hasInputToEq && hasEqToOutput && hasOutput) {
        const alreadyLinked = cleaned.graph.edges.some((edge) => edge.from === "__input__" && edge.to === "__output__");
        if (!alreadyLinked) {
          cleaned.graph.edges.push({ from: "__input__", to: "__output__", fromPort: 0, toPort: 0, gain: 1 });
        }
      }

      removedGlobalEq = true;
    }
  }

  if (cleaned.audioFxModelId == null || cleaned.audioFxModelId === "") {
    delete cleaned.audioFxModelId;
    removedKeys.push("audioFxModelId");
  }

  if (cleaned.irId == null || cleaned.irId === "") {
    delete cleaned.irId;
    removedKeys.push("irId");
  }

  if (cleaned.customModelPath == null || cleaned.customModelPath === "") {
    delete cleaned.customModelPath;
    removedKeys.push("customModelPath");
  }

  if (cleaned.customIrPath == null || cleaned.customIrPath === "") {
    delete cleaned.customIrPath;
    removedKeys.push("customIrPath");
  }

  return {
    cleaned,
    removedKeys: removedKeys.filter((value, index) => removedKeys.indexOf(value) === index),
    normalizedAliases,
    removedGlobalEq,
  };
}

export function hasGraphNodes(preset: Preset | null | undefined): boolean {
  return Boolean(preset?.graph && Array.isArray(preset.graph.nodes) && preset.graph.nodes.length > 0);
}
