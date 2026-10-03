import { beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));

import { showNotification } from "../ts/notifications.js";
import { uiState } from "../ts/state.js";
import { EffectGuids } from "../ts/effectGuids.js";
import { collectEffectPresetsForExport, finishEffectPresetsForArchive } from "../ts/presets/effectPresetExport.js";
import type { BlendDefinition, StoredEffectPreset } from "../ts/types.js";

const amp: StoredEffectPreset = {
  id: "efp-amp",
  name: "Crunch",
  parameters: { inputGain: 3 },
  resources: [{ resourceType: "nam", resourceId: "lib-model", filePath: "C:/models/crunch.nam" }],
};
const blend: StoredEffectPreset = {
  id: "efp-blend",
  name: "Two Amps",
  parameters: { mix: 1 },
  config: { blendId: "blend-1" },
};
const plugin: StoredEffectPreset = { id: "efp-plugin", name: "Synth", parameters: {}, config: { pluginName: "X" } };

function enableExport(enabled = true): void {
  uiState.appSettings = {
    "features.factoryPresetArchives.enabled": true,
    "factoryPresets.exportIncludesEffectPresets": enabled,
  } as unknown as typeof uiState.appSettings;
}

describe("effect presets in a preset collection export", () => {
  beforeEach(() => {
    vi.mocked(showNotification).mockClear();
    uiState.effectPresets = {
      [EffectGuids.kAmpNamOptimized]: [amp],
      [EffectGuids.kAmpNamBlend]: [blend],
      [EffectGuids.kPluginHost]: [plugin],
    };
  });

  it("collects nothing unless the factory archive tools and the export setting are both on", () => {
    enableExport(false);
    expect(collectEffectPresetsForExport().byEffectType).toEqual({});

    uiState.appSettings = { "factoryPresets.exportIncludesEffectPresets": true } as unknown as typeof uiState.appSettings;
    expect(collectEffectPresetsForExport().byEffectType).toEqual({});
  });

  it("collects every effect's presets but a hosted plugin's, with the resources and blends they need", () => {
    enableExport();
    const collected = collectEffectPresetsForExport();

    expect(Object.keys(collected.byEffectType).sort()).toEqual([EffectGuids.kAmpNamBlend, EffectGuids.kAmpNamOptimized].sort());
    expect(collected.refs.map((ref) => ref.resourceId)).toEqual(["lib-model"]);
    expect(collected.blendIds).toEqual(["blend-1"]);
  });

  it("moves models onto the archive's ids and drops the local file path", () => {
    enableExport();
    const blends = [{ id: "blend-1", name: "Blend", category: "", models: ["lib-model"] }] as BlendDefinition[];
    const { effectPresets } = finishEffectPresetsForArchive(collectEffectPresetsForExport(), new Map([["lib-model", "abc123"]]), blends);

    expect(effectPresets?.[EffectGuids.kAmpNamOptimized]).toEqual([
      { id: "efp-amp", name: "Crunch", parameters: { inputGain: 3 }, resources: [{ resourceType: "nam", resourceId: "abc123" }] },
    ]);
    expect(effectPresets?.[EffectGuids.kAmpNamBlend]?.[0]?.config).toEqual({ blendId: "blend-1" });
    expect(showNotification).not.toHaveBeenCalled();
  });

  it("leaves out, with a warning, a preset whose model was not read or whose blend is not carried", () => {
    enableExport();
    const result = finishEffectPresetsForArchive(collectEffectPresetsForExport(), new Map(), []);

    expect(result).toEqual({});
    expect(showNotification).toHaveBeenCalledWith("Export warning", expect.stringContaining("2 effect presets were left out"));
  });
});
