import { beforeEach, describe, expect, it, vi } from "vitest";

const mocks = vi.hoisted(() => ({
  preset: null as unknown,
  confirm: vi.fn(async () => true),
  replace: vi.fn(),
}));

vi.mock("../ts/state.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  getSignalPathPreset: () => mocks.preset,
}));
vi.mock("../ts/dialogs.js", () => ({ showConfirm: mocks.confirm }));
vi.mock("../ts/signalPath.js", () => ({ replaceNodeWithFxItem: mocks.replace }));

import { BLEND_SAVED_FROM_NODE_EVENT, type BlendEditorOpenDetail } from "../ts/blendUtils.js";
import { EffectGuids } from "../ts/effectGuids.js";
import { initBlendNodeReplacement } from "../ts/signalPath/blendReplacement.js";
import type { BlendDefinition } from "../ts/types.js";

const blend: BlendDefinition = { id: "b1", name: "JCM Blend", category: "amp", models: ["m1", "m2"] };
const namNode = { id: "n1", type: EffectGuids.kAmpNamOptimized, displayName: "NAM Amp" };

async function saveFromNode(detail: BlendEditorOpenDetail): Promise<void> {
  document.dispatchEvent(new CustomEvent(BLEND_SAVED_FROM_NODE_EVENT, { detail }));
  await new Promise((resolve) => setTimeout(resolve, 0));
}

describe("offering a saved blend in place of the node it was made from", () => {
  beforeEach(() => {
    initBlendNodeReplacement();
    mocks.preset = { id: "p1", graph: { nodes: [namNode] } };
    mocks.confirm.mockReset().mockResolvedValue(true);
    mocks.replace.mockReset();
  });

  it("asks, then replaces the node with the blend effect", async () => {
    await saveFromNode({ blend, sourceNode: { presetId: "p1", nodeId: "n1" } });
    expect(mocks.confirm).toHaveBeenCalledOnce();
    expect(mocks.replace).toHaveBeenCalledWith(namNode, mocks.preset, {
      effectType: EffectGuids.kAmpNamBlend,
      blendId: "b1",
      blendName: "JCM Blend",
      blendCategory: "amp",
    });
  });

  it("keeps the node when the user says no", async () => {
    mocks.confirm.mockResolvedValue(false);
    await saveFromNode({ blend, sourceNode: { presetId: "p1", nodeId: "n1" } });
    expect(mocks.replace).not.toHaveBeenCalled();
  });

  it("asks nothing once the node is gone, another preset is shown, or it is already a blend", async () => {
    await saveFromNode({ blend, sourceNode: { presetId: "p1", nodeId: "missing" } });
    await saveFromNode({ blend, sourceNode: { presetId: "p2", nodeId: "n1" } });
    mocks.preset = { id: "p1", graph: { nodes: [{ ...namNode, type: EffectGuids.kAmpNamBlend }] } };
    await saveFromNode({ blend, sourceNode: { presetId: "p1", nodeId: "n1" } });
    expect(mocks.confirm).not.toHaveBeenCalled();
    expect(mocks.replace).not.toHaveBeenCalled();
  });

  it("does not replace a node that went away while the question was open", async () => {
    mocks.confirm.mockImplementation(async () => {
      mocks.preset = { id: "p1", graph: { nodes: [] } };
      return true;
    });
    await saveFromNode({ blend, sourceNode: { presetId: "p1", nodeId: "n1" } });
    expect(mocks.replace).not.toHaveBeenCalled();
  });
});
