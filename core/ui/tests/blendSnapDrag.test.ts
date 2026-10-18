/**
 * A blend in snap mode: dragging one captured-parameter knob jumps every knob to the nearest
 * model's capture. The panel used to be rebuilt at the first mapping the drag reached, which
 * replaced the knob under the pointer and ended the drag there.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
type Sent = { type: string; nodeId?: string; paramKey?: string; value?: number };
let sent: Sent[] = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as Sent);
};

const { EffectGuids } = await import("../ts/effectGuids.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { uiState } = await import("../ts/state.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");

EffectTypeRegistry.register(EffectGuids.kAmpNamBlend, {
  type: EffectGuids.kAmpNamBlend,
  displayName: "Blend",
  category: "amp",
  requiresResource: false,
  parameters: [],
});

// Two models, captured at gain 2 / bass 2 and gain 8 / bass 9 on the knobs' 0..10 scale.
uiState.blendLibrary = [{
  id: "b", name: "B", category: "amp", models: [], blendMode: "snap", parameters: ["gain", "bass"],
  modelMappings: [
    { id: "quiet", parameters: { gain: 0.2, bass: 0.2 } },
    { id: "loud", parameters: { gain: 0.8, bass: 0.9 } },
  ],
}];

function node(): GraphNode {
  return { id: "bl1", type: EffectGuids.kAmpNamBlend, displayName: "Blend", category: "amp", bypassed: false,
    params: { gain: 0.2, bass: 0.2 }, config: { blendId: "b" } } as GraphNode;
}

function presetWith(n: GraphNode): Preset {
  return { id: "user-test", name: "Test", graph: { nodes: [n], edges: [] } } as Preset;
}

/** jsdom does not implement PointerEvent; the knob only reads the geometry and the id. */
function pointer(type: string, y: number): PointerEvent {
  const event = new MouseEvent(type, { bubbles: true, clientX: 50, clientY: y, button: 0 });
  Object.defineProperty(event, "pointerId", { value: 1 });
  return event as PointerEvent;
}

const knob = (key: string): HTMLElement => document.querySelector(`.node-param-knob[data-param-key='${key}']`) as HTMLElement;

describe("blend snap drag", () => {
  let current: GraphNode;

  beforeEach(() => {
    sent = [];
    current = node();
    showNodeParamsPanel(current, presetWith(current));
  });

  it("moves the other knobs in place and keeps the knob being dragged", () => {
    const gain = knob("gain");
    const bass = knob("bass");
    // The gain knob runs 2..8 over 200 px; 150 px up is 6.5, nearer the loud model's 8.
    gain.dispatchEvent(pointer("pointerdown", 200));
    gain.dispatchEvent(pointer("pointermove", 50));

    expect(current.params).toEqual({ gain: 0.8, bass: 0.9 });
    expect(sent.filter((m) => m.type === "updateSignalPathNodeParam").map((m) => [m.paramKey, m.value]))
      .toEqual([["gain", 0.8], ["bass", 0.9]]);
    // The same elements, still in the page: the drag goes on.
    expect(knob("gain")).toBe(gain);
    expect(knob("bass")).toBe(bass);
    expect(gain.dataset.value).toBe("8");
    expect(bass.dataset.value).toBe("9");
  });

  it("shows the capture the knob snapped to when it is the one already playing", () => {
    const gain = knob("gain");
    // 50 px up is 3.5: still nearest the quiet model's 2, which the node already holds.
    gain.dispatchEvent(pointer("pointerdown", 200));
    gain.dispatchEvent(pointer("pointermove", 150));

    expect(current.params).toEqual({ gain: 0.2, bass: 0.2 });
    expect(sent.filter((m) => m.type === "updateSignalPathNodeParam")).toEqual([]);
    expect(gain.dataset.value).toBe("2");
  });
});
