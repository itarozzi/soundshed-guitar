import { beforeEach, describe, expect, it, vi } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import type { GraphNode } from "../ts/types.js";
import { bindGraphicEqControls } from "../ts/signalPath/paramsPanel/eq.js";
import { sendSignalPathNodeParamUpdate } from "../ts/signalPath/commands.js";

vi.mock("../ts/signalPath/state.js", () => ({
  get nodeParamsPanelElement() { return document.getElementById("node-params-panel"); },
}));
vi.mock("../ts/signalPath/commands.js", () => ({ sendSignalPathNodeParamUpdate: vi.fn() }));
vi.mock("../ts/signalPath/render.js", () => ({ requestNodeParamsPanel: vi.fn() }));
vi.mock("../ts/presetV2.js", () => ({
  EffectTypeRegistry: { resolve: (type: string) => type },
  getNodeEffectInfo: () => null,
}));

let node: GraphNode;
let input: HTMLInputElement;

beforeEach(() => {
  vi.clearAllMocks();
  document.body.innerHTML = `<div id="node-params-panel">
    <input type="number" class="graphic-eq-frequency" data-param-key="band2Freq" min="20" max="999" step="1" value="350">
    <input type="number" class="graphic-eq-frequency" data-param-key="band3Freq" min="351" max="3999" step="1" value="1000">
  </div>`;
  node = {
    id: "eq", type: EffectGuids.kEqGraphic, displayName: "EQ", category: "eq", bypassed: false,
    config: {}, params: { band1Freq: 100, band2Freq: 350, band3Freq: 1000, band4Freq: 4000 },
  };
  input = document.querySelector<HTMLInputElement>('[data-param-key="band3Freq"]')!;
  bindGraphicEqControls(node, { id: "preset", name: "Test" });
});

describe("graphic EQ frequency entry", () => {
  it("lets 8, 80 and 800 remain draft text until Enter commits", () => {
    for (const draft of ["8", "80", "800"]) {
      input.value = draft;
      input.dispatchEvent(new Event("input"));
      expect(input.value).toBe(draft);
      expect(node.params.band3Freq).toBe(1000);
      expect(sendSignalPathNodeParamUpdate).not.toHaveBeenCalled();
    }
    input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" }));
    expect(node.params.band3Freq).toBe(800);
    expect(sendSignalPathNodeParamUpdate).toHaveBeenCalledTimes(1);
    expect(sendSignalPathNodeParamUpdate).toHaveBeenCalledWith("eq", "band3Freq", 800);
    expect(document.querySelector<HTMLInputElement>('[data-param-key="band2Freq"]')!.max).toBe("799");
    expect(input.isConnected).toBe(true);
  });

  it("clamps only the committed entry to its neighbours", () => {
    input.value = "8";
    input.dispatchEvent(new Event("change"));
    expect(input.value).toBe("351");
    expect(node.params.band3Freq).toBe(351);
  });

  it("commits on blur, without sending twice after change", () => {
    input.value = "800";
    input.dispatchEvent(new Event("change"));
    input.dispatchEvent(new Event("blur"));
    expect(sendSignalPathNodeParamUpdate).toHaveBeenCalledTimes(1);
  });

  it("restores empty or invalid entries without changing the engine", () => {
    for (const draft of ["", "not a frequency"]) {
      input.value = draft;
      input.dispatchEvent(new Event("blur"));
      expect(input.value).toBe("1000");
    }
    expect(sendSignalPathNodeParamUpdate).not.toHaveBeenCalled();
  });

  it("cancels a draft with Escape", () => {
    input.value = "800";
    input.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
    expect(input.value).toBe("1000");
    expect(sendSignalPathNodeParamUpdate).not.toHaveBeenCalled();
  });

  it("keeps arrow increments at one Hz and commits without replacing the field", () => {
    input.stepDown();
    input.dispatchEvent(new Event("input"));
    input.dispatchEvent(new Event("change"));
    expect(node.params.band3Freq).toBe(999);
    expect(input.isConnected).toBe(true);
  });
});
