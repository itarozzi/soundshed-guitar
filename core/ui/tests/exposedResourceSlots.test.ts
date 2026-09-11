import { describe, expect, it } from "vitest";
import { resolveExposedResourceSlot } from "../ts/exposedResourceSlots";

describe("resolveExposedResourceSlot", () => {
  it("gives a composite one slot per exposed resource, in declaration order", () => {
    // "Supercharged Neural Amp" surfaces an amp model and a cab IR, and both
    // declare resource slot 0 — of their own inner node.
    expect(resolveExposedResourceSlot("composite:supercharged-neural-amp", 0, 0)).toBe(0);
    expect(resolveExposedResourceSlot("composite:supercharged-neural-amp", 0, 1)).toBe(1);
  });

  it("keeps the slots an effect declares on itself", () => {
    // IR cab A/B.
    expect(resolveExposedResourceSlot("cab_ir", 0, 0)).toBe(0);
    expect(resolveExposedResourceSlot("cab_ir", 1, 1)).toBe(1);
    // A WASM module reserves slot 0 for the module, so its blobs start at 1.
    expect(resolveExposedResourceSlot("wasm_effect", 1, 0)).toBe(1);
    expect(resolveExposedResourceSlot("wasm_effect", 2, 1)).toBe(2);
  });

  it("falls back to the declaration order when no slot is declared", () => {
    expect(resolveExposedResourceSlot("cab_ir", undefined, 1)).toBe(1);
  });
});
