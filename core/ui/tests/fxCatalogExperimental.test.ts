/**
 * Which effects the FX library offers: the experimental ones only with Experimental Effects on,
 * Guitar to MIDI, which no longer is, always, and Pickup Match, which lives in the global chain
 * for now, and Verbal, which is off for now, never.
 */
import { beforeEach, describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids.js";
import { uiState } from "../ts/state.js";

const { EffectTypeRegistry } = await import("../ts/presetV2.js");
const { getCatalogEffects } = await import("../ts/fxSelector.js");
const { onEffectCatalog } = await import("../ts/messages/effectHandlers.js");

for (const [type, displayName, category] of [
  [EffectGuids.kGuitarToMidi, "Guitar to MIDI", "synth"],
  [EffectGuids.kTransposeStft, "Transpose (STFT)", "pitch"],
  [EffectGuids.kTransposeHybrid, "Transpose (Hybrid)", "pitch"],
]) {
  EffectTypeRegistry.register(type, { type, displayName, category, requiresResource: false, parameters: [] });
}

const offered = (): string[] => getCatalogEffects().map((effect) => EffectTypeRegistry.resolve(effect.type));

describe("FX library catalog", () => {
  beforeEach(() => {
    uiState.appSettings = {} as typeof uiState.appSettings;
  });

  it("offers Guitar to MIDI with Experimental Effects off, and hides the experimental transposes", () => {
    expect(offered()).toContain(EffectGuids.kGuitarToMidi);
    expect(offered()).not.toContain(EffectGuids.kTransposeStft);
    expect(offered()).not.toContain(EffectGuids.kTransposeHybrid);
  });

  it("never offers Pickup Match, even once the engine's catalogue has described it", () => {
    onEffectCatalog({ type: "effectCatalog", catalog: [
      { type: EffectGuids.kPickupMatch, name: "Pickup Match", category: "eq", parameters: [] },
    ] });
    expect(EffectTypeRegistry.get(EffectGuids.kPickupMatch)?.displayName).toBe("Pickup Match");
    expect(offered()).not.toContain(EffectGuids.kPickupMatch);
  });

  it("never offers Verbal, which is off for now", () => {
    onEffectCatalog({ type: "effectCatalog", catalog: [
      { type: EffectGuids.kReverbVerbal, name: "Verbal", category: "reverb", parameters: [] },
    ] });
    expect(EffectTypeRegistry.get(EffectGuids.kReverbVerbal)?.displayName).toBe("Verbal");
    expect(offered()).not.toContain(EffectGuids.kReverbVerbal);
  });

  it("adds the experimental transposes when the feature is on", () => {
    uiState.appSettings = { "features.experimentalEffects.enabled": true } as unknown as typeof uiState.appSettings;
    expect(offered()).toEqual(
      expect.arrayContaining([EffectGuids.kGuitarToMidi, EffectGuids.kTransposeStft, EffectGuids.kTransposeHybrid]),
    );
  });
});
