import { describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/bridge.js", () => ({ postMessage: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));

// The editor looks its modal up at construction, so the page exists first.
document.body.innerHTML = `<div id="blend-editor-modal" style="display: none;"></div>`;

const { EffectGuids } = await import("../ts/effectGuids.js");
const { uiState } = await import("../ts/state.js");
const { BlendEditorModal } = await import("../ts/blendEditor.js");

function blendNode(): GraphNode {
  return { id: "blend-1", type: EffectGuids.kAmpNamBlend, displayName: "Blend", category: "amp", bypassed: false,
    params: { gain: 0.2 }, config: { blendId: "b" } };
}

function presetWith(node: GraphNode): Preset {
  return { id: "p1", name: "P1", graph: { nodes: [node], edges: [] } } as Preset;
}

describe("blend editor Test knobs", () => {
  it("move the node on the draft being edited, not the library's saved copy of the preset", () => {
    // The Test knobs write the node they resolve and send the engine the value. Resolved from
    // the preset cache, they wrote the saved copy: the params panel, drawn from the draft,
    // showed the old value, so did Save, and the cached preset was silently changed.
    const saved = presetWith(blendNode());
    const draft = presetWith(blendNode());
    uiState.activePresetId = "p1";
    uiState.presetCache = new Map([["p1", saved]]);
    uiState.activePresetDraft = draft;

    const editor = new BlendEditorModal({ getBlendLibrary: () => [], getResourceLibrary: () => uiState.resourceLibrary });
    document.getElementById("blend-editor-modal")!.dataset.nodeId = "blend-1";

    const node = (editor as unknown as { getActiveNode(): GraphNode | null }).getActiveNode();
    expect(node).toBe(draft.graph.nodes[0]);
  });
});
