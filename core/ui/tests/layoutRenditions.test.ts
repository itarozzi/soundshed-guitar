import { describe, expect, it } from "vitest";
import { backgroundDrawnWidth, chooseRenditionUrl, renditionEdgesFor, type RenditionSource } from "../ts/layoutRenditions.js";

const big: RenditionSource = {
  url: "layout-images/id/bg?v=1",
  width: 6000,
  height: 4000,
  renditions: [
    { edge: 1024, width: 1024, height: 683, url: "layout-images/id/bg%401024?v=1" },
    { edge: 2048, width: 2048, height: 1365, url: "layout-images/id/bg%402048?v=1" },
    { edge: 4096, width: 4096, height: 2731, url: "layout-images/id/bg%404096?v=1" },
  ],
};

describe("renditionEdgesFor", () => {
  it("makes only copies smaller than the original", () => {
    expect(renditionEdgesFor(6000, 4000)).toEqual([1024, 2048, 4096]);
    expect(renditionEdgesFor(1500, 3000)).toEqual([1024, 2048]);
    expect(renditionEdgesFor(2049, 100)).toEqual([1024, 2048]);
  });

  it("makes none for an image no longer than 2048 px, which is drawn from its own file as before", () => {
    expect(renditionEdgesFor(1024, 768)).toEqual([]);
    expect(renditionEdgesFor(2048, 1536)).toEqual([]);
    expect(renditionEdgesFor(1774, 1183)).toEqual([]);
  });
});

describe("chooseRenditionUrl", () => {
  it("picks the smallest copy with a pixel for every device pixel", () => {
    expect(chooseRenditionUrl(big, 480, 1)).toBe(big.renditions![0].url);
    expect(chooseRenditionUrl(big, 600, 2)).toBe(big.renditions![1].url);
    expect(chooseRenditionUrl(big, 1600, 2)).toBe(big.renditions![2].url);
  });

  it("falls back to the original when no copy is wide enough", () => {
    expect(chooseRenditionUrl(big, 2500, 2)).toBe(big.url);
  });

  it("draws the original for an image with no copies, as every layout image was before", () => {
    expect(chooseRenditionUrl({ url: "layout-images/id/x?v=1", width: 800, height: 600 }, 200, 1)).toBe("layout-images/id/x?v=1");
    expect(chooseRenditionUrl({ url: "layout-images/id/x?v=1" }, 200, 1)).toBe("layout-images/id/x?v=1");
  });

  it("keeps a data URL, which is the image itself", () => {
    expect(chooseRenditionUrl({ ...big, dataUrl: "data:image/png;base64,QQ==" }, 300, 1)).toBe("data:image/png;base64,QQ==");
  });

  it("never treats a pixel ratio below 1 as needing fewer pixels than CSS pixels", () => {
    expect(chooseRenditionUrl(big, 1500, 0.5)).toBe(big.renditions![1].url);
  });
});

describe("backgroundDrawnWidth", () => {
  it("follows each background size mode", () => {
    // A 2000 x 1000 image in an 800 x 600 layer.
    expect(backgroundDrawnWidth("cover", undefined, 800, 600, 2000, 1000)).toBe(1200);
    expect(backgroundDrawnWidth(undefined, undefined, 800, 600, 2000, 1000)).toBe(1200);
    expect(backgroundDrawnWidth("contain", undefined, 800, 600, 2000, 1000)).toBe(800);
    expect(backgroundDrawnWidth("stretch", undefined, 800, 600, 2000, 1000)).toBe(800);
    expect(backgroundDrawnWidth("custom", 0.5, 800, 600, 2000, 1000)).toBe(400);
    expect(backgroundDrawnWidth("tile", undefined, 800, 600, 2000, 1000)).toBe(2000);
  });
});
