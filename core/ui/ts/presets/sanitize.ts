import { clonePreset, uiState } from "../state.js";
import type { Preset, PresetArchiveSessionState } from "../types.js";
export function getPresetArchiveSessionState(): PresetArchiveSessionState | null {
  return uiState.presetArchiveSession?.active ? uiState.presetArchiveSession : null;
}

export function applyPresetArchiveSessionState(state: PresetArchiveSessionState | null): void {
  if (state?.active) {
    uiState.presetArchiveSession = {
      active: true,
      archiveName: state.archiveName ?? uiState.presetArchiveSession?.archiveName,
      archiveKey: state.archiveKey ?? uiState.presetArchiveSession?.archiveKey,
      presetCount: typeof state.presetCount === "number" ? state.presetCount : uiState.presetArchiveSession?.presetCount,
    };
  } else {
    uiState.presetArchiveSession = null;
  }
}

export function stripLegacyGlobals(preset: Preset): Preset {
  const cleaned = clonePreset(preset);
  delete (cleaned as Record<string, unknown>).globals;
  delete (cleaned as Record<string, unknown>).global;
  return cleaned;
}

export function sanitizePresetForArchive(preset: Preset): Preset {
  const cleaned = clonePreset(preset);
  delete (cleaned as Record<string, unknown>).globals;
  delete (cleaned as Record<string, unknown>).global;
  delete (cleaned as Record<string, unknown>).globalSignalChain;
  return cleaned;
}

/** A node id an imported preset may keep. The engine applies the same rule (IsSafeImportedNodeId). */
export const SAFE_NODE_ID = /^[\w-]{1,64}$/;

function newNodeId(): string {
  return typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `node-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

/**
 * Makes a preset from an archive or a Tone Sharing pack safe to store and show, in place.
 *
 * - A node id that is not 1-64 of `[A-Za-z0-9_-]` gets a fresh one: node ids go into
 *   markup and selectors all over the UI. The same new id is used in the graph and every
 *   scene, and edges follow it; an edge naming no node is dropped. (Automation is not part
 *   of a preset, so nothing else refers to them.)
 * - Resource file paths are dropped. They name a file on the sharer's machine, or wherever
 *   the archive likes; a UNC path would have the engine open a remote share.
 */
export function sanitizeImportedPreset(preset: Preset): Preset {
  const graphs = [preset.graph, ...(preset.scenes ?? []).map((scene) => scene?.graph)]
    .filter((graph): graph is NonNullable<Preset["graph"]> => Boolean(graph && typeof graph === "object"));

  // An id that is not text (the JSON is not held to its type) matches nothing and gets its own.
  const idText = (id: unknown): string => (typeof id === "string" || typeof id === "number" ? String(id) : "");
  const replacements = new Map<string, string>();
  for (const graph of graphs) {
    for (const node of Array.isArray(graph.nodes) ? graph.nodes : []) {
      const id = idText(node?.id);
      if (id && !SAFE_NODE_ID.test(id) && !replacements.has(id)) {
        replacements.set(id, newNodeId());
      }
    }
  }
  const resolve = (id: unknown): string | null => {
    const text = idText(id);
    return SAFE_NODE_ID.test(text) ? text : (replacements.get(text) ?? null);
  };

  for (const graph of graphs) {
    graph.nodes = (Array.isArray(graph.nodes) ? graph.nodes : []).filter((node) => node && typeof node === "object");
    for (const node of graph.nodes) {
      node.id = resolve(node.id) ?? newNodeId();
      // `resource` is the older single-slot form, which the engine still reads.
      const legacyResource = (node as { resource?: { filePath?: unknown } }).resource;
      for (const resource of [...(Array.isArray(node.resources) ? node.resources : []), legacyResource]) {
        if (resource && typeof resource === "object") {
          delete resource.filePath;
        }
      }
    }
    graph.edges = (Array.isArray(graph.edges) ? graph.edges : []).flatMap((edge) => {
      const from = resolve(edge?.from);
      const to = resolve(edge?.to);
      return from && to ? [{ ...edge, from, to }] : [];
    });
  }

  delete preset.customModelPath;
  delete preset.customIrPath;
  for (const attachment of Array.isArray(preset.attachments) ? preset.attachments : []) {
    delete attachment.filePath;
    delete attachment.path;
    delete attachment.customModelPath;
    delete attachment.customIrPath;
  }
  for (const embedded of Array.isArray(preset.embeddedResources) ? preset.embeddedResources : []) {
    delete embedded.originalPath;
  }
  return preset;
}
