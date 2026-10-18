/**
 * The app side of the Tone Builder: the service client (catalogue hash and upload, job polling)
 * and turning a proposal into presets (which captures to fetch, building, crediting, saving).
 */
// @vitest-environment jsdom
import { createHash } from "node:crypto";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { ToneProposal as Proposal } from "../ts/toneBuilder/jobTypes.js";

const apiFetch = vi.fn();
const toneSharingFetch = vi.fn();
const posted: Array<Record<string, unknown>> = [];
const library: Record<string, Array<{ id: string; fileMissing?: boolean }>> = { nam: [], ir: [] };
const assignFolder = vi.fn();

vi.mock("../ts/toneSharingPanel/api.js", () => ({ apiFetch, toneSharingFetch }));
vi.mock("../ts/bridge.js", () => ({ postMessage: (message: Record<string, unknown>) => posted.push(message) }));
vi.mock("../ts/resourceLibrary.js", () => ({
  getLibraryResource: (type: string, id: string) => library[type]?.find((r) => r.id === id) ?? null,
}));
vi.mock("../ts/tone3000.js", () => ({ downloadTone3000ResourceByReference: vi.fn() }));
vi.mock("../ts/presets/folderArchive.js", () => ({ assignImportedPresetsToTopLevelFolder: assignFolder }));
vi.mock("../ts/presets/cache.js", () => ({ cachePresetInMemory: vi.fn() }));
vi.mock("../ts/presetLibraryStore.js", () => ({ cachePreset: vi.fn(), putLibraryPresetFirst: vi.fn() }));
vi.mock("../ts/presets/sanitize.js", () => ({ sanitizeImportedPreset: <T>(preset: T) => preset }));
vi.mock("../ts/toneBuilder/appInventory.js", () => ({
  readAppBuildInventory: () => ({
    effects: [
      { type: "amp-type", displayName: "Neural Amp (NAM)", category: "amp", requiresResource: true, resourceType: "nam", parameters: [], presets: [] },
      { type: "cab-type", displayName: "Simple Cabinet", category: "cab", parameters: [], presets: [{ name: "1960 4x12 V30", parameters: {} }] },
    ],
    resources: library.nam.map((r) => ({ ...r, type: "nam", name: "Plexi", provider: "tone3000" })),
  }),
}));

const { canonicalJson, ensureEffectCatalogue, waitForToneBuilderJob } = await import("../ts/toneBuilder/service.js");
const { buildAndSaveProposal, collectProposalCaptures, planCaptureDownloads } = await import("../ts/toneBuilder/realise.js");

function captureRef(ref: string, provider = "tone3000") {
  return {
    ref, provider, key: ref.split(":")[1], type: "nam" as const, sha256: null, gearId: "amp/marshall/plexi", captureScope: "amp",
    licence: { id: "cc-by", url: null, policy: "redistributable" as const, attribution: `"Plexi" by Ann (${provider}, cc-by)` },
    creator: { name: "Ann", url: null }, sourceUrl: "https://example/plexi", title: "Plexi", providerData: { toneId: "1", modelId: ref.split(":")[1] },
  };
}

function proposal(): Proposal {
  return {
    schema: "soundshed.tone-proposal/2", effectCatalogHash: "h", dossierId: null, notes: "internal only",
    packTitle: "Test – Crunch", packDescription: "A crunch rig.", sources: [{ title: "Rig rundown", url: "https://www.example.com/rig" }], artworkPrompt: "A stage",
    validation: { ok: true, problems: [], presets: [] },
    credits: [{ ref: "tone3000:1", provider: "tone3000", title: "Plexi", creator: "Ann", creatorUrl: null, sourceUrl: "https://example/plexi", licence: "cc-by", licenceUrl: null, licencePolicy: "redistributable", attribution: `"Plexi" by Ann (tone3000, cc-by)` }],
    presets: [{
      key: "rhythm", name: "Rhythm", description: "Crunch", tags: [], tuning: null, gearIds: [],
      scenes: [{ key: "main", title: "Main", chain: [
        { fx: "Neural Amp (NAM)", preset: null, set: {}, label: null, off: false, resource: { ...captureRef("tone3000:1"), fallback: null } },
        { split: [[{ fx: "Neural Amp (NAM)", preset: null, set: {}, label: null, off: false, resource: { ...captureRef("other:9", "other"), fallback: null } }], []], gains: null, masterLevel: null, label: null },
        { fx: "Simple Cabinet", preset: "1960 4x12 V30", set: {}, label: null, off: false, resource: { gearId: "cab/marshall/1960", scope: "cab", fallback: { fx: "Simple Cabinet", preset: "1960 4x12 V30", set: {} } } },
      ] }],
    }],
    builderPresets: [{ key: "rhythm", name: "Rhythm", description: "Crunch", chain: [{ fx: "Neural Amp (NAM)", resource: { id: "tone3000:1" } }, { fx: "Simple Cabinet", preset: "1960 4x12 V30" }] }],
  };
}

afterEach(() => {
  apiFetch.mockReset();
  toneSharingFetch.mockReset();
  posted.length = 0;
  library.nam = [];
  assignFolder.mockReset();
});

describe("the service client", () => {
  it("hashes the catalogue as the service does: keys sorted at every level", () => {
    expect(canonicalJson({ b: 1, a: [{ d: 2, c: undefined, e: "x" }] })).toBe(`{"a":[{"d":2,"e":"x"}],"b":1}`);
  });

  it("uploads the catalogue only when the service lacks it", async () => {
    const effects = [{ type: "t", displayName: "T", category: "c", parameters: [], presets: [] }];
    const expected = createHash("sha256").update(canonicalJson(effects)).digest("hex");
    toneSharingFetch.mockResolvedValueOnce({ ok: true, status: 200 });
    expect(await ensureEffectCatalogue(effects)).toBe(expected);
    expect(apiFetch).not.toHaveBeenCalled();

    toneSharingFetch.mockResolvedValueOnce({ ok: false, status: 404 });
    apiFetch.mockResolvedValueOnce({ hash: expected, stored: true });
    await ensureEffectCatalogue(effects);
    expect(apiFetch).toHaveBeenCalledWith(`/tone-builder/catalogues/${expected}`, expect.objectContaining({ method: "PUT" }));
  });

  it("polls until the job is done", async () => {
    apiFetch
      .mockResolvedValueOnce({ jobId: "j", status: "researching", progress: [], error: null })
      .mockResolvedValueOnce({ jobId: "j", status: "done", progress: [], error: null, proposal: proposal() });
    const updates: string[] = [];
    const job = await waitForToneBuilderJob("j", (update) => updates.push(update.status), undefined, async () => {});
    expect(job.status).toBe("done");
    expect(updates).toEqual(["researching", "done"]);
  });
});

describe("realising a proposal", () => {
  it("finds every capture once and plans the downloads", () => {
    expect(collectProposalCaptures(proposal()).map((ref) => ref.ref)).toEqual(["tone3000:1", "other:9"]);
    const plan = planCaptureDownloads(proposal());
    expect(plan.toFetch.map((ref) => ref.ref)).toEqual(["tone3000:1"]);
    expect(plan.unfetchable.map((ref) => ref.ref)).toEqual(["other:9"]);

    library.nam = [{ id: "tone3000:1" }];
    expect(planCaptureDownloads(proposal()).toFetch).toEqual([]);
  });

  it("builds, credits, saves and files the presets", () => {
    library.nam = [{ id: "tone3000:1" }];
    const { presets } = buildAndSaveProposal("job-1", proposal(), "Tone Builder – Test");
    expect(presets).toHaveLength(1);
    expect(presets[0].description).toBe("Crunch\n\nCaptures: Plexi by Ann.\n\nSources: Rig rundown (example.com).");
    expect(presets[0].description).not.toMatch(/cc-by|tone3000|licen/i);
    const save = posted.find((m) => m.type === "savePreset");
    expect(save).toMatchObject({ saveMode: "import", presetId: presets[0].id, name: "Rhythm", category: "Tone Builder", includeGlobalSignalChain: false });
    expect(assignFolder).toHaveBeenCalledWith("Tone Builder – Test", [presets[0].id]);
  });

  it("refuses to build on a capture the library does not have", () => {
    expect(() => buildAndSaveProposal("job-1", proposal(), "F")).toThrow(/not found or file missing/);
    expect(posted).toEqual([]);
  });
});

describe("capture file names", () => {
  it("keep long provider titles short enough for Windows paths, and unique by key", async () => {
    const { captureFileName } = await import("../ts/toneBuilder/realise.js");
    const name = captureFileName({
      title: "Fender Vibroverb 1964 - Dumble - Two-Rock - John Mayer / Fender® Vibroverb® 1964 - Dumble® Steel String Singer® #002 - Two-Rock® John Mayer Signature Prototype (_Signature #83_)",
      key: "418176",
    }, ".nam");
    expect(name.length).toBeLessThanOrEqual(60 + 1 + 6 + 4);
    expect(name).toBe("Fender-Vibroverb-1964-Dumble-Two-Rock-John-Mayer-Fender-Vibr-418176.nam");
    expect(captureFileName({ title: "®®®", key: "7" }, ".wav")).toBe("capture-7.wav");
  });
});
