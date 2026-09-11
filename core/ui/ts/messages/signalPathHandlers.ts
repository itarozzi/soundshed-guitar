/**
 * Node-level changes the engine reports back: config, parameter and bypass
 * updates, the global chain, and the signal-path test result.
 */

import { syncControlsFromState } from "../controls.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { renderActivePreset } from "../presets.js";
import { setNodeParam } from "../presetV2.js";
import { applySpatialPositionUpdate, refreshSelectedNodeParams, renderSignalPathBar } from "../signalPath.js";
import { shouldMarkSignalPathNodeConfigUpdateDirty } from "../signalPathConfigUpdates.js";
import { getActivePresetForRender, setActivePresetDraft, setPresetDirty, uiState } from "../state.js";
import type { GlobalSignalChainConfig, Preset } from "../types.js";
import { normalizeGlobalSignalChain } from "./normalize.js";
import type { IncomingPayload, SignalPathNodeConfigUpdateOptions } from "./types.js";

export function applySignalPathNodeConfigUpdate(
  nodeId: string,
  key: string,
  value: string | undefined,
  valueLength?: number,
  options: SignalPathNodeConfigUpdateOptions = {},
): boolean {
  const preset = getActivePresetForRender();
  if (!preset) {
    return false;
  }

  const updateGraph = (graph: Preset["graph"] | undefined): boolean => {
    const node = graph?.nodes?.find((candidate) => candidate.id === nodeId);
    if (!node) {
      return false;
    }
    node.config = { ...(node.config ?? {}) };
    if (typeof value === "string") {
      node.config[key] = value;
    }
    if (key === "pluginStateBase64" && typeof valueLength === "number") {
      node.config.pluginStateBase64Length = `${valueLength}`;
    }
    return true;
  };

  let updated = updateGraph(preset.graph);
  for (const scene of preset.scenes ?? []) {
    updated = updateGraph(scene.graph) || updated;
  }

  if (!updated) {
    return false;
  }

  setActivePresetDraft(preset);
  if (options.markDirty !== false) {
    setPresetDirty(true);
  }
  refreshSelectedNodeParams();
  renderSignalPathBar();
  return true;
}

export function onSignalPathTestResult(payload: IncomingPayload): void {
  const result = payload as Record<string, unknown>;
  uiState.signalTest = {
    frequency: (result.frequency as number) ?? 0,
    duration: (result.duration as number) ?? 0,
    elapsed: (result.elapsed as number) ?? 0,
    sampleRate: (result.sampleRate as number) ?? 0,
    inputRMS: (result.inputRMS as number) ?? 0,
    outputLeft: Array.isArray(result.outputRMS) ? ((result.outputRMS as number[])[0] ?? 0) : 0,
    outputRight: Array.isArray(result.outputRMS) ? ((result.outputRMS as number[])[1] ?? 0) : 0,
    passed: Boolean(result.passed),
    message: (result.message as string) ?? "",
  };
  renderActivePreset();
  const ratio = uiState.signalTest.elapsed > 0 ? (uiState.signalTest.duration / uiState.signalTest.elapsed).toFixed(2) : "N/A";
  const timingInfo = `Audio: ${uiState.signalTest.duration.toFixed(3)}s, Elapsed: ${uiState.signalTest.elapsed.toFixed(3)}s (${ratio}x realtime)`;
  showNotification(
    uiState.signalTest.passed ? "Signal path test passed" : "Signal path test failed",
    timingInfo + (uiState.signalTest.message ? ` - ${uiState.signalTest.message}` : ""),
  );
}

export function onSpatialPosition(payload: IncomingPayload): void {
  // Live source position from the spatialiser's motion engine, so the on-screen
  // puck matches what is being heard. Purely cosmetic: if it never arrives, the
  // widget just shows the anchor position instead.
  const nodes = payload.nodes;
  if (Array.isArray(nodes)) {
    applySpatialPositionUpdate(nodes as never);
  }
}

export function onSignalPathNodeConfigUpdated(payload: IncomingPayload): void {
  const update = payload as {
    nodeId?: string;
    key?: string;
    value?: string;
    valueLength?: number;
    captured?: boolean;
    dirty?: boolean;
    persist?: boolean;
    silent?: boolean;
  };
  if (typeof update.nodeId === "string" && typeof update.key === "string") {
    const markDirty = shouldMarkSignalPathNodeConfigUpdateDirty(update);
    let applied = false;
    if (typeof update.value === "string") {
      applied = applySignalPathNodeConfigUpdate(update.nodeId, update.key, update.value, update.valueLength, { markDirty });
    } else if (update.captured && update.key === "pluginStateBase64") {
      applied = applySignalPathNodeConfigUpdate(update.nodeId, update.key, undefined, update.valueLength, { markDirty });
    }
    if (!applied && markDirty) {
      setPresetDirty(true);
    }
    if (update.key === "pluginStateBase64" && !update.silent) {
      showNotification("Plugin state captured");
    }
  }
}

export function onSignalPathNodeParamUpdated(payload: IncomingPayload): void {
  const update = payload as { nodeId?: string; key?: string; value?: number };
  if (typeof update.nodeId === "string" && typeof update.key === "string" && typeof update.value === "number") {
    const preset = getActivePresetForRender();
    if (preset) {
      try {
        setNodeParam(preset, update.nodeId, update.key, update.value);
      } catch {
        // Node not in the active preset draft — ignore silently.
      }
    }
    refreshSelectedNodeParams();
  }
}

export function onSignalPathNodeBypassUpdated(payload: IncomingPayload): void {
  const update = payload as { nodeId?: string; bypassed?: boolean };
  if (typeof update.nodeId === "string" && typeof update.bypassed === "boolean") {
    const preset = getActivePresetForRender();
    const node = preset?.graph?.nodes?.find((n) => n.id === update.nodeId);
    if (node) {
      (node as unknown as { bypassed?: boolean }).bypassed = update.bypassed;
      (node as unknown as { enabled?: boolean }).enabled = !update.bypassed;
    }
    refreshSelectedNodeParams();
    renderActivePreset();
  }
}

export function onGlobalSignalChainChanged(payload: IncomingPayload): void {
  const chainPayload = payload as { config?: GlobalSignalChainConfig; globalSignalChain?: GlobalSignalChainConfig };
  const chainConfig = chainPayload.config ?? chainPayload.globalSignalChain;
  if (chainConfig) {
    uiState.globalSignalChain = normalizeGlobalSignalChain(chainConfig) ?? uiState.globalSignalChain;
    appendLog("Global signal chain configuration loaded");
    syncControlsFromState();
  }
}
