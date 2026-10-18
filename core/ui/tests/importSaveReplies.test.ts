/**
 * An import's saves are answered with "presetImported", which stocks the library and leaves the
 * active preset alone. A pack of N presets must not flip the active id N times on the way, nor
 * arm the echo guard against the state the import's closing loadPreset brings.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

window.IPlugSendMsg = () => undefined;

const { uiState } = await import("../ts/state.js");
const { onPresetImported, onPresetLoaded } = await import("../ts/messages/presetHandlers.js");
const { isStoredPreset } = await import("../ts/presetLibraryStore.js");
const { shouldIgnoreStatePreset } = await import("../ts/messages/echoGuard.js");

function preset(id: string): Preset {
  return {
    id,
    name: `Preset ${id}`,
    category: "User",
    graph: { nodes: [], edges: [] },
    scenes: [{ id: "scene-1", title: "Scene 1", graph: { nodes: [], edges: [] } }],
  } as unknown as Preset;
}

beforeEach(() => {
  uiState.presets = [];
  uiState.filteredPresets = [];
  uiState.presetCache = new Map();
  uiState.activePresetId = "playing";
  uiState.activePresetIsNew = false;
  uiState.presetDirty = true;
});

describe("the replies to an import's saves", () => {
  it("stock the library without changing the active preset, until the import loads one", () => {
    const imported = ["p1", "p2", "p3"];
    for (const id of imported) {
      onPresetImported({ type: "presetImported", preset: preset(id) });
    }

    expect(uiState.activePresetId).toBe("playing");
    expect(uiState.presetDirty).toBe(true);
    expect(imported.every((id) => isStoredPreset(id))).toBe(true);
    expect(imported.every((id) => uiState.presetCache.has(id))).toBe(true);
    expect(uiState.presets.map((entry) => entry.id)).toEqual(imported);
    // No echo guard is armed, so the state the closing loadPreset brings gets through.
    expect(shouldIgnoreStatePreset(preset("p3"))).toBe(false);

    onPresetLoaded({ type: "presetLoaded", preset: preset("p3"), sceneId: "scene-1" });

    expect(uiState.activePresetId).toBe("p3");
  });

  it("replace the library's copy with the preset as the engine stored it", () => {
    uiState.presets = [{ ...preset("p1"), name: "As imported" }];
    uiState.presetCache.set("p1", { ...preset("p1"), name: "As imported" });

    onPresetImported({ type: "presetImported", preset: { ...preset("p1"), name: "As stored" } });

    expect(uiState.presets.map((entry) => entry.id)).toEqual(["p1"]);
    expect(uiState.presetCache.get("p1")?.name).toBe("As stored");
  });

  it("ignore a reply without a preset", () => {
    onPresetImported({ type: "presetImported" });

    expect(uiState.presets).toEqual([]);
    expect(uiState.activePresetId).toBe("playing");
  });
});
