/**
 * Double-clicking a node in the chain opens the FX library to replace it. The library names
 * the node and marks the effect in it now, and a click on another effect puts that effect in
 * the node's place, with no drag (which a touch screen makes awkward). Dragging from the
 * library works as it always has.
 *
 * The library knows nothing of the chain, so this module tells it which node it replaces,
 * keeps that current as the chain re-renders, and hands the choice to the signal path, which
 * owns what a replacement does.
 */

import { revealCompactNodeDetail } from "../compactStage.js";
import {
  focusFxSelectorCategory,
  getFxSelectorReplaceTarget,
  isSameFxItem,
  setFxSelectorReplaceTarget,
  type FxItemIdentity,
  type FxPointerDragPayload,
} from "../fxSelector.js";
import { getNodeEffectInfo } from "../presetV2.js";
import { getSignalPathPreset } from "../state.js";
import type { GraphNode, Preset } from "../types.js";
import { isProtectedSignalPathNode } from "./bypass.js";
import { getNodeDisplayName, getNodeResourceDisplayName } from "./nodeLabels.js";
import { getNodeCategory } from "./nodeTypes.js";

export type ReplaceNodeWithFxItem = (node: GraphNode, preset: Preset, payload: FxPointerDragPayload) => void;

function nodeIdentity(node: GraphNode): FxItemIdentity {
  return { effectType: node.type, blendId: node.config?.blendId, customEffectId: node.config?.customEffectId };
}

/** What the node's tile is called: its model or IR when it has one, as the chain shows it. */
function nodeName(node: GraphNode): string {
  const resourceName = getNodeEffectInfo(node)?.requiresResource ? getNodeResourceDisplayName(node, 0) : "";
  return resourceName || getNodeDisplayName(node);
}

function findNode(preset: Preset | null | undefined, presetId: string, nodeId: string): GraphNode | undefined {
  return preset?.id === presetId ? preset.graph?.nodes.find((candidate) => candidate.id === nodeId) : undefined;
}

/**
 * Opens the FX library on the node's category. A splitter or mixer cannot be replaced, so
 * for one of those it opens as it does from Add FX.
 */
export function openReplaceChooser(node: GraphNode, preset: Preset, replaceNode: ReplaceNodeWithFxItem): void {
  const presetId = preset.id;
  const nodeId = node.id;
  focusFxSelectorCategory(getNodeCategory(node), {
    expand: true,
    clearSearch: true,
    replace: isProtectedSignalPathNode(node) ? undefined : {
      presetId,
      nodeId,
      name: nodeName(node),
      current: nodeIdentity(node),
      onChoose: (payload) => {
        const livePreset = getSignalPathPreset();
        const liveNode = findNode(livePreset, presetId, nodeId);
        if (!livePreset || !liveNode) {
          return;
        }
        replaceNode(liveNode, livePreset, payload);
        // As an added effect does: on a phone the chain gives way to the new effect's controls.
        revealCompactNodeDetail();
      },
    },
  });
}

/**
 * Called on each render of the chain. Once the node is gone (deleted, or another preset
 * shown) the library stays open as a plain one; when it has changed (an effect dropped on
 * it), the title and the marked effect follow.
 */
export function syncReplaceChooser(preset: Preset): void {
  const target = getFxSelectorReplaceTarget();
  if (!target) {
    return;
  }
  const node = findNode(preset, target.presetId, target.nodeId);
  if (!node) {
    setFxSelectorReplaceTarget(null);
    return;
  }
  const name = nodeName(node);
  const current = nodeIdentity(node);
  if (name !== target.name || !isSameFxItem(current, target.current)) {
    setFxSelectorReplaceTarget({ ...target, name, current });
  }
}
