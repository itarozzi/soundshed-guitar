// @vitest-environment node
import { afterEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types.js";

const apiFetch = vi.fn();
const createItem = vi.fn();
vi.mock("../ts/toneSharingPanel/api.js", () => ({ apiFetch }));
vi.mock("../ts/toneSharingPanel/consent.js", () => ({ ensurePublishConsent: vi.fn() }));
const uploadAsset = vi.fn().mockResolvedValue("asset-thumb");
vi.mock("../ts/toneSharingPanel/itemUpload.js", () => ({ createToneSharingPresetItem: createItem, uploadToneSharingAsset: uploadAsset }));
vi.mock("../ts/toneSharingPanel/images.js", () => ({ buildPackImageVariants: async () => ({ small: { blob: new Blob(["small"]) }, large: { blob: new Blob(["large"]) } }) }));

const { sharePresets } = await import("../ts/toneBuilder/share.js");

const preset = (id: string, name: string) => ({ id, name, description: `About ${name}` }) as unknown as Preset;

afterEach(() => {
  apiFetch.mockReset();
  createItem.mockReset();
});

describe("sharing Tone Builder presets", () => {
  it("submits each preset and gathers two or more into a pack", async () => {
    createItem.mockImplementation(async (p: Preset) => ({ id: `itm-${p.id}` }));
    apiFetch.mockImplementation(async (path: string) => (path === "/packs" ? { pack: { id: "pak-1" } } : {}));
    const result = await sharePresets([preset("a", "Rhythm"), preset("b", "Lead")], "AC/DC – Back in Black", "Notes", () => {}, new Blob(["jpeg"], { type: "image/jpeg" }));

    expect(result).toEqual({ itemIds: ["itm-a", "itm-b"], packId: "pak-1", failed: [] });
    expect(createItem).toHaveBeenCalledWith(expect.objectContaining({ id: "a" }), { title: "Rhythm", description: "About Rhythm" });
    const paths = apiFetch.mock.calls.map((call) => `${call[1]?.method ?? "GET"} ${call[0]}`);
    expect(paths).toEqual([
      "POST /items/itm-a/publish",
      "POST /items/itm-b/publish",
      "POST /packs",
      "POST /packs/pak-1/items",
      "POST /packs/pak-1/publish",
    ]);
    expect(JSON.parse(apiFetch.mock.calls[3][1].body)).toEqual({ itemIds: ["itm-a", "itm-b"] });
    expect(uploadAsset).toHaveBeenCalledWith("thumbnail", expect.any(Blob));
    expect(JSON.parse(apiFetch.mock.calls[2][1].body)).toEqual({ title: "AC/DC – Back in Black", description: "Notes", thumbnailAssetId: "asset-thumb" });
  });

  it("shares one preset without a pack, and reports a failure without stopping the rest", async () => {
    createItem.mockImplementationOnce(async () => {
      throw new Error("upload refused");
    }).mockImplementationOnce(async () => ({ id: "itm-b" }));
    apiFetch.mockResolvedValue({});
    const result = await sharePresets([preset("a", "Rhythm"), preset("b", "Lead")], "T", "", () => {});

    expect(result.itemIds).toEqual(["itm-b"]);
    expect(result.packId).toBeNull();
    expect(result.failed).toEqual([{ name: "Rhythm", message: "upload refused" }]);
    expect(apiFetch.mock.calls.map((call) => call[0])).toEqual(["/items/itm-b/publish"]);
  });
});
