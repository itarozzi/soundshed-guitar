/**
 * Preset archive exports are built by the engine (exportPresetArchive,
 * core/src/controller/PluginControllerArchiveExport.cpp, covered by PresetArchiveExportTests).
 * The web UI only names the presets, the archive and the folder, and shows the answer.
 */
import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

const sent: Array<Record<string, unknown>> = [];
window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);

const { uiState } = await import("../ts/state.js");
const { showNotification } = await import("../ts/notifications.js");
const archive = await import("../ts/presets/archive.js");
const { onPresetExportSaved } = await import("../ts/messages/presetHandlers.js");

const preset = (id: string, name: string): Preset => ({ id, name, graph: { nodes: [], edges: [] } }) as unknown as Preset;

function exportRequests(): Array<Record<string, unknown>> {
  return sent.filter((message) => message.type === "exportPresetArchive");
}

describe("preset archive export", () => {
  beforeEach(() => {
    sent.length = 0;
    vi.mocked(showNotification).mockClear();
    uiState.presets = [preset("p1", "Chug"), preset("p2", "Lead")];
    uiState.presetCache = new Map();
    uiState.presetFolders = [{ id: "folder-1", name: "Metal Gigs", presetIds: ["p2"], children: [] }];
    uiState.activePresetFolderId = "__all__";
    uiState.activePresetId = null as unknown as string;
    uiState.presetArchiveSession = null;
  });

  it("asks the engine for every preset, with the folder tree, from All Presets", () => {
    archive.exportSelectedPresetCollectionArchive();
    expect(exportRequests()).toEqual([
      { type: "exportPresetArchive", presetIds: ["p1", "p2"], name: "All-Presets", folderId: "__all__", collection: true },
    ]);
    expect(sent.some((message) => message.type === "savePresetArchive")).toBe(false);
  });

  it("asks for the open folder's presets under the folder's name", () => {
    uiState.activePresetFolderId = "folder-1";
    archive.exportSelectedPresetCollectionArchive();
    expect(exportRequests()).toEqual([
      { type: "exportPresetArchive", presetIds: ["p2"], name: "Metal Gigs", folderId: "folder-1", collection: true },
    ]);
  });

  it("asks for the current preset alone, named after it", () => {
    uiState.activePresetId = "p1";
    uiState.presetCache.set("p1", preset("p1", "Chug"));
    archive.exportCurrentPresetArchive();
    expect(exportRequests()).toEqual([{ type: "exportPresetArchive", presetIds: ["p1"], name: "Chug" }]);
  });

  it("names an archive session's export after the archive", () => {
    uiState.presetArchiveSession = { active: true, archiveName: "Friday.soundshed.presets" };
    archive.exportPresetArchiveSession();
    expect(exportRequests()).toEqual([
      { type: "exportPresetArchive", presetIds: ["p1", "p2"], name: "Friday.soundshed.presets", folderId: "__all__", collection: true },
    ]);
  });

  it("sends nothing when there is nothing to export", () => {
    archive.exportCurrentPresetArchive();
    uiState.presets = [];
    archive.exportAllPresetsArchive();
    archive.exportPresetArchiveSession();
    expect(exportRequests()).toEqual([]);
    expect(vi.mocked(showNotification).mock.calls.map(([title]) => title)).toEqual([
      "Export failed",
      "Export failed",
      "No archive session",
    ]);
  });

  it("shows the engine's warnings with the saved path", () => {
    onPresetExportSaved({ type: "presetExportSaved", path: "C:/out/Pack.soundshed.presets", warnings: ["1 preset could not be found", 3] });
    expect(vi.mocked(showNotification).mock.calls).toEqual([
      ["Preset exported", "C:/out/Pack.soundshed.presets"],
      ["Export warning", "1 preset could not be found"],
    ]);
  });
});
