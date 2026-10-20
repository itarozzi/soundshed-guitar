import { afterEach, describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import { Features, getFeatureSettingKey } from "../ts/featureFlags.js";
import { getBlendState, renderBlendInfoHtml, updateBlendMatchSummary } from "../ts/signalPathBlend.js";
import { uiState } from "../ts/state.js";
import type { BlendDefinition, GraphNode } from "../ts/types.js";

const blend: BlendDefinition = {
  id: "b1",
  name: "Marshall JCM 800 Mod",
  category: "amp",
  models: ["m-low", "m-high"],
  blendMode: "snap",
  modelMappings: [
    { id: "m-low", parameterId: "gain", parameterValue: 0.2, parameters: { gain: 0.2 } },
    { id: "m-high", parameterId: "gain", parameterValue: 0.8, parameters: { gain: 0.8 } },
  ],
};

function makeNode(blendId = "b1", gain = 0.2): GraphNode {
  return { id: "n1", type: EffectGuids.kAmpNamBlend, params: { gain }, config: { blendId } } as unknown as GraphNode;
}

function render(node: GraphNode): HTMLElement {
  const panel = document.createElement("div");
  panel.innerHTML = renderBlendInfoHtml(node, getBlendState(node)!);
  return panel;
}

describe("the blend info block in the effect panel", () => {
  const originalBlends = uiState.blendLibrary;
  const originalResources = uiState.resourceLibrary;
  const originalSettings = uiState.appSettings;

  afterEach(() => {
    uiState.blendLibrary = originalBlends;
    uiState.resourceLibrary = originalResources;
    uiState.appSettings = originalSettings;
  });

  function useLibrary(blendTools: boolean): void {
    uiState.blendLibrary = [blend];
    uiState.resourceLibrary = {
      ...uiState.resourceLibrary,
      nam: [{ id: "m-low", name: "JCM G2" }, { id: "m-high", name: "JCM G8" }],
    } as typeof uiState.resourceLibrary;
    uiState.appSettings = { ...uiState.appSettings, [getFeatureSettingKey(Features.BlendTools)]: blendTools };
  }

  it("is the name with a pencil and the mode on one row, and the playing model under it", () => {
    useLibrary(true);
    const panel = render(makeNode());
    const row = panel.querySelector(".blend-info-row");
    expect(row?.querySelector(".blend-info-name")?.textContent).toBe("Marshall JCM 800 Mod");
    expect(row?.querySelector(".blend-info-name")?.getAttribute("title")).toBe("Marshall JCM 800 Mod · 2 models");
    expect(row?.querySelector(".blend-info-edit-btn")).not.toBeNull();
    expect(row?.querySelector<HTMLSelectElement>(".blend-mode-select")?.options[0].textContent).toBe("Default (Snap)");
    expect(panel.querySelector(".blend-match-line")?.textContent).toBe("Playing: JCM G2");
    expect(panel.textContent).not.toContain("Models:");
  });

  it("leaves the pencil out without Blend Tools", () => {
    useLibrary(false);
    expect(render(makeNode()).querySelector(".blend-info-edit-btn")).toBeNull();
  });

  it("follows the knob in place", () => {
    useLibrary(true);
    const node = makeNode();
    const panel = render(node);
    node.params.gain = 0.8;
    updateBlendMatchSummary(panel, node, getBlendState(node)!);
    const line = panel.querySelector<HTMLElement>(".blend-match-line");
    expect(line?.textContent).toBe("Playing: JCM G8");
    expect(line?.title).toBe("Playing: JCM G8");
  });

  it("says when the blend is missing, and what the node does meanwhile", () => {
    useLibrary(true);
    const node = makeNode("gone");
    const panel = render(node);
    expect(panel.querySelector(".blend-info-block")?.classList.contains("is-missing")).toBe(true);
    expect(panel.querySelector(".blend-info-name")?.textContent).toBe("Blend not found");
    expect(panel.querySelector(".blend-match-line")?.textContent).toContain("plays its input dry");
    expect(panel.querySelector(".blend-info-edit-btn")).toBeNull();
    // A knob turned on a missing blend leaves that explanation alone.
    updateBlendMatchSummary(panel, node, getBlendState(node)!);
    expect(panel.querySelector(".blend-match-line")?.textContent).toContain("plays its input dry");
  });
});
