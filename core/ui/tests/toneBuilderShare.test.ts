// @vitest-environment node
import { afterEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types.js";

const apiFetch = vi.fn();
const uploadArchives = vi.fn();
vi.mock("../ts/toneSharingPanel/api.js", () => ({ apiFetch }));
const uploadAsset = vi.fn().mockResolvedValue("asset-thumb");
vi.mock("../ts/toneSharingPanel/itemUpload.js", () => ({ uploadPresetArchives: uploadArchives, uploadToneSharingAsset: uploadAsset }));
vi.mock("../ts/toneSharingPanel/images.js", () => ({ buildPackImageVariants: async () => ({ small: { blob: new Blob(["small"]) }, large: { blob: new Blob(["large"]) } }) }));

const { publishToneBuilderPresets } = await import("../ts/toneBuilder/share.js");

const preset = (id: string, name: string) => ({ id, name, description: `About ${name}` }) as unknown as Preset;

afterEach(() => {
  apiFetch.mockReset();
  uploadArchives.mockReset();
  uploadAsset.mockClear();
});

describe("publishing Tone Builder presets", () => {
  it("uploads each preset's archives and the artwork, then hands them to the job's publish", async () => {
    uploadArchives.mockImplementation(async (p: Preset) => ({ payloadAssetId: `pub-${p.id}`, privatePayloadAssetId: `priv-${p.id}` }));
    apiFetch.mockResolvedValue({ published: { itemIds: ["itm-a", "itm-b"], packId: "pak-1" }, alreadyPublished: false });
    const progress: string[] = [];
    const result = await publishToneBuilderPresets(
      "tbj_1",
      [{ key: "rhythm", preset: preset("a", "Rhythm") }, { key: "lead", preset: preset("b", "Lead") }],
      (message) => progress.push(message),
      new Blob(["jpeg"], { type: "image/jpeg" }),
    );

    expect(result).toEqual({ itemIds: ["itm-a", "itm-b"], packId: "pak-1", alreadyPublished: false });
    expect(uploadAsset).toHaveBeenCalledWith("thumbnail", expect.any(Blob));
    expect(apiFetch).toHaveBeenCalledTimes(1);
    const [path, init] = apiFetch.mock.calls[0];
    expect(path).toBe("/tone-builder/jobs/tbj_1/publish");
    expect(init.method).toBe("POST");
    // Only which archive is which preset: the service writes the titles, text and tags itself.
    expect(JSON.parse(init.body)).toEqual({
      presets: [
        { key: "rhythm", payloadAssetId: "pub-a", privatePayloadAssetId: "priv-a" },
        { key: "lead", payloadAssetId: "pub-b", privatePayloadAssetId: "priv-b" },
      ],
      thumbnailAssetId: "asset-thumb",
    });
    expect(progress).toEqual(["Publishing 1 of 2: Rhythm", "Publishing 2 of 2: Lead", "Publishing on Tone Sharing"]);
  });

  it("publishes without a thumbnail when there is no artwork, and fails as a whole when an upload fails", async () => {
    uploadArchives.mockResolvedValueOnce({ payloadAssetId: "pub-a", privatePayloadAssetId: "priv-a" });
    apiFetch.mockResolvedValue({ published: { itemIds: ["itm-a"], packId: null }, alreadyPublished: true });
    const result = await publishToneBuilderPresets("tbj_2", [{ key: "rhythm", preset: preset("a", "Rhythm") }], () => {});
    expect(result.alreadyPublished).toBe(true);
    expect(JSON.parse(apiFetch.mock.calls[0][1].body).thumbnailAssetId).toBeNull();
    expect(uploadAsset).not.toHaveBeenCalled();

    apiFetch.mockClear();
    uploadArchives.mockRejectedValueOnce(new Error("upload refused"));
    await expect(publishToneBuilderPresets("tbj_3", [{ key: "rhythm", preset: preset("a", "Rhythm") }], () => {})).rejects.toThrow("upload refused");
    expect(apiFetch).not.toHaveBeenCalled();
  });
});
