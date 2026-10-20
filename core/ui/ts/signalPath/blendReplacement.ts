/**
 * A blend made from a NAM node (Create Blend in that node's model picker) is offered in the
 * node's place when its editor is saved. The blend editor knows nothing of the chain, so it
 * announces the save with `BLEND_SAVED_FROM_NODE_EVENT` and this module asks, then makes the
 * swap the way the FX library replaces a node.
 */

import { BLEND_SAVED_FROM_NODE_EVENT, type BlendEditorOpenDetail } from "../blendUtils.js";
import { showConfirm } from "../dialogs.js";
import { EffectGuids } from "../effectGuids.js";
import { getNodeEffectInfo } from "../presetV2.js";
import { replaceNodeWithFxItem } from "../signalPath.js";
import { getSignalPathPreset } from "../state.js";
import type { GraphNode, Preset } from "../types.js";
import { getNodeDisplayName, getNodeResourceDisplayName } from "./nodeLabels.js";

let bound = false;

/** The node as the chain shows it now, if it is still there and still not a blend. */
function findSourceNode(presetId: string, nodeId: string): { preset: Preset; node: GraphNode } | null {
  const preset = getSignalPathPreset();
  const node = preset?.id === presetId ? preset.graph?.nodes.find((candidate) => candidate.id === nodeId) : undefined;
  return preset && node && node.type !== EffectGuids.kAmpNamBlend ? { preset, node } : null;
}

async function offerBlendInPlaceOfNode(detail: BlendEditorOpenDetail): Promise<void> {
  const source = detail.sourceNode;
  // The chain may have moved on while the editor was open: another preset shown, the node
  // deleted or replaced. The blend is saved either way, and in the FX library.
  const found = source ? findSourceNode(source.presetId, source.nodeId) : null;
  if (!source || !found) {
    return;
  }

  const { node } = found;
  const nodeName = (getNodeEffectInfo(node)?.requiresResource ? getNodeResourceDisplayName(node, 0) : "") || getNodeDisplayName(node);
  const replace = await showConfirm(
    `Replace "${nodeName}" with the blend "${detail.blend.name}"? The blend takes its place in the chain.`,
    "Use the New Blend?",
    { ok: "Replace", cancel: "Keep" },
  );
  if (!replace) {
    return;
  }

  // Looked up again: the chain can re-render while the question is open.
  const live = findSourceNode(source.presetId, source.nodeId);
  if (!live) {
    return;
  }
  replaceNodeWithFxItem(live.node, live.preset, {
    effectType: EffectGuids.kAmpNamBlend,
    blendId: detail.blend.id,
    blendName: detail.blend.name,
    blendCategory: detail.blend.category,
  });
}

export function initBlendNodeReplacement(): void {
  if (bound) {
    return;
  }
  bound = true;
  document.addEventListener(BLEND_SAVED_FROM_NODE_EVENT, (event) => {
    const detail = (event as CustomEvent<BlendEditorOpenDetail | undefined>).detail;
    if (detail?.blend && detail.sourceNode) {
      void offerBlendInPlaceOfNode(detail);
    }
  });
}
