/**
 * A Custom Effect node's params and resources come from its module's descriptor, cached in the
 * node config (Custom Effect ABI 2: param.<id>.<field> and resource.<id>.<field>, in the order each
 * id first appears).
 */
import { describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import { EffectTypeRegistry, getNodeEffectInfo, WASM_GUEST_DESCRIPTOR_CONFIG_KEY } from "../ts/presetV2.js";

EffectTypeRegistry.register(EffectGuids.kWasmHost, {
  type: EffectGuids.kWasmHost,
  displayName: "Custom Effect",
  category: "utility",
  requiresResource: true,
  parameters: [
    { key: "mix", name: "Mix", default: 1, min: 0, max: 1 },
    { key: "inputGainDb", name: "Input Gain", default: 0, min: -24, max: 24 },
  ],
  exposedResources: [{ resourceId: "module", displayName: "Module", nodeId: "", resourceType: "wasm", resourceIndex: 0 }],
});

function nodeWithDescriptor(entries: Array<[string, string]>) {
  return {
    type: EffectGuids.kWasmHost,
    config: {
      [WASM_GUEST_DESCRIPTOR_CONFIG_KEY]: JSON.stringify(entries.map(([key, value]) => ({ key, value }))),
    },
  };
}

describe("Custom Effect node params", () => {
  it("lists params by id, in the order they are first declared, before the host's", () => {
    const info = getNodeEffectInfo(nodeWithDescriptor([
      ["effect.name", "Tremolo"],
      ["param.rate.title", "Rate"],
      ["param.depth.title", "Depth"],
      ["param.depth.default", "0.5"],
      ["param.rate.max", "10"],
    ]));

    expect(info?.displayName).toBe("Tremolo");
    expect(info?.parameters.map((param) => param.key)).toEqual(["rate", "depth", "mix", "inputGainDb"]);
    expect(info?.parameters[0]).toMatchObject({ name: "Rate", max: 10 });
    expect(info?.parameters[1]).toMatchObject({ name: "Depth", default: 0.5 });
  });

  it("ignores numbered ABI 1 keys and never lists a host control twice", () => {
    const info = getNodeEffectInfo(nodeWithDescriptor([
      ["param.0.id", "gain"],
      ["param.0.title", "Gain"],
      ["param.mix.title", "Guest Mix"],
    ]));

    expect(info?.parameters.map((param) => param.key)).toEqual(["mix", "inputGainDb"]);
    expect(info?.parameters[0].name).toBe("Mix");
  });
});

describe("Custom Effect node resources", () => {
  it("lists resources by id after the module, at places in declaration order", () => {
    const info = getNodeEffectInfo(nodeWithDescriptor([
      ["resource.table.title", "Wavetable"],
      ["resource.ir.title", "Impulse"],
      ["resource.ir.type", "ir"],
      ["resource.table.allowBrowseFile", "false"],
    ]));

    expect(info?.exposedResources?.map((resource) => [resource.resourceId, resource.resourceIndex])).toEqual([
      ["module", 0],
      ["table", 1],
      ["ir", 2],
    ]);
    expect(info?.exposedResources?.[1]).toMatchObject({ displayName: "Wavetable", resourceType: "blob", allowBrowseFile: false });
    expect(info?.exposedResources?.[2]).toMatchObject({ displayName: "Impulse", resourceType: "ir" });
  });

  it("ignores numbered ABI 1 resource blocks and the module's own id", () => {
    const info = getNodeEffectInfo(nodeWithDescriptor([
      ["resource.0.id", "impulse"],
      ["resource.0.slot", "1"],
      ["resource.module.title", "Another Module"],
    ]));

    expect(info?.exposedResources?.map((resource) => resource.resourceId)).toEqual(["module"]);
  });
});
