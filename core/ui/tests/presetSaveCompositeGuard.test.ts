import { beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

// While a composite is open for editing, the chain on screen is the composite's inner graph. Saving
// then sent that graph as a "__composite_edit_<id>__" preset and put it in the active preset's
// place in the cache and the library. Save, Save As and Edit now refuse until the editor closes.

const postMessage = vi.fn();
const showNotification = vi.fn();

vi.mock("../ts/bridge.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  postMessage: (...args: unknown[]) => postMessage(...args),
}));
vi.mock("../ts/notifications.js", () => ({
  clearNotification: vi.fn(),
  showNotification: (...args: unknown[]) => showNotification(...args),
}));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));
vi.mock("../ts/dataLibraries.js", () => ({
  REMOTE_BASE_URL: "",
  buildAttachmentsFromPreset: () => [],
  getDefaultPresets: () => [],
}));
vi.mock("../ts/presets/toolbar.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  updatePresetActionButtons: vi.fn(),
}));

const { uiState } = await import("../ts/state.js");
const { saveOverwriteCurrentPreset, openEditPresetModal } = await import("../ts/presets/actions.js");
const { openSavePresetModal } = await import("../ts/presets/saveModal.js");

function preset(id: string): Preset {
  return {
    id,
    name: id,
    category: "User",
    version: 2,
    graph: { nodes: [{ id: "in", type: "input", params: {} }, { id: "out", type: "output", params: {} }], edges: [] },
  } as unknown as Preset;
}

beforeEach(() => {
  postMessage.mockReset();
  showNotification.mockReset();
  uiState.activePresetId = "user-my-preset";
  uiState.activePresetDraft = preset("user-my-preset");
  uiState.presetCache = new Map([["user-my-preset", preset("user-my-preset")]]);
  uiState.compositeEditMode = true;
  uiState.compositeEditPreset = preset("__composite_edit_comp-1__");
});

describe("saving while a composite is open for editing", () => {
  it("Save sends nothing and leaves the active preset alone", () => {
    saveOverwriteCurrentPreset();

    expect(postMessage).not.toHaveBeenCalled();
    expect(uiState.presetCache.get("user-my-preset")?.id).toBe("user-my-preset");
    expect(uiState.activePresetDraft?.id).toBe("user-my-preset");
    expect(showNotification).toHaveBeenCalledTimes(1);
  });

  it("Save As and Edit do not open the preset modal", () => {
    openSavePresetModal();
    openEditPresetModal();

    expect(postMessage).not.toHaveBeenCalled();
    expect(showNotification).toHaveBeenCalledTimes(2);
  });
});
