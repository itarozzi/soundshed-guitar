// @vitest-environment node
import { describe, expect, it } from "vitest";
import { BuildRegistry } from "../ts/toneBuilder/build.js";
import { balancePresetSpec, correctionFor, stagingCaptureIndex } from "../ts/toneBuilder/levels.js";
import type { SceneMeasure } from "../ts/toneBuilder/levels.js";
import type { BuildInventory, ChainItemSpec, PresetSpec } from "../ts/toneBuilder/types.js";

const AMP_NAM = "49ea214c-91e6-41f9-bd27-ad6eec0ae90a";
const FX_NAM = "c3263344-65e4-4b7e-b102-ea625700e12f";

const inventory: BuildInventory = {
  effects: [
    { type: AMP_NAM, displayName: "Neural Amp (NAM)", category: "amp", requiresResource: true, resourceType: "nam", parameters: [{ key: "outputGain", min: -24, max: 24, default: 0 }], presets: [] },
    { type: FX_NAM, displayName: "Neural FX (NAM)", category: "amp", requiresResource: true, resourceType: "nam", parameters: [{ key: "outputGain", min: -24, max: 24, default: 0 }], presets: [] },
    { type: "delay", displayName: "Delay", category: "delay", parameters: [], presets: [] },
  ],
  resources: [{ id: "amp", type: "nam", name: "Amp" }, { id: "fx", type: "nam", name: "Fuzz" }],
};
const registry = new BuildRegistry(inventory);
const amp = { fx: "Neural Amp (NAM)", resource: { id: "amp" } };
const fuzz = { fx: "Neural FX (NAM)", resource: { id: "fx" } };
const collection = { idScope: "levels", category: "Tests" };

describe("where a correction goes", () => {
  it("prefers the last switched-on amp capture, then an FX capture, else the output trim", () => {
    expect(stagingCaptureIndex([fuzz, amp, { fx: "Delay" }], registry)).toBe(1);
    expect(stagingCaptureIndex([fuzz, { ...amp, off: true }], registry)).toBe(0);
    expect(stagingCaptureIndex([{ fx: "Delay" }], registry)).toBe(-1);
  });

  it("never uses a capture that feeds a split of captures", () => {
    const chain: ChainItemSpec[] = [fuzz, { split: [[amp], [amp]] }];
    expect(stagingCaptureIndex(chain, registry)).toBe(-1);
  });

  it("corrects in half-dB steps without lifting the peak over -1 dBFS", () => {
    expect(correctionFor(-21.2, -9)).toBe(3);
    expect(correctionFor(-15.3, -3)).toBe(-2.5);
    expect(correctionFor(-24, -2)).toBe(1);
  });
});

describe("balancePresetSpec", () => {
  /** A fake engine: loudness follows the capture's output gain and the scene trim one for one. */
  function linear(base: number, peakOffset = 10): SceneMeasure {
    return async (preset, sceneId) => {
      const scene = (preset.scenes as Array<{ id: string; graph: { nodes: Array<{ type: string; params: Record<string, number> }> } }>).find((s) => s.id === sceneId);
      const nodes = scene?.graph.nodes ?? [];
      const gain = nodes.filter((n) => n.type === AMP_NAM || n.type === FX_NAM).reduce((sum, n) => sum + (n.params.outputGain ?? 0), 0);
      const trim = nodes.find((n) => n.type === "output")?.params.gainDb ?? 0;
      const lufs = base + gain + trim;
      return { integratedLufs: lufs, peakDbfs: lufs + peakOffset };
    };
  }

  it("brings a hot capture to -18 LUFS on its Output Gain", async () => {
    const spec: PresetSpec = { key: "p", name: "P", description: "", chain: [amp, { fx: "Delay" }] };
    const { spec: balanced, report } = await balancePresetSpec(collection, spec, registry, linear(-9));
    expect(report[0]).toMatchObject({ balanced: true, correctionDb: -9 });
    expect((balanced.scenes?.[0].chain[0] as { set?: Record<string, number> }).set?.outputGain).toBe(-9);
  });

  it("puts what the capture cannot take, and scenes without one, on the output trim", async () => {
    const spec: PresetSpec = {
      key: "p", name: "P", description: "",
      scenes: [
        { key: "far", title: "Far", chain: [amp] },
        { key: "plain", title: "Plain", chain: [{ fx: "Delay" }] },
      ],
    };
    const { spec: balanced, report } = await balancePresetSpec(collection, spec, registry, linear(-48, -10));
    expect(report.every((scene) => scene.balanced)).toBe(true);
    expect((balanced.scenes?.[0].chain[0] as { set?: Record<string, number> }).set?.outputGain).toBe(24);
    expect(balanced.scenes?.[0].outputGainDb).toBe(6);
    expect(balanced.scenes?.[1].outputGainDb).toBe(30);
  });

  it("leaves a scene alone when it cannot be measured", async () => {
    const spec: PresetSpec = { key: "p", name: "P", description: "", chain: [amp] };
    const failing: SceneMeasure = async () => {
      throw new Error("missing model");
    };
    const { spec: unchanged, report } = await balancePresetSpec(collection, spec, registry, failing);
    expect(report[0]).toMatchObject({ balanced: false, note: "not measured: missing model" });
    expect((unchanged.scenes?.[0].chain[0] as { set?: unknown }).set).toBeUndefined();
  });
});
