/**
 * Parameter changes that come from the engine rather than the panel: MIDI and host
 * automation, which arrive as `signalPathNodeParamUpdated` up to 30 times a second per
 * parameter. Rebuilding the params panel for each one cost ~15% of the renderer's main
 * thread during a sweep, so a change the open panel can show on its knob is shown there,
 * and only what it cannot show rebuilds the panel, at most every REBUILD_INTERVAL_MS.
 */

import { showEngineParamChange } from "./paramsPanel.js";
import { requestNodeParamsRefresh } from "./render.js";
import { getSelectedNodeId } from "./state.js";
import type { GraphNode } from "../types.js";

/** The longest a change that needs a rebuild waits for it; also the most often one runs. */
const REBUILD_INTERVAL_MS = 100;

let rebuildTimer: ReturnType<typeof setTimeout> | null = null;

function scheduleRebuild(): void {
  if (rebuildTimer !== null) {
    return;
  }
  rebuildTimer = setTimeout(() => {
    rebuildTimer = null;
    requestNodeParamsRefresh();
  }, REBUILD_INTERVAL_MS);
}

/**
 * Shows an engine-side change to `node.params[key]`, which the caller has already applied.
 * Only the selected node's controls are on screen, so a change to any other node draws
 * nothing; `changed` false (the engine echoing a value the UI already had) draws nothing.
 */
export function showEngineNodeParamChange(node: GraphNode, key: string, value: number, changed: boolean): void {
  if (!changed || node.id !== getSelectedNodeId()) {
    return;
  }
  if (!showEngineParamChange(node, key, value)) {
    scheduleRebuild();
  }
}
