/**
 * Re-rendering the chain keeps the selected node's panel as it is. A state update that
 * swaps that node's model must still redraw it: a Tone3000 pick that lands after its
 * preview otherwise left the preview's temp file on screen while the engine played the
 * imported model.
 */
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset, ResourceRef } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
window.IPlugSendMsg = () => undefined;

const { uiState, setActivePresetDraft } = await import("../ts/state.js");
const { renderSignalPathBar } = await import("../ts/signalPath.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const { setSelectedNodeId } = await import("../ts/signalPath/state.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");

EffectTypeRegistry.register("test-amp", {
  type: "test-amp",
  displayName: "Test Amp",
  category: "amp",
  requiresResource: true,
  resourceType: "nam",
  parameters: [{ key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "" }],
});

const TEMP = "C:/Users/me/AppData/Roaming/Soundshed Guitar/data/v1/temp/preview_123.nam";

function node(id: string, type: string, resources?: ResourceRef[]): GraphNode {
  return { id, type, displayName: "", category: "", bypassed: false, params: {}, config: {}, resources };
}

function presetWithAmp(resources: ResourceRef[]): Preset {
  return {
    id: "user-amp",
    name: "Amp",
    graph: {
      nodes: [node("__input__", "input"), node("amp", "test-amp", resources), node("__output__", "output")],
      edges: [
        { from: "__input__", to: "amp", fromPort: 0, toPort: 0, gain: 1 },
        { from: "amp", to: "__output__", fromPort: 0, toPort: 0, gain: 1 },
      ],
    },
  };
}

const panel = () => document.getElementById("node-params-panel")!;

/// The chain as an engine state leaves it: the draft replaced, then the bar redrawn.
function receiveState(preset: Preset): void {
  setActivePresetDraft(preset);
  renderSignalPathBar();
}

describe("the selected node's panel after a state update", () => {
  beforeAll(() => {
    uiState.resourceLibrary = { nam: [{ id: "tone3000:102", name: "Crunch", filePath: "C:/Models/crunch.nam" }], ir: [] };
    uiState.activePresetId = "user-amp";
  });

  beforeEach(() => {
    // The user is on the amp while its preview plays.
    const previewing = presetWithAmp([{ resourceType: "nam", filePath: TEMP }]);
    receiveState(previewing);
    setSelectedNodeId("amp");
    showNodeParamsPanel(previewing.graph!.nodes[1], previewing);
    expect(panel().querySelector(".resource-path-info")?.textContent).toBe(TEMP);
  });

  it("is redrawn when the update swaps the node's model", () => {
    receiveState(presetWithAmp([{ resourceType: "nam", resourceId: "tone3000:102" }]));

    expect(panel().querySelector(".resource-path-info")).toBeNull();
    expect(panel().querySelector(".resource-picker-label-text")?.textContent).toBe("Crunch");
  });

  it("is left alone when only a blend value moved, as one does mid-drag", () => {
    const knob = panel().querySelector("[data-param-key], .node-param-knob");
    expect(knob).not.toBeNull();
    receiveState(presetWithAmp([{ resourceType: "nam", filePath: TEMP, parameterValue: 0.7 }]));

    expect(panel().querySelector("[data-param-key], .node-param-knob")).toBe(knob);
  });
});
