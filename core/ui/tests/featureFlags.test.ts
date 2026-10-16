import { beforeEach, describe, expect, it } from "vitest";
import { uiState } from "../ts/state.js";
import { FEATURE_GROUPS, Features, isFeatureEnabled } from "../ts/featureFlags.js";

describe("Multi-Rig feature flag", () => {
  beforeEach(() => {
    uiState.appSettings = {} as typeof uiState.appSettings;
  });

  it("is on by default", () => {
    expect(isFeatureEnabled(Features.MultiRig)).toBe(true);
  });

  it("no longer follows the legacy advanced-options switch", () => {
    uiState.appSettings = { "ui.advancedOptionsEnabled": false } as unknown as typeof uiState.appSettings;
    expect(isFeatureEnabled(Features.MultiRig)).toBe(true);
  });

  it("stays off for a user who switched it off explicitly", () => {
    uiState.appSettings = { "features.multiRig.enabled": false } as unknown as typeof uiState.appSettings;
    expect(isFeatureEnabled(Features.MultiRig)).toBe(false);
  });

  it("is listed under Core Features, not Power Features", () => {
    const core = FEATURE_GROUPS.find((group) => group.id === "core");
    const power = FEATURE_GROUPS.find((group) => group.id === "power");
    expect(core?.featureIds).toContain(Features.MultiRig);
    expect(power?.featureIds).not.toContain(Features.MultiRig);
  });
});

describe("Pickup Match feature flag", () => {
  beforeEach(() => {
    uiState.appSettings = {} as typeof uiState.appSettings;
  });

  it("is off by default, as an experimental Power Feature", () => {
    expect(isFeatureEnabled(Features.PickupMatch)).toBe(false);
    expect(FEATURE_GROUPS.find((group) => group.id === "power")?.featureIds).toContain(Features.PickupMatch);
  });

  it("does not follow the legacy advanced-options switch", () => {
    uiState.appSettings = { "ui.advancedOptionsEnabled": true } as unknown as typeof uiState.appSettings;
    expect(isFeatureEnabled(Features.PickupMatch)).toBe(false);
  });

  it("is on for a user who switched it on", () => {
    uiState.appSettings = { "features.pickupMatch.enabled": true } as unknown as typeof uiState.appSettings;
    expect(isFeatureEnabled(Features.PickupMatch)).toBe(true);
  });
});
