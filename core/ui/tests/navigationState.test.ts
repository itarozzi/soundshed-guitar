import { beforeEach, describe, expect, it } from "vitest";
import {
  getNavigationViewState,
  mergeNavigationViewState,
  replaceNavigationViewState,
} from "../ts/navigationState.js";

beforeEach(() => {
  replaceNavigationViewState({
    mainPanel: "visualizer",
    settings: { equipmentTab: "settings", libraryTab: "tone3000" },
  });
});

describe("navigation view state", () => {
  it("merges one settings tab without losing the others", () => {
    expect(mergeNavigationViewState({ settings: { libraryTab: "presets" } })).toBe(true);
    expect(getNavigationViewState()).toEqual({
      mainPanel: "visualizer",
      settings: { equipmentTab: "settings", libraryTab: "presets" },
    });
    expect(mergeNavigationViewState({ settings: { libraryTab: "presets" } })).toBe(false);
  });

  it("replaces the complete view during host restoration", () => {
    replaceNavigationViewState({ mainPanel: "sharing" });
    expect(getNavigationViewState()).toEqual({ mainPanel: "sharing" });
  });
});
