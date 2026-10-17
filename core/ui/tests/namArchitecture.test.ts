import { beforeEach, describe, expect, it, vi } from "vitest";
import type { LibraryResource } from "../ts/types.js";

const resourceData = new Map<string, string>();
vi.mock("../ts/archiveUtils.js", () => ({
  requestResourceData: vi.fn(async (_type: string, id: string) => resourceData.get(id)),
}));
vi.mock("../ts/signalPath/render.js", () => ({ requestSignalPathRender: vi.fn() }));

const sent: Array<{ type: string }> = [];
window.IPlugSendMsg = (payload: string) => {
  sent.push(JSON.parse(payload) as { type: string });
};

const {
  classifyNamModelArchitecture,
  inferNamArchitectureBadgeFromData,
  inferredNamArchitectureByResourceId,
  requestNamArchitectureInference,
} = await import("../ts/signalPath/nodeLabels.js");

const a1Layers = [{ input_size: 1, channels: 16, kernel_size: 3, dilations: [1, 2, 4], activation: "Tanh", gated: false }];
const a2Layers = [{
  input_size: 1,
  channels: 8,
  kernel_sizes: [3, 3, 3],
  dilations: [1, 2, 4],
  activation: ["Tanh", "Tanh", "Tanh"],
  head1x1: { active: false, out_channels: 1, groups: 1 },
  layer1x1: { active: true, groups: 1 },
}];

/** A nested submodel: a whole WaveNet with an "architecture" of its own. */
const submodel = (maxValue: number) => ({
  max_value: maxValue,
  model: {
    version: "0.7.0",
    metadata: { name: "inner", input_level_dbu: -99 },
    architecture: "WaveNet",
    config: { layers: a1Layers, head: null },
    weights: [0.1, 0.2],
    sample_rate: 48000,
  },
});

/** A model whose members appear in `order`, as different trainers write them. */
function model(order: string[], architecture: string, config: unknown): string {
  const parts: Record<string, unknown> = {
    version: "0.7.0",
    // A string naming the other architecture, ahead of the real member in some orders.
    metadata: { name: "Says \"architecture\": \"WaveNet\" in its name", input_level_dbu: 18 },
    architecture,
    config,
    weights: Array.from({ length: 50 }, (_, i) => i / 100),
    sample_rate: 48000,
  };
  return JSON.stringify(Object.fromEntries(order.map((key) => [key, parts[key]])));
}

const toBase64 = (text: string) => Buffer.from(text, "utf8").toString("base64");

const keyOrders = [
  ["version", "metadata", "architecture", "config", "weights", "sample_rate"],
  ["version", "architecture", "config", "weights", "sample_rate", "metadata"],
  ["config", "version", "weights", "metadata", "sample_rate", "architecture"],
];

describe("the A1/A2 badge read from a model file", () => {
  it.each(keyOrders)("reads a SlimmableContainer as A2 with its members in order %s,%s,%s,%s,%s,%s", (...order) => {
    const text = model(order, "SlimmableContainer", { submodels: [submodel(0.5), submodel(1)] });
    expect(inferNamArchitectureBadgeFromData(toBase64(text))).toBe("A2");
  });

  it.each(keyOrders)("tells an unwrapped A2 WaveNet from an A1 one with its members in order %s,%s,%s,%s,%s,%s", (...order) => {
    expect(inferNamArchitectureBadgeFromData(toBase64(model(order, "WaveNet", { layers: a2Layers })))).toBe("A2");
    expect(inferNamArchitectureBadgeFromData(toBase64(model(order, "WaveNet", { layers: a1Layers })))).toBe("A1");
  });

  it("has no badge for other architectures or for what is not a model", () => {
    expect(classifyNamModelArchitecture({ architecture: "LSTM", config: { num_layers: 1 } })).toBe("");
    expect(classifyNamModelArchitecture({ patchList: [], version: "1.0.0" })).toBe("");
    expect(classifyNamModelArchitecture([{ architecture: "SlimmableContainer" }])).toBe("");
    expect(inferNamArchitectureBadgeFromData(toBase64("{\"architecture\": \"SlimmableContainer\""))).toBe("");
  });
});

describe("the badge worked out for a library row without one", () => {
  beforeEach(() => {
    sent.length = 0;
    resourceData.clear();
    inferredNamArchitectureByResourceId.clear();
  });

  it("is kept for the session and never written back over the engine's", async () => {
    const order = keyOrders[2];
    resourceData.set("m1", toBase64(model(order, "SlimmableContainer", { submodels: [submodel(1)] })));
    const resource: LibraryResource = { id: "m1", name: "Model", category: "amp", metadata: { provider: "local" } } as LibraryResource;

    requestNamArchitectureInference(resource);
    await vi.waitFor(() => expect(inferredNamArchitectureByResourceId.get("m1")).toBe("A2"));

    expect(resource.metadata).toEqual({ provider: "local" });
    expect(sent.filter((message) => message.type === "updateLibraryResource")).toEqual([]);
  });
});
