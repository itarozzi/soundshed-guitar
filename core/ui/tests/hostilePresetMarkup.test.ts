/**
 * A preset from an imported archive or a Tone Sharing pack is whatever someone chose to
 * share. Its node ids, fxChain labels, display names and setlist-visible names must render
 * as text: none of them may add an element or an attribute.
 */
import { beforeAll, describe, expect, it, vi } from "vitest";
import type { GraphNode, Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

// Module-level DOM roots are looked up at import, so the page exists first.
// (No underscore in the payload: the details view shows a stage's underscores as spaces.)
document.body.innerHTML = `
  <div id="preset-details"></div>
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
  <div id="setlist-list"></div>
  <div id="setlist-slots"></div>
  <div id="setlist-editor-header"></div>
`;
window.IPlugSendMsg = () => undefined;

const HOSTILE = `x"><img src=x onerror="window.pwned=1">`;

const { uiState } = await import("../ts/state.js");
const { renderPresetDetails } = await import("../ts/views.js");
const { renderSignalPathBar } = await import("../ts/signalPath.js");
const { renderSetlistPanel } = await import("../ts/presets/setlists.js");
const { showNodeParamsPanel } = await import("../ts/signalPath/paramsPanel/panel.js");
const { EffectTypeRegistry } = await import("../ts/presetV2.js");

// An amp that takes a model, so the panel draws its resource selector and a knob.
EffectTypeRegistry.register("test-amp", {
  type: "test-amp",
  displayName: "Test Amp",
  category: "amp",
  requiresResource: true,
  resourceType: "nam",
  parameters: [{ key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "" }],
});

function node(id: string, type: string, displayName = ""): GraphNode {
  return { id, type, displayName, category: "", bypassed: false, params: {}, config: {} };
}

function hostilePreset(): Preset {
  return {
    id: "user-hostile",
    name: HOSTILE,
    fxChain: [HOSTILE, `drive_${HOSTILE}`],
    attachments: [{ type: HOSTILE as never, hash: HOSTILE }],
    graph: {
      nodes: [node("__input__", "input"), node(HOSTILE, "gain", HOSTILE), node("__output__", "output")],
      edges: [
        { from: "__input__", to: HOSTILE, fromPort: 0, toPort: 0, gain: 1 },
        { from: HOSTILE, to: "__output__", fromPort: 0, toPort: 0, gain: 1 },
      ],
    },
  };
}

/** Nothing the preset carried became markup: not its image, and no event-handler attribute anywhere. */
function expectInert(root: Element): void {
  expect(root.querySelectorAll('img[src="x"]').length).toBe(0);
  const handlerAttributes = Array.from(root.querySelectorAll("*"))
    .flatMap((element) => Array.from(element.attributes))
    .filter((attribute) => attribute.name.startsWith("on"));
  expect(handlerAttributes).toEqual([]);
  expect((window as { pwned?: number }).pwned).toBeUndefined();
}

const hooks = {
  onApplyPreset: vi.fn(async () => undefined),
  onRequestSignalTest: vi.fn(),
  onBindLoadButtons: vi.fn(),
};

describe("a hostile preset renders as text", () => {
  beforeAll(() => {
    const preset = hostilePreset();
    uiState.presets = [preset];
    uiState.presetCache = new Map([[preset.id, preset]]);
    uiState.activePresetId = preset.id;
  });

  it("in the preset details: fxChain labels, attachment type and name", () => {
    const details = document.getElementById("preset-details")!;
    renderPresetDetails(hostilePreset(), hooks);

    expectInert(details);
    const labels = Array.from(details.querySelectorAll(".fx-node-label"), (label) => label.textContent);
    expect(labels).toContain(HOSTILE);
    expect(labels).toContain(`drive ${HOSTILE}`);
    expect(details.querySelector(".attachment-type")?.textContent).toBe(HOSTILE);
    expect(details.querySelector(".preset-title")?.textContent).toBe(HOSTILE);
  });

  it("in the details' chain when the stages come from the graph's display names", () => {
    const details = document.getElementById("preset-details")!;
    renderPresetDetails({ ...hostilePreset(), fxChain: [] }, hooks);

    expectInert(details);
    expect(Array.from(details.querySelectorAll(".fx-node-label"), (label) => label.textContent)).toContain(HOSTILE);
  });

  it("in the signal path: the node id round-trips through its data attribute", () => {
    const nodes = document.getElementById("signal-path-nodes")!;
    renderSignalPathBar();

    expectInert(nodes);
    const rendered = Array.from(nodes.querySelectorAll<HTMLElement>(".signal-node[data-node-id]"), (element) => element.dataset.nodeId);
    expect(rendered).toContain(HOSTILE);
  });

  it("in the node's parameter panel, with a file path on its resource", () => {
    const preset = hostilePreset();
    const amp = node(HOSTILE, "test-amp", HOSTILE);
    amp.resources = [{ resourceType: "nam", resourceId: HOSTILE, filePath: `C:\\${HOSTILE}.nam` }];
    preset.graph!.nodes.splice(1, 1, amp);
    const panel = document.getElementById("node-params-panel")!;
    showNodeParamsPanel(amp, preset);

    expectInert(panel);
    const ids = Array.from(panel.querySelectorAll<HTMLElement>("[data-node-id]"), (element) => element.dataset.nodeId);
    expect(ids.length).toBeGreaterThan(3);
    expect(ids.every((id) => id === HOSTILE)).toBe(true);
    expect(panel.querySelector(".resource-path-info")?.textContent).toBe(`C:\\${HOSTILE}.nam`);
  });

  it("in the setlist editor: setlist and preset names", () => {
    uiState.setlists = [{ id: `set-${HOSTILE}`, name: HOSTILE, bank: null, slots: [{ presetId: "user-hostile" }] }];
    uiState.activeSetlistId = `set-${HOSTILE}`;
    renderSetlistPanel();

    for (const id of ["setlist-list", "setlist-slots"]) {
      expectInert(document.getElementById(id)!);
    }
    expect(document.querySelector<HTMLElement>(".setlist-item")?.dataset.setlistId).toBe(`set-${HOSTILE}`);
    expect(document.querySelector(".setlist-item span")?.textContent).toBe(HOSTILE);
    expect(document.querySelector(".setlist-slot-title")?.textContent).toBe(HOSTILE);
  });
});
