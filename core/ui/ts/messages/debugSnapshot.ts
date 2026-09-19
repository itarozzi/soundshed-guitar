/**
 * The UI-side debug snapshot: a sanitised picture of what the WebView currently
 * shows, sent back on request and after interesting messages.
 *
 * Anything that looks like a key, token or path is redacted before it leaves.
 */

import { postMessage } from "../bridge.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { getActivePresetForRender, uiState } from "../state.js";
import type { Preset } from "../types.js";
import type { IncomingPayload } from "./types.js";

export const DEBUG_SNAPSHOT_SKIP_TYPES = new Set(["dspPerformance", "sld", "sldA", "sldS", "sldRoster", "spatialPosition", "captureDebugSnapshot", "debugSnapshotWritten"]);

let debugSnapshotTimer: number | null = null;

export function isSensitiveDebugKey(key: string): boolean {
  const normalizedKey = key.toLowerCase();
  return normalizedKey.includes("token")
    || normalizedKey.includes("api_key")
    || normalizedKey.includes("apikey")
    || normalizedKey.includes("secret")
    || normalizedKey.includes("password")
    || normalizedKey.includes("authorization")
    || normalizedKey.includes("cookie")
    || normalizedKey.includes("credential");
}

export function sanitizeDebugValue(value: unknown, seen = new WeakSet<object>(), currentKey = ""): unknown {
  if (isSensitiveDebugKey(currentKey)) {
    return "<redacted>";
  }
  if (value == null || typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
    return value;
  }
  if (value instanceof Date) {
    return value.toISOString();
  }
  if (Array.isArray(value)) {
    return value.map((entry) => sanitizeDebugValue(entry, seen));
  }
  if (value instanceof Map) {
    const mapped: Record<string, unknown> = {};
    value.forEach((entryValue, entryKey) => {
      const key = String(entryKey);
      mapped[key] = sanitizeDebugValue(entryValue, seen, key);
    });
    return mapped;
  }
  if (value instanceof Set) {
    return Array.from(value.values(), (entry) => sanitizeDebugValue(entry, seen));
  }
  if (typeof value === "object") {
    if (seen.has(value as object)) {
      return "[Circular]";
    }
    seen.add(value as object);
    const sanitized: Record<string, unknown> = {};
    Object.entries(value as Record<string, unknown>).forEach(([key, entryValue]) => {
      sanitized[key] = sanitizeDebugValue(entryValue, seen, key);
    });
    seen.delete(value as object);
    return sanitized;
  }
  // Primitives, dates, arrays, maps and plain objects have all returned by
  // now; only functions, symbols and bigints reach here, where String() is
  // the correct rendering.
  // eslint-disable-next-line @typescript-eslint/no-base-to-string
  return String(value);
}

export function describeElement(element: Element | null): Record<string, unknown> | null {
  if (!(element instanceof HTMLElement)) {
    return null;
  }
  return {
    tagName: element.tagName.toLowerCase(),
    id: element.id || null,
    className: element.className || null,
    ariaLabel: element.getAttribute("aria-label"),
    text: element.textContent?.trim().slice(0, 120) || null,
  };
}

export function buildUiDebugSnapshot(source: string): Record<string, unknown> {
  const activePresetForRender = getActivePresetForRender();
  return {
    capturedAt: new Date().toISOString(),
    source,
    uiState: sanitizeDebugValue(uiState),
    activePresetForRender: sanitizeDebugValue(activePresetForRender),
    document: {
      title: document.title,
      readyState: document.readyState,
      visibilityState: document.visibilityState,
      locationHref: window.location.href,
      viewport: {
        width: window.innerWidth,
        height: window.innerHeight,
      },
      activeElement: describeElement(document.activeElement),
      bodyClassName: document.body.className,
    },
  };
}

export function postUiDebugSnapshot(source: string): Record<string, unknown> {
  const snapshot = buildUiDebugSnapshot(source);
  postMessage({
    type: "debugReportUiState",
    source,
    snapshot,
  });
  return snapshot;
}

export function scheduleUiDebugSnapshot(source: string): void {
  if (debugSnapshotTimer !== null) {
    window.clearTimeout(debugSnapshotTimer);
  }
  // Long enough that a burst of messages (a preset switch is ~4) coalesces into one
  // snapshot, and that the snapshot lands after the interaction rather than during it.
  debugSnapshotTimer = window.setTimeout(() => {
    debugSnapshotTimer = null;
    postUiDebugSnapshot(source);
  }, 1500);
}

export function summarizeGraphForDebug(graph?: Preset["graph"] | null): Record<string, unknown> {
  const nodes = graph?.nodes ?? [];
  const edges = graph?.edges ?? [];
  return {
    nodeCount: nodes.length,
    edgeCount: edges.length,
    nodes: nodes.map((node) => ({ id: node.id, type: node.type })),
    edges: edges.map((edge) => ({ from: edge.from, to: edge.to })),
  };
}

export function summarizePresetForDebug(preset?: Preset | null): Record<string, unknown> | null {
  if (!preset) {
    return null;
  }

  return {
    id: preset.id,
    name: preset.name,
    graph: summarizeGraphForDebug(preset.graph),
    scenes: (preset.scenes ?? []).map((scene) => ({
      id: scene.id,
      title: scene.title,
      graph: summarizeGraphForDebug(scene.graph),
    })),
  };
}

window.SoundshedDebug = {
  captureSnapshot(reason = "manual"): Record<string, unknown> {
    return postUiDebugSnapshot(reason);
  },
  getUiSnapshot(reason = "manual"): Record<string, unknown> {
    return buildUiDebugSnapshot(reason);
  },
  getPresetSummary(): Record<string, unknown> {
    const activePresetId = uiState.activePresetId ?? null;
    const activePresetForRender = getActivePresetForRender();
    const cachedActivePreset = activePresetId ? (uiState.presetCache.get(activePresetId) ?? null) : null;
    return {
      activePresetId,
      activePresetSceneId: uiState.activePresetSceneId ?? null,
      activePresetIsNew: uiState.activePresetIsNew,
      presetDirty: uiState.presetDirty,
      activePresetForRender: summarizePresetForDebug(activePresetForRender),
      activePresetDraft: summarizePresetForDebug(uiState.activePresetDraft),
      activePresetSnapshot: summarizePresetForDebug(uiState.activePresetSnapshot),
      cachedActivePreset: summarizePresetForDebug(cachedActivePreset),
    };
  },
};

export function onCaptureDebugSnapshot(payload: IncomingPayload): void {
  const source = typeof (payload as { source?: string }).source === "string"
    ? (payload as { source?: string }).source as string
    : "backend-request";
  postUiDebugSnapshot(source);
}

export function onDebugSnapshotWritten(payload: IncomingPayload): void {
  const info = payload as { path?: string; source?: string };
  console.log("[DebugSnapshot] written", info.path ?? "", info.source ?? "");
  if (info.source === "footer-button") {
    appendLog(`debug snapshot written ← ${info.path ?? "unknown path"}`);
    showNotification("Debug state captured", info.path ?? "logs/debug-state.json");
  }
}
