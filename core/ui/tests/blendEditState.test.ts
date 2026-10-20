import { describe, expect, it } from "vitest";
import {
  addModels,
  blendWarnings,
  definitionFromEditState,
  editStateFromDefinition,
  groupByVoicing,
  matchLevels,
  readNamLoudness,
  removeParam,
  setAxis,
  type BlendEditState,
} from "../ts/blendEditor/editState.js";
import type { BlendDefinition, LibraryResource, ResourceLibrary } from "../ts/types.js";

function model(id: string, name: string, metadata: Record<string, string> = {}, category = "amp"): LibraryResource {
  return { id, name, category, metadata } as unknown as LibraryResource;
}

function libraryOf(...models: LibraryResource[]): ResourceLibrary {
  return { nam: models } as unknown as ResourceLibrary;
}

function emptyState(): BlendEditState {
  return { id: "b1", name: "Blend", category: "amp", mode: "interpolate", axis: "sweep", params: [], rows: [] };
}

const pack = { toneId: "t1", toneTitle: "Pack One", gear: "amp" };
const jcm = libraryOf(
  model("g8", "JCM - P5 B5 M5 T5 G8", pack),
  model("g2", "JCM - P5 B5 M5 T5 G2", pack),
  model("g5", "JCM - P5 B5 M5 T5 G5", pack),
);

describe("blend edit state", () => {
  it("adds models placed by the parameters their names vary on, sorted, and skips duplicates", () => {
    const state = emptyState();
    expect(addModels(state, ["g8", "g2", "g5", "g2"], jcm)).toBe(3);
    // P, B, M and T are the same on every model, so they make no knob.
    expect(state.params).toEqual(["gain"]);
    expect(state.axis).toBe("params");
    expect(state.rows.map((row) => row.id)).toEqual(["g2", "g5", "g8"]);
    expect(state.rows.map((row) => row.values.gain)).toEqual([0.2, 0.5, 0.8]);
    expect(addModels(state, ["g5"], jcm)).toBe(0);
  });

  it("sweeps models whose names say nothing, in the order they were added", () => {
    const library = libraryOf(model("a", "Clean Amp"), model("b", "Crunch Amp"));
    const state = emptyState();
    addModels(state, ["b", "a"], library);
    expect(state.axis).toBe("sweep");
    const definition = definitionFromEditState(state, library);
    expect(definition.parameters).toEqual([]);
    expect(definition.modelMappings).toEqual([{ id: "b" }, { id: "a" }]);
  });

  it("reads T as Tone on a pedal and Treble on an amp", () => {
    const pedals = libraryOf(model("p1", "OD G2 T5", { gear: "pedal" }, "pedal"), model("p2", "OD G2 T8", { gear: "pedal" }, "pedal"));
    const state = emptyState();
    addModels(state, ["p1", "p2"], pedals);
    expect(state.params).toEqual(["tone"]);
  });

  it("saves the primary parameter both ways, a trim only when set, and the pack only when shared", () => {
    const state = emptyState();
    addModels(state, ["g2", "g8"], jcm);
    state.rows[1].levelTrimDb = -3;
    const definition = definitionFromEditState(state, jcm);
    expect(definition.modelMappings?.[0]).toEqual({ id: "g2", parameterId: "gain", parameterValue: 0.2, parameters: { gain: 0.2 } });
    expect(definition.modelMappings?.[1].levelTrimDb).toBe(-3);
    expect(definition.toneGroupId).toBe("t1");
    expect(definition.category).toBe("amp");

    const mixed = libraryOf(...(jcm.nam ?? []), model("x", "Other G4", { toneId: "t2", toneTitle: "Pack Two", gear: "amp" }));
    addModels(state, ["x"], mixed);
    expect(definitionFromEditState(state, mixed).toneGroupId).toBeUndefined();
  });

  it("round-trips a saved definition, keeping its parameters and trims", () => {
    const saved: BlendDefinition = {
      id: "b9", name: "Saved", category: "amp", blendMode: "snap", parameters: ["gain"], models: ["g2", "g8"],
      modelMappings: [
        { id: "g2", parameterId: "gain", parameterValue: 0.2, parameters: { gain: 0.2 } },
        { id: "g8", parameterId: "gain", parameterValue: 0.8, parameters: { gain: 0.8 }, levelTrimDb: 2.5 },
      ],
    };
    const state = editStateFromDefinition(saved, jcm);
    expect(state.mode).toBe("snap");
    expect(state.params).toEqual(["gain"]);
    expect(definitionFromEditState(state, jcm).modelMappings).toEqual(saved.modelMappings);
  });

  it("keeps values through a switch to Sweep and back, and sweeps once the last parameter goes", () => {
    const state = emptyState();
    addModels(state, ["g2", "g8"], jcm);
    setAxis(state, "sweep", jcm);
    expect(definitionFromEditState(state, jcm).modelMappings?.[0].parameters).toBeUndefined();
    setAxis(state, "params", jcm);
    expect(state.rows[0].values.gain).toBe(0.2);
    removeParam(state, "gain");
    expect(state.axis).toBe("sweep");
  });

  it("warns about too few models, blank values, and models sharing a position", () => {
    const state = emptyState();
    expect(blendWarnings(state, jcm).map((w) => w.text)).toContain("A blend needs at least two models.");
    addModels(state, ["g2", "g8"], jcm);
    expect(blendWarnings(state, jcm)).toEqual([]);
    delete state.rows[0].values.gain;
    expect(blendWarnings(state, jcm).map((w) => w.text)).toContain("1 model has no Gain value.");
    state.rows[0].values.gain = 0.8;
    expect(blendWarnings(state, jcm).map((w) => w.text)[0]).toMatch(/^2 models share Gain 8/);
  });

  it("offers to split by voicing when voicings collide, labelled by what sets them apart", () => {
    const library = libraryOf(
      model("a0", "Bass Pre - Pack - Alpha-BITE G0"),
      model("a5", "Bass Pre - Pack - Alpha-BITE G5"),
      model("b0", "Bass Pre - Pack - Alpha-Bite-Growl G0"),
      model("b5", "Bass Pre - Pack - Alpha-Bite-Growl G5"),
    );
    const state = emptyState();
    addModels(state, ["a0", "a5", "b0", "b5"], library);
    const warning = blendWarnings(state, library).find((w) => w.action === "split-voicings");
    expect(warning?.text).toMatch(/^2 models share Gain 0, and 1 more position/);
    const voicings = groupByVoicing(state, library);
    expect(voicings.map((v) => v.label)).toEqual(["Alpha-BITE", "Alpha-Bite-Growl"]);
    expect(voicings.map((v) => v.rows.map((row) => row.id))).toEqual([["a0", "a5"], ["b0", "b5"]]);
  });

  it("matches levels to the median loudness, to half a dB, leaving unknown models alone", () => {
    const state = emptyState();
    addModels(state, ["g2", "g5", "g8"], jcm);
    state.rows[2].levelTrimDb = 1;
    const matched = matchLevels(state, new Map([["g2", -10], ["g5", -14.2]]));
    expect(matched).toBe(2);
    expect(state.rows.map((row) => row.levelTrimDb)).toEqual([-2, 2, 1]);
    expect(readNamLoudness(JSON.stringify({ metadata: { loudness: -18.5 } }))).toBe(-18.5);
    expect(readNamLoudness("not json")).toBeNull();
  });
});
