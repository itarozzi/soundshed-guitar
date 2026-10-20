import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/tone3000.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  isTone3000AuthReady: () => true,
}));

// The chain the picker was opened from: a NAM node "n1" unless a test swaps it.
const chain = vi.hoisted(() => ({ preset: null as unknown }));
vi.mock("../ts/state.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  getSignalPathPreset: () => chain.preset,
}));

import { BLEND_EDITOR_OPEN_EVENT, type BlendEditorOpenDetail } from "../ts/blendUtils.js";
import { EffectGuids } from "../ts/effectGuids.js";
import { Features, getFeatureSettingKey } from "../ts/featureFlags.js";
import { Tone3000BulkImporter } from "../ts/resourceBrowser/tone3000BulkImport.js";
import type { Tone3000Navigator } from "../ts/resourceBrowser/tone3000Navigation.js";
import type { ResourceBrowserOptions } from "../ts/resourceBrowser/types.js";
import { uiState } from "../ts/state.js";
import type { Tone3000Model, Tone3000Tone } from "../ts/tone3000ApiTypes.js";
import type { BlendDefinition } from "../ts/types.js";

const tone: Tone3000Tone = { id: 42, title: "Test Amp", gear: "amp" };
const models = ["G2", "G5", "G8"].map((name, index) => ({ id: index + 1, name, model_url: `https://x/${index + 1}.nam` })) as unknown as Tone3000Model[];

function makeImporter(options: { imported?: string[]; failing?: string[]; onImportMany?: (ids: string[]) => void } = {}) {
  const imported = new Map((options.imported ?? []).map((id) => [id, `tone3000:${id}`]));
  const downloads: string[] = [];
  let browserOptions: ResourceBrowserOptions | null = { resourceType: "nam", nodeId: "n1", onSelect: () => {}, onImportMany: options.onImportMany } as unknown as ResourceBrowserOptions;
  const navigation = {
    findImportedTone3000ResourceId: (_type: string, modelId: string) => imported.get(modelId) ?? "",
    downloadAndImportTone3000Model: async (_tone: Tone3000Tone, model: Tone3000Model) => {
      const id = String(model.id);
      downloads.push(id);
      if (options.failing?.includes(id)) {
        throw new Error("Download failed: 500");
      }
      imported.set(id, `tone3000:${id}`);
      return `tone3000:${id}`;
    },
  } as unknown as Tone3000Navigator;
  const closeModal = vi.fn(() => { browserOptions = null; });
  const importer = new Tone3000BulkImporter({
    navigation,
    getOptions: () => browserOptions,
    closeModal,
    render: () => {},
  });
  return { importer, downloads, closeModal, close: () => { browserOptions = null; } };
}

describe("Tone3000 Import All and Create Blend", () => {
  const sent: Array<{ type: string; blend?: BlendDefinition }> = [];
  const opened: BlendDefinition[] = [];
  const sourceNodes: Array<BlendEditorOpenDetail["sourceNode"]> = [];
  const onOpen = (event: Event): void => {
    const detail = (event as CustomEvent<BlendEditorOpenDetail>).detail;
    opened.push(detail.blend);
    sourceNodes.push(detail.sourceNode);
  };
  const originalBlendLibrary = uiState.blendLibrary;
  const originalAppSettings = uiState.appSettings;

  beforeEach(() => {
    sent.length = 0;
    opened.length = 0;
    sourceNodes.length = 0;
    chain.preset = { id: "p1", graph: { nodes: [{ id: "n1", type: EffectGuids.kAmpNamOptimized }] } };
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message));
    document.addEventListener(BLEND_EDITOR_OPEN_EVENT, onOpen);
  });

  afterEach(() => {
    delete window.IPlugSendMsg;
    document.removeEventListener(BLEND_EDITOR_OPEN_EVENT, onOpen);
    uiState.blendLibrary = originalBlendLibrary;
    uiState.appSettings = originalAppSettings;
  });

  it("downloads only the models not already imported", async () => {
    const { importer, downloads, closeModal } = makeImporter({ imported: ["2"] });
    await importer.importAll(tone, models, false);
    expect(downloads).toEqual(["1", "3"]);
    expect(closeModal).not.toHaveBeenCalled();
    expect(opened).toHaveLength(0);
    expect(models.every((model) => importer.isModelImported("nam", model))).toBe(true);
  });

  it("keeps going past a model that fails and leaves it not imported", async () => {
    const { importer, downloads } = makeImporter({ failing: ["2"] });
    await importer.importAll(tone, models, false);
    expect(downloads).toEqual(["1", "2", "3"]);
    expect(importer.isModelImported("nam", models[1])).toBe(false);
  });

  it("stops when the picker is closed part way through", async () => {
    const harness = makeImporter();
    const importing = harness.importer.importAll(tone, models, true);
    harness.close();
    await importing;
    expect(harness.downloads).toEqual(["1"]);
    expect(opened).toHaveLength(0);
  });

  it("saves a blend of every model, closes the picker and opens the blend editor", async () => {
    const { importer, closeModal } = makeImporter({ imported: ["1"] });
    await importer.importAll(tone, models, true);

    const saved = sent.find((message) => message.type === "saveBlendDefinition")?.blend;
    expect(saved?.models).toEqual(["tone3000:1", "tone3000:2", "tone3000:3"]);
    expect(saved?.toneGroupId).toBe("42");
    expect(closeModal).toHaveBeenCalledOnce();
    expect(opened).toEqual([saved]);
    // Started from a NAM node's picker, so the editor's Save offers the blend in its place.
    expect(sourceNodes).toEqual([{ presetId: "p1", nodeId: "n1" }]);
  });

  it("names no node to replace when the picker's node is gone or already a blend", async () => {
    chain.preset = { id: "p1", graph: { nodes: [{ id: "n1", type: EffectGuids.kAmpNamBlend }] } };
    await makeImporter({ imported: ["1", "2", "3"] }).importer.importAll(tone, models, true);
    chain.preset = { id: "p1", graph: { nodes: [] } };
    await makeImporter({ imported: ["1", "2", "3"] }).importer.importAll(tone, models, true);
    expect(opened).toHaveLength(2);
    expect(sourceNodes).toEqual([undefined, undefined]);
  });

  it("reopens the blend already made from these models instead of saving another", async () => {
    const existing: BlendDefinition = { id: "b1", name: "Mine", category: "amp", models: ["tone3000:1", "tone3000:2", "tone3000:3"], toneGroupId: "42" };
    uiState.blendLibrary = [existing];
    const { importer } = makeImporter({ imported: ["1", "2", "3"] });
    await importer.importAll(tone, models, true);
    expect(sent.some((message) => message.type === "saveBlendDefinition")).toBe(false);
    expect(opened).toEqual([existing]);
  });

  it("offers Create Blend only with Blend Tools on, and Import All only while models are missing", () => {
    uiState.appSettings = { ...uiState.appSettings, [getFeatureSettingKey(Features.BlendTools)]: false };
    const partial = makeImporter({ imported: ["1"] }).importer;
    expect(partial.renderToolbar(tone, models, "nam")).toContain("Import Remaining");
    expect(partial.renderToolbar(tone, models, "nam")).not.toContain("Create Blend");

    uiState.appSettings = { ...uiState.appSettings, [getFeatureSettingKey(Features.BlendTools)]: true };
    const complete = makeImporter({ imported: ["1", "2", "3"] }).importer;
    const toolbar = complete.renderToolbar(tone, models, "nam");
    expect(toolbar).toContain("All 3 models imported");
    expect(toolbar).not.toContain("resource-browser-tone-import-all");
    expect(toolbar).toContain("Create Blend");
    expect(complete.renderToolbar(tone, models, "ir")).not.toContain("Create Blend");
  });

  it("opened by the blend editor, adds the whole tone to the blend instead of making one", async () => {
    uiState.appSettings = { ...uiState.appSettings, [getFeatureSettingKey(Features.BlendTools)]: true };
    const added: string[][] = [];
    const { importer, downloads, closeModal } = makeImporter({ imported: ["1", "2", "3"], onImportMany: (ids) => added.push(ids) });
    const toolbar = importer.renderToolbar(tone, models, "nam");
    expect(toolbar).toContain("Add All to Blend");
    expect(toolbar).not.toContain("Create Blend");

    await importer.importAll(tone, models, false);
    expect(downloads).toEqual([]);
    expect(closeModal).toHaveBeenCalledOnce();
    expect(added).toEqual([["tone3000:1", "tone3000:2", "tone3000:3"]]);
    expect(sent.some((message) => message.type === "saveBlendDefinition")).toBe(false);
  });
});
