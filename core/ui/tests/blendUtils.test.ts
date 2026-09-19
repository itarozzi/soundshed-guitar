import { describe, expect, it } from "vitest";
import {
  buildBlendModelMappingsFromNames,
  collectBlendModelIds,
  denormalizeBlendValue,
  describeBlendMix,
  findBlendForToneGroup,
  getBlendParamSpec,
  mappedBlendParamIds,
  normalizeBlendValue,
  selectBlendMix,
} from "../ts/blendUtils.js";
import type { BlendDefinition, BlendModelMapping } from "../ts/types.js";

// The engine's rule (MultiModelNAMAmpEffect) these mirror: the two models nearest the knob
// values by squared distance, weighted by inverse distance, only over parameters some model
// was captured at; with none of those set, the Blend sweep across the models' positions.

const quiet: BlendModelMapping = { id: "quiet", parameterId: "gain", parameterValue: 0, parameters: { gain: 0 } };
const loud: BlendModelMapping = { id: "loud", parameterId: "gain", parameterValue: 1, parameters: { gain: 1 } };

function blend(overrides: Partial<BlendDefinition>): BlendDefinition {
  return { id: "b", name: "B", category: "amp", models: [], ...overrides };
}

describe("selectBlendMix", () => {
  it("weights the two nearest models by inverse distance", () => {
    const mix = selectBlendMix([quiet, loud], { gain: 0.3 }, 0, "interpolate");
    // distances 0.09 and 0.49
    const w1 = 1 / 0.09;
    const w2 = 1 / 0.49;
    expect(mix?.primary).toBe(0);
    expect(mix?.secondary).toBe(1);
    expect(mix?.primaryWeight).toBeCloseTo(w1 / (w1 + w2), 6);
  });

  it("plays only the nearest in snap mode", () => {
    const mix = selectBlendMix([quiet, loud], { gain: 0.7 }, 0, "snap");
    expect(mix).toEqual({ primary: 1, primaryWeight: 1, secondary: null, secondaryWeight: 0 });
  });

  it("leaves out a model with too small a share, as the engine does", () => {
    const mix = selectBlendMix([quiet, loud], { gain: 1 }, 0, "interpolate");
    expect(mix?.primary).toBe(1);
    expect(mix?.secondary).toBeNull();
  });

  it("ignores a value for a parameter no model was captured at", () => {
    const withStale = selectBlendMix([quiet, loud], { gain: 0.3, bass: 0.9 }, 0, "interpolate");
    const without = selectBlendMix([quiet, loud], { gain: 0.3 }, 0, "interpolate");
    expect(withStale).toEqual(without);
  });

  it("sweeps the models in list order when nothing is captured", () => {
    const listed: BlendModelMapping[] = [{ id: "a" }, { id: "b" }, { id: "c" }];
    expect(selectBlendMix(listed, {}, 0, "interpolate")?.primary).toBe(0);
    expect(selectBlendMix(listed, {}, 1, "interpolate")?.primary).toBe(2);
    const middle = selectBlendMix(listed, {}, 0.25, "interpolate");
    expect(middle?.primary).toBe(0);
    expect(middle?.secondary).toBe(1);
    expect(middle?.secondaryWeight).toBeCloseTo(0.5, 6);
  });

  it("uses captured values as sweep positions when no knob is set", () => {
    const mix = selectBlendMix([loud, quiet], {}, 0, "interpolate");
    expect(mix?.primary).toBe(1);
  });

  it("plays a lone model whatever the knobs say", () => {
    expect(selectBlendMix([quiet], { gain: 1 }, 1, "interpolate")?.primary).toBe(0);
    expect(selectBlendMix([], {}, 0, "interpolate")).toBeNull();
  });
});

describe("describeBlendMix", () => {
  it("names the models and their shares", () => {
    const summary = describeBlendMix([quiet, loud], { gain: 0.5 }, 0, "interpolate", (id) => id.toUpperCase());
    expect(summary).toEqual({ name: "QUIET", details: "Mix: QUIET 50% / LOUD 50%" });
  });
});

describe("mappedBlendParamIds", () => {
  it("keeps the parameters some model is captured at, in the definition's order", () => {
    const mappings: BlendModelMapping[] = [
      { id: "a", parameters: { bass: 0.2, gain: 0.1 } },
      { id: "b", parameters: { gain: 0.9 } },
    ];
    expect(mappedBlendParamIds(blend({ parameters: ["gain", "treble", "bass"] }), mappings)).toEqual(["gain", "bass"]);
  });

  it("is empty when nothing is captured, so the Blend sweep is shown", () => {
    expect(mappedBlendParamIds(blend({ parameters: ["gain"] }), [{ id: "a", parameterId: "gain" }])).toEqual([]);
  });
});

describe("findBlendForToneGroup", () => {
  const library = [
    blend({ id: "x", toneGroupId: "tone-1", models: ["m1", "m2"] }),
    blend({ id: "y", toneGroupId: "tone-1", modelMappings: [{ id: "m1" }, { id: "m2" }, { id: "m3" }] }),
  ];

  it("finds the blend made from the same group and models, in any order", () => {
    expect(findBlendForToneGroup(library, "tone-1", ["m3", "m1", "m2"])?.id).toBe("y");
    expect(findBlendForToneGroup(library, "tone-1", ["m2", "m1"])?.id).toBe("x");
  });

  it("finds none for other models or another group", () => {
    expect(findBlendForToneGroup(library, "tone-1", ["m1"])).toBeUndefined();
    expect(findBlendForToneGroup(library, "tone-2", ["m1", "m2"])).toBeUndefined();
    expect(findBlendForToneGroup(library, "", ["m1", "m2"])).toBeUndefined();
  });
});

describe("mappings and scales", () => {
  it("collects every model a definition names, once", () => {
    expect(collectBlendModelIds(blend({ models: ["b", "c"], modelMappings: [{ id: "a" }, { id: "b" }] }))).toEqual([
      "a",
      "b",
      "c",
    ]);
  });

  it("reads captured settings from model names", () => {
    const [mapping] = buildBlendModelMappingsFromNames([{ id: "m", name: "Plexi G7 bright", category: "amp" }]);
    expect(mapping).toEqual({ id: "m", parameterId: "gain", parameterValue: 0.7, parameters: { gain: 0.7 } });
  });

  it("round-trips a knob value through the engine's 0..1", () => {
    const spec = getBlendParamSpec("gain");
    expect(normalizeBlendValue(7.5, spec)).toBeCloseTo(0.75, 9);
    expect(denormalizeBlendValue(0.75, spec)).toBeCloseTo(7.5, 9);
  });
});
