import { afterEach, describe, expect, it, vi } from "vitest";
import { backgroundImageDrawnWidth, resolveLayoutImageUrl } from "../ts/layoutImages.js";
import { ensureLayoutImageRenditions, missingRenditionEdges, resetRenditionMakerForTests, type RenditionMakerDeps } from "../ts/layoutRenditionMaker.js";
import { renderCustomLayout } from "../ts/layoutRenderer.js";
import type { EffectLayout, LayoutImageRef } from "../ts/layoutTypes.js";
import { onLayoutImageRenditionSaved } from "../ts/messages/layoutHandlers.js";
import { uiState } from "../ts/state.js";
import type { GraphNode } from "../ts/types.js";

const huge: LayoutImageRef = { imageId: "huge_1", fileName: "huge_1.png", url: "layout-images/id/huge_1?v=9", version: "9", width: 6000, height: 4000 };

function deps(): RenditionMakerDeps & { posted: Record<string, unknown>[] } {
  const posted: Record<string, unknown>[] = [];
  // What the default sender posts: the copy as a saveLayoutImageRendition message.
  return {
    posted,
    fetchBlob: vi.fn(async () => new Blob([new Uint8Array([1, 2, 3])])),
    make: vi.fn(async (_blob: Blob, edge: number) => ({ blob: new Blob([new Uint8Array([edge % 256])]), width: edge, height: Math.round(edge / 1.5) })),
    send: (copy) => posted.push({ type: "saveLayoutImageRendition", ...copy }),
  };
}

afterEach(() => {
  resetRenditionMakerForTests();
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
});

describe("missingRenditionEdges", () => {
  it("lists the copies a large image still lacks", () => {
    expect(missingRenditionEdges(huge)).toEqual([1024, 2048, 4096]);
    expect(missingRenditionEdges({ ...huge, renditions: [{ edge: 2048, width: 2048, height: 1365, url: "x" }] })).toEqual([1024, 4096]);
  });

  it("lists none for a small image, a data URL, or an image whose size or version is unknown", () => {
    expect(missingRenditionEdges({ ...huge, width: 1774, height: 1183 })).toEqual([]);
    expect(missingRenditionEdges({ ...huge, dataUrl: "data:image/png;base64,QQ==" })).toEqual([]);
    expect(missingRenditionEdges({ ...huge, width: undefined })).toEqual([]);
    expect(missingRenditionEdges({ ...huge, version: undefined })).toEqual([]);
  });
});

describe("ensureLayoutImageRenditions", () => {
  it("fetches the original once and sends each missing copy for this version", async () => {
    const d = deps();
    await ensureLayoutImageRenditions(huge, d);
    expect(d.fetchBlob).toHaveBeenCalledTimes(1);
    expect(d.posted.map((m) => [m.type, m.imageId, m.version, m.edge])).toEqual([
      ["saveLayoutImageRendition", "huge_1", "9", 1024],
      ["saveLayoutImageRendition", "huge_1", "9", 2048],
      ["saveLayoutImageRendition", "huge_1", "9", 4096],
    ]);
    expect(typeof d.posted[0].data).toBe("string");
  });

  it("does not start again for the same version, but does for a new one", async () => {
    const d = deps();
    await ensureLayoutImageRenditions(huge, d);
    await ensureLayoutImageRenditions(huge, d);
    expect(d.posted).toHaveLength(3);
    await ensureLayoutImageRenditions({ ...huge, version: "10" }, d);
    expect(d.posted).toHaveLength(6);
  });

  it("gives up quietly when the original cannot be fetched or the browser cannot resample", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    const failing = { ...deps(), fetchBlob: vi.fn(async () => { throw new Error("404"); }) };
    await ensureLayoutImageRenditions(huge, failing);
    expect(failing.posted).toEqual([]);
    expect(warn).toHaveBeenCalled();

    const noCanvas = { ...deps(), make: vi.fn(async () => null) };
    await ensureLayoutImageRenditions({ ...huge, version: "11" }, noCanvas);
    expect(noCanvas.posted).toEqual([]);
    warn.mockRestore();
  });
});

describe("drawing from the copies", () => {
  const withCopies: LayoutImageRef = {
    ...huge,
    renditions: [
      { edge: 1024, width: 1024, height: 683, url: "layout-images/rendition/huge_1/1024?v=9" },
      { edge: 2048, width: 2048, height: 1365, url: "layout-images/rendition/huge_1/2048?v=9" },
    ],
  };
  const layout = (extra: Partial<EffectLayout> = {}): EffectLayout => ({
    effectType: "fx",
    version: 1,
    dimensions: { width: 600, height: 400 },
    backgrounds: [{ layerIndex: 0, type: "image", value: "huge_1", size: "cover" }],
    controls: [],
    textLabels: [],
    ...extra,
  });
  const node = { id: "n", type: "fx", params: {} } as unknown as GraphNode;

  it("works out how wide a background is drawn at the layout's largest", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [withCopies] };
    expect(backgroundImageDrawnWidth(layout().backgrounds[0], layout())).toBe(600);
    expect(backgroundImageDrawnWidth(layout().backgrounds[0], layout({ scaling: { maxScale: 2 } }))).toBe(1200);
  });

  it("draws a large background from the smallest copy wide enough, and grows the need with maxScale", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [withCopies] };
    expect(renderCustomLayout(node, layout(), [])).toContain("layout-images/rendition/huge_1/1024?v=9");
    expect(renderCustomLayout(node, layout({ scaling: { maxScale: 2 } }), [])).toContain("layout-images/rendition/huge_1/2048?v=9");
    expect(renderCustomLayout(node, layout({ scaling: { maxScale: 4 } }), [])).toContain("layout-images/id/huge_1?v=9");
  });

  it("draws a large image without copies from its own file, and starts making them", async () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [{ ...huge }] };
    // jsdom cannot fetch the app's URLs, so making them fails here, and says so.
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    expect(resolveLayoutImageUrl("huge_1", 600)).toBe("layout-images/id/huge_1?v=9");
    await vi.waitFor(() => expect(warn).toHaveBeenCalledWith(expect.stringContaining("huge_1"), expect.anything()));
    warn.mockRestore();
  });

  it("adds a kept copy to the library in edge order", () => {
    uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [{ ...huge }] };
    onLayoutImageRenditionSaved({ type: "layoutImageRenditionSaved", imageId: "huge_1", edge: 2048, width: 2048, height: 1365, url: "u2" });
    onLayoutImageRenditionSaved({ type: "layoutImageRenditionSaved", imageId: "huge_1", edge: 1024, width: 1024, height: 683, url: "u1" });
    expect(uiState.layoutLibrary.images[0].renditions?.map((r) => r.edge)).toEqual([1024, 2048]);
  });
});
