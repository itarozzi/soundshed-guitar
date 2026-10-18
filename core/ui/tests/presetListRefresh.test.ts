/**
 * "presetList" also arrives unasked: the engine answers a delete with it, and shared sync
 * brings one. Taking it must not undo what the library is showing.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

window.IPlugSendMsg = () => undefined;

const { uiState } = await import("../ts/state.js");
const { onPresetList } = await import("../ts/messages/presetHandlers.js");
const { PRESET_FOLDER_ALL_ID, PRESET_FOLDER_FAVORITES_ID, PRESET_FOLDER_RECENTS_ID } = await import("../ts/presets/sorting.js");
const { isStoredPreset, showAllLibraryPresets } = await import("../ts/presetLibraryStore.js");
const { activeTagFilters, getFilteredPresets } = await import("../ts/presets/filter.js");
const { renderPresetUI } = await import("../ts/presets/library.js");

const summary = (id: string) => ({ id, name: id, category: "User" });

beforeEach(() => {
  uiState.presets = [];
  uiState.filteredPresets = [];
  uiState.presetCache = new Map();
  uiState.activePresetId = null;
  uiState.activePresetIsNew = false;
  uiState.activePresetFolderId = PRESET_FOLDER_ALL_ID;
  uiState.presetFavorites = new Set();
  uiState.presetFolders = [];
  uiState.appSettings = {};
  uiState.uiSettings = { zoom: 1, presetRecents: [] };
  activeTagFilters.clear();
});

describe("hiding factory preset packs", () => {
  const presets = [
    { id: "pack-clean", name: "Clean Factory", category: "Clean", source: "factory" },
    { id: "user-clean", name: "Clean User", category: "Factory", source: "user" },
    { id: "session-clean", name: "Clean Session", source: "session" },
  ];

  it("keeps source metadata and hides only factory-marked presets, not a user category", () => {
    uiState.appSettings = { "features.factoryPresetPacks.enabled": false };
    onPresetList({ type: "presetList", presets });

    expect(uiState.presets[0].source).toBe("factory");
    expect(uiState.filteredPresets.map((preset) => preset.id)).toEqual(["session-clean", "user-clean"]);
    expect(isStoredPreset("pack-clean")).toBe(true);
    expect(uiState.presetCache.has("pack-clean")).toBe(true);
    expect(getFilteredPresets("Clean Factory")).toEqual([]);
    expect(getFilteredPresets("user").map((preset) => preset.id)).toEqual(["user-clean"]);
  });

  it("applies to Favorites, folders, tags and Recents, even with a full cached body lacking source", () => {
    onPresetList({ type: "presetList", presets });
    uiState.appSettings = { "features.factoryPresetPacks.enabled": false };
    uiState.presetFavorites = new Set(["pack-clean", "user-clean"]);
    uiState.activePresetFolderId = PRESET_FOLDER_FAVORITES_ID;
    expect(getFilteredPresets("").map((preset) => preset.id)).toEqual(["user-clean"]);

    uiState.presetFolders = [{ id: "folder", name: "Folder", children: [], presetIds: ["pack-clean", "user-clean"] }];
    uiState.activePresetFolderId = "folder";
    expect(getFilteredPresets("").map((preset) => preset.id)).toEqual(["user-clean"]);

    uiState.presetCache.set("pack-clean", { id: "pack-clean", name: "Clean Factory", tags: ["Clean"] });
    uiState.presetCache.set("user-clean", { id: "user-clean", name: "Clean User", tags: ["Clean"] });
    uiState.uiSettings = { zoom: 1, presetRecents: ["user-clean", "pack-clean", "session-clean"] };
    uiState.activePresetFolderId = PRESET_FOLDER_RECENTS_ID;
    expect(getFilteredPresets("").map((preset) => preset.id)).toEqual(["user-clean", "session-clean"]);
    activeTagFilters.add("Clean");
    expect(getFilteredPresets("clean").map((preset) => preset.id)).toEqual(["user-clean"]);
  });

  it("reapplies a late-arriving preference and view resets without unloading the active preset", () => {
    onPresetList({ type: "presetList", presets });
    uiState.activePresetId = "pack-clean";
    expect(uiState.filteredPresets).toHaveLength(3);

    uiState.appSettings = { "features.factoryPresetPacks.enabled": false };
    showAllLibraryPresets();
    renderPresetUI(uiState.presetCache.get("pack-clean") ?? null);
    expect(uiState.filteredPresets.map((preset) => preset.id)).toEqual(["session-clean", "user-clean"]);
    expect(uiState.activePresetId).toBe("pack-clean");

    uiState.appSettings = { "features.factoryPresetPacks.enabled": true };
    renderPresetUI(uiState.presetCache.get("pack-clean") ?? null);
    expect(uiState.filteredPresets).toHaveLength(3);
  });

  it("handles an all-factory library and an empty library", () => {
    uiState.appSettings = { "features.factoryPresetPacks.enabled": false };
    onPresetList({ type: "presetList", presets: [presets[0]] });
    expect(uiState.filteredPresets).toEqual([]);
    onPresetList({ type: "presetList", presets: [] });
    expect(getFilteredPresets("")).toEqual([]);
  });
});

describe("taking the engine's preset list", () => {
  it("keeps the folder the library is showing", () => {
    uiState.activePresetFolderId = PRESET_FOLDER_FAVORITES_ID;
    uiState.presetFavorites = new Set(["b"]);

    onPresetList({ type: "presetList", presets: [summary("a"), summary("b"), summary("c")] });

    expect(uiState.presets.map((preset) => preset.id)).toEqual(["a", "b", "c"]);
    expect(uiState.filteredPresets.map((preset) => preset.id)).toEqual(["b"]);
  });

  it("keeps a new preset listed until it is saved, without taking it for a stored one", () => {
    uiState.presetCache.set("user-new", { id: "user-new", name: "New Preset", category: "User" } as Preset);
    uiState.activePresetId = "user-new";
    uiState.activePresetIsNew = true;

    onPresetList({ type: "presetList", presets: [summary("a")] });

    expect(uiState.presets.map((preset) => preset.id)).toEqual(["user-new", "a"]);
    expect(isStoredPreset("a")).toBe(true);
    expect(isStoredPreset("user-new")).toBe(false);
  });
});
