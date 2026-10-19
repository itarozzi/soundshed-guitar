/**
 * Parameter changes from MIDI or host automation: shown on the open panel's knob without a
 * rebuild, rebuilt at most once per interval when no knob can show them, and ignored for a
 * node whose controls are not on screen.
 */
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;
HTMLCanvasElement.prototype.getContext = (() => null) as typeof HTMLCanvasElement.prototype.getContext;
window.IPlugSendMsg = () => {};

const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const { showEngineNodeParamChange } = await import("../ts/signalPath/engineParamChange.js");
const { setNodeParamsRefresher } = await import("../ts/signalPath/render.js");
const { setSelectedNodeId } = await import("../ts/signalPath/state.js");

const TEST_TYPE = "test-automation-fx";

beforeAll(() => {
  EffectTypeRegistry.register(TEST_TYPE, {
    type: TEST_TYPE,
    displayName: "Test Drive",
    category: "drive",
    requiresResource: false,
    parameters: [
      { key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "amount", group: "Main" },
      { key: "boost", name: "Boost", default: 0, min: 0, max: 1, unit: "toggle", group: "Main" },
    ],
  });
});

function node(id: string): GraphNode {
  return { id, type: TEST_TYPE, displayName: "Test Drive", category: "drive", bypassed: false, params: { gain: 0.5, boost: 0 }, config: {} } as GraphNode;
}

function knobFor(key: string): HTMLElement {
  return document.querySelector<HTMLElement>(`#node-params-panel .node-param-knob[data-param-key="${key}"]`)!;
}

/** Applies a change as the message handler does: params first, then the panel. */
function engineSets(target: GraphNode, key: string, value: number): void {
  const changed = target.params[key] !== value;
  target.params[key] = value;
  showEngineNodeParamChange(target, key, value, changed);
}

describe("showEngineNodeParamChange", () => {
  let selected: GraphNode;
  let rebuilds: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    vi.useFakeTimers();
    selected = node("drive-1");
    const preset = { id: "user-test", name: "Test", graph: { nodes: [selected, node("drive-2")], edges: [] } } as unknown as Preset;
    setSelectedNodeId(selected.id);
    showNodeParamsPanel(selected, preset);
    rebuilds = vi.fn();
    setNodeParamsRefresher(rebuilds);
  });

  afterEach(() => {
    vi.runAllTimers();
    vi.useRealTimers();
  });

  it("moves the knob in place rather than rebuilding the panel", () => {
    const knob = knobFor("gain");

    engineSets(selected, "gain", 0.8);
    vi.advanceTimersByTime(500);

    expect(knob.isConnected).toBe(true);
    expect(knob.dataset.value).toBe("0.8");
    expect(rebuilds).not.toHaveBeenCalled();
  });

  it("rebuilds once for a burst of changes no knob can show", () => {
    expect(knobFor("boost")).toBeNull();

    engineSets(selected, "boost", 1);
    engineSets(selected, "boost", 0);
    engineSets(selected, "boost", 1);
    expect(rebuilds).not.toHaveBeenCalled();

    vi.advanceTimersByTime(100);
    expect(rebuilds).toHaveBeenCalledTimes(1);
  });

  it("draws nothing for a node that is not selected", () => {
    const other = node("drive-2");
    const knob = knobFor("gain");

    engineSets(other, "gain", 0.1);
    engineSets(other, "boost", 1);
    vi.advanceTimersByTime(500);

    expect(knob.dataset.value).toBe("0.5");
    expect(rebuilds).not.toHaveBeenCalled();
  });

  it("draws nothing when the engine echoes the value the panel already has", () => {
    engineSets(selected, "boost", 0);
    vi.advanceTimersByTime(500);

    expect(rebuilds).not.toHaveBeenCalled();
  });
});
