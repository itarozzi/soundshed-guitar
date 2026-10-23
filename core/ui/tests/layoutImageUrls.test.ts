import { afterEach, describe, expect, it } from "vitest";
import { resolveLayoutImageUrl } from "../ts/layoutImages.js";
import { getLayoutImageUrl } from "../ts/layoutDesigner/images.js";
import type { LayoutImageRef } from "../ts/layoutTypes.js";
import { uiState } from "../ts/state.js";

function useImages(images: LayoutImageRef[]): void {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images };
}

afterEach(() => {
  useImages([]);
});

describe("resolveLayoutImageUrl", () => {
  it("loads an image from the URL the engine lists it by", () => {
    useImages([{ imageId: "bg_1", fileName: "bg_1.png", url: "layout-images/id/bg_1?v=7" }]);
    expect(resolveLayoutImageUrl("bg_1")).toBe("layout-images/id/bg_1?v=7");
  });

  it("prefers a data URL, which is the image itself (imported this session, or an older entry)", () => {
    useImages([{ imageId: "bg_1", fileName: "bg_1.png", url: "layout-images/id/bg_1?v=7", dataUrl: "data:image/png;base64,QQ==" }]);
    expect(resolveLayoutImageUrl("bg_1")).toBe("data:image/png;base64,QQ==");
  });

  it("is null for an id the library does not hold, or holds with nowhere to load it from", () => {
    useImages([{ imageId: "named_only", fileName: "named_only.png" }]);
    expect(resolveLayoutImageUrl("missing")).toBeNull();
    expect(resolveLayoutImageUrl("named_only")).toBeNull();
  });

  it("is what the designer draws with, so it and the params panel draw the same file", () => {
    useImages([{ imageId: "knob_1", fileName: "knob_1.png", url: "layout-images/id/knob_1?v=2" }]);
    expect(getLayoutImageUrl("knob_1")).toBe(resolveLayoutImageUrl("knob_1"));
  });
});
