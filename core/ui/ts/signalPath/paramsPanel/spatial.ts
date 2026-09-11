/**
 * The spatial panner: reading a position out of the node's parameters, drawing
 * it, and writing a dragged position back.
 */

import { EffectGuids } from "../../effectGuids.js";
import { EffectTypeRegistry } from "../../presetV2.js";
import { SpatialPannerInteraction } from "../../spatialPanner.js";
import type { SpatialLiveState, SpatialPosition } from "../../spatialPanner.js";
import { getActivePresetForRender } from "../../state.js";
import type { GraphNode } from "../../types.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";
import { showNodeParamsPanel } from "./panel.js";
import { nodeParamKnobs, paramsPanelInteractions } from "./state.js";

export const SPATIAL_PARAM_KEYS: ReadonlyArray<keyof SpatialPosition> = ["azimuth", "elevation", "distance"];

export function readSpatialPosition(node: GraphNode): SpatialPosition {
  const params = node.params ?? {};
  return {
    azimuth: typeof params.azimuth === "number" ? params.azimuth : 0,
    elevation: typeof params.elevation === "number" ? params.elevation : 0,
    distance: typeof params.distance === "number" ? params.distance : 1.5,
  };
}

export function updateSpatialVisualization(node: GraphNode): void {
  if (EffectTypeRegistry.resolve(node.type) !== EffectGuids.kSpatial3D) {
    return;
  }
  const canvas = nodeParamsPanelElement?.querySelector(".spatial-panner-canvas") as HTMLCanvasElement | null;
  if (!canvas) {
    return;
  }

  const position = readSpatialPosition(node);
  const speakerMode = (node.params?.listenMode ?? 0) >= 0.5;

  if (paramsPanelInteractions.spatial && paramsPanelInteractions.spatialNodeId === node.id && canvas.isConnected) {
    paramsPanelInteractions.spatial.updatePosition(position);
    paramsPanelInteractions.spatial.setSpeakerMode(speakerMode);
    return;
  }

  // Either a different node or a rebuilt panel: the old canvas is gone, so the old
  // interaction's listeners point at a detached element and must be torn down.
  if (paramsPanelInteractions.spatial) {
    paramsPanelInteractions.spatial.destroy();
    paramsPanelInteractions.spatial = null;
  }

  const apply = (next: SpatialPosition, rebuildPanel: boolean): void => {
    for (const key of SPATIAL_PARAM_KEYS) {
      const value = next[key];
      if (node.params[key] === value) continue;
      node.params[key] = value;
      sendSignalPathNodeParamUpdate(node.id, key, value);
      const knob = nodeParamKnobs.get(key);
      if (knob) knob.setValue(value);
    }
    if (rebuildPanel) {
      const preset = getActivePresetForRender();
      if (preset) showNodeParamsPanel(node, preset);
    }
  };

  paramsPanelInteractions.spatial = new SpatialPannerInteraction(
    canvas,
    position,
    (next) => apply(next, false),
    // Committing does not rebuild the panel: the knobs are already synced above, and
    // a rebuild would replace the canvas mid-gesture and drop pointer capture.
    (next) => apply(next, false)
  );
  paramsPanelInteractions.spatial.setSpeakerMode(speakerMode);
  paramsPanelInteractions.spatialNodeId = node.id;
}

/**
 * Live source positions pushed by the DSP. Only the node currently on screen is of
 * interest; everything else is discarded so a chain full of spatialisers costs nothing.
 */
export function applySpatialPositionUpdate(
  nodes: Array<{ nodeId?: unknown } & Partial<SpatialLiveState>>
): void {
  if (!paramsPanelInteractions.spatial || !paramsPanelInteractions.spatialNodeId) {
    return;
  }
  const match = Array.isArray(nodes)
    ? nodes.find((entry) => entry && entry.nodeId === paramsPanelInteractions.spatialNodeId)
    : undefined;
  if (!match) {
    paramsPanelInteractions.spatial.setLiveState(null);
    return;
  }
  const num = (value: unknown, fallback: number): number =>
    typeof value === "number" && Number.isFinite(value) ? value : fallback;
  paramsPanelInteractions.spatial.setLiveState({
    azimuth: num(match.azimuth, 0),
    elevation: num(match.elevation, 0),
    distance: num(match.distance, 1.5),
    itdUs: num(match.itdUs, 0),
    ildDb: num(match.ildDb, 0),
    moving: match.moving === true,
  });
}
