import { describe, expect, it } from "vitest";

import { sortFactoryEffectPresets } from "../ts/signalPath/effectPresets.js";

const names = (entries: { name: string }[]): string[] => entries.map((entry) => entry.name);

describe("factory effect preset order", () => {
  it("is alphanumeric, numbers by value and case ignored", () => {
    const sorted = sortFactoryEffectPresets([
      { name: "Twin 2x12" },
      { name: "closed 4x12" },
      { name: "AC30 2x12 Blue" },
      { name: "Band 10" },
      { name: "1960 4x12 V30" },
      { name: "Band 2" },
      { name: "Wide 2x12" },
    ]);
    expect(names(sorted)).toEqual([
      "1960 4x12 V30",
      "AC30 2x12 Blue",
      "Band 2",
      "Band 10",
      "closed 4x12",
      "Twin 2x12",
      "Wide 2x12",
    ]);
  });

  it("keeps the order of equal names and leaves its input alone", () => {
    const input = [
      { name: "Crunch", kind: "factory" },
      { name: "Clean", kind: "factory" },
      { name: "Crunch", kind: "pack" },
    ];
    const sorted = sortFactoryEffectPresets(input);
    expect(sorted.map((entry) => entry.kind)).toEqual(["factory", "factory", "pack"]);
    expect(names(sorted)).toEqual(["Clean", "Crunch", "Crunch"]);
    expect(names(input)).toEqual(["Crunch", "Clean", "Crunch"]);
  });
});
