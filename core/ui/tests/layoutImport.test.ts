import { describe, expect, it } from "vitest";
import {
  importedImageId,
  layoutImageExtension,
  normalizeImportedLayout,
  prepareLayoutImport,
} from "../ts/layoutDesigner/layoutImport.js";
import { nextFreeBackgroundLayer, normalizeBackgroundLayers } from "../ts/layoutTypes.js";
import type { EffectLayout, LayoutBackground } from "../ts/layoutTypes.js";

const PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).buffer;

function archiveWith(layout: Record<string, unknown>, images: unknown[]): Record<string, unknown> {
  return { formatVersion: 1, layout, images };
}

const target = { layoutId: "layout-open", effectType: "delay_digital", name: "Open layout", createdAt: "2026-01-01T00:00:00.000Z" };

describe("layoutImageExtension", () => {
  it("accepts a plain image file name in any case", () => {
    expect(layoutImageExtension("Background.PNG")).toBe("png");
    expect(layoutImageExtension("knob.face.jpeg")).toBe("jpeg");
  });

  it("refuses names that lead out of the folder, or are not images", () => {
    for (const name of ["..\\..\\Startup\\x.png", "../x.png", "C:x.png", "C:/x.png", "a/b.png", "x.bat", ".png", "..", ""]) {
      expect(layoutImageExtension(name)).toBeNull();
    }
    expect(layoutImageExtension(42)).toBeNull();
  });
});

describe("importedImageId", () => {
  it("keeps only plain characters from the archive's name", () => {
    expect(importedImageId("My Pedal (v2).png", "7")).toBe("My_Pedal_v2_7");
    expect(importedImageId("¡¡¡.png", "7")).toBe("image_7");
  });
});

describe("normalizeImportedLayout", () => {
  it("is null for something that is not a layout", () => {
    expect(normalizeImportedLayout(null)).toBeNull();
    expect(normalizeImportedLayout([1, 2])).toBeNull();
  });

  it("fills what is missing and drops what has the wrong shape", () => {
    const layout = normalizeImportedLayout({
      effectType: "delay_digital",
      controls: [
        { paramKey: "time", type: "knob", position: { x: 10, y: 10 } },
        { paramKey: "mix" },
        "nonsense",
      ],
      textLabels: [{ id: "l1", text: "Hi", position: { x: 1, y: 2 } }, { id: "l2" }],
      backgrounds: [{ layerIndex: 0, type: "image", value: "bg" }, { type: "script", value: "x" }],
    });
    expect(layout).not.toBeNull();
    expect(layout!.controls.map((control) => control.paramKey)).toEqual(["time"]);
    expect(layout!.textLabels).toHaveLength(1);
    expect(layout!.textLabels[0].fontSize).toBe(14);
    expect(layout!.backgrounds).toHaveLength(1);
    expect(layout!.overlays).toEqual([]);
    expect(layout!.dimensions).toEqual({ width: 400, height: 280 });
  });

  it("gives every field the designer and renderer put into markup or CSS its type", () => {
    // Each of these used to reach an attribute or a style as it stood: the scale as
    // value="${scaleX}" in the designer and as transform: scale(...) in the panel.
    const markup = `1"><iframe src="https://example.com">`;
    const css = "1);background-image:url(https://example.com/x.png";
    const layout = normalizeImportedLayout({
      effectType: "delay_digital",
      defaultControlsScale: { x: markup, y: css },
      defaultControlsOffset: { x: css, y: 4 },
      containerTheme: "dark evil",
      author: { name: "not a string" },
      backgrounds: [{ layerIndex: 0, type: "color", value: "#000", offsetX: css, offsetY: markup, scale: css, opacity: css }],
      overlays: [
        { id: "o1", position: { x: 0, y: 0 }, size: { width: 8, height: 8 }, style: { borderWidth: css, borderRadius: markup } },
        { id: "o2", position: { x: 0, y: 0 }, size: { width: 8, height: 8 }, style: "nonsense" },
      ],
      controls: [{ paramKey: "time", type: "knob", position: { x: 0, y: 0 }, style: "nonsense", labelOverride: 5 }],
      textLabels: [{ id: "l1", text: "Hi", position: { x: 0, y: 0 }, fontFamily: 12, color: {} }],
    })!;

    expect(layout.defaultControlsScale).toEqual({ x: 1, y: 1 });
    expect(layout.defaultControlsOffset).toEqual({ x: 0, y: 4 });
    expect(layout.containerTheme).toBeUndefined();
    expect(layout.author).toBeUndefined();
    expect(layout.backgrounds[0]).toMatchObject({ offsetX: undefined, offsetY: undefined, scale: undefined, opacity: undefined });
    expect(layout.overlays![0].style).toMatchObject({ borderWidth: undefined, borderRadius: undefined });
    expect(layout.overlays![1].style).toBeUndefined();
    expect(layout.controls[0].style).toBeUndefined();
    expect(layout.controls[0].labelOverride).toBeUndefined();
    expect(layout.textLabels[0]).toMatchObject({ fontFamily: undefined, color: undefined });
    expect(normalizeImportedLayout({ effectType: "x", containerTheme: "classic" })!.containerTheme).toBe("classic");
  });
});

describe("prepareLayoutImport", () => {
  it("renames the images, repoints the layout at them and takes the open layout's identity", async () => {
    const archive = archiveWith(
      {
        effectType: "reverb_room",
        layoutId: "someone-elses-layout",
        blendId: "their-blend",
        name: "Shared Face",
        thumbnailDataUrl: "data:image/jpeg;base64,AAAA",
        backgrounds: [{ layerIndex: 0, type: "image", value: "bg" }],
        controls: [
          { paramKey: "time", type: "knob", position: { x: 0, y: 0 }, style: { knobStyle: "custom", knobImageId: "bg" } },
          { paramKey: "not_a_param", type: "knob", position: { x: 8, y: 0 } },
        ],
        textLabels: [],
      },
      [
        { imageId: "evil", fileName: "..\\..\\..\\Startup\\evil.png" },
        { imageId: "bg", fileName: "bg.png", type: "background" },
        { imageId: "gone", fileName: "gone.png" },
      ],
    );
    const reads: string[] = [];

    const prepared = await prepareLayoutImport(
      archive,
      async (path) => {
        reads.push(path);
        return path === "images/bg.png" ? PNG : null;
      },
      target,
      new Set(["time", "feedback"]),
    );

    if ("error" in prepared) throw new Error(prepared.error);
    expect(reads).toEqual(["images/bg.png", "images/gone.png"]);
    expect(prepared.images).toHaveLength(1);
    const [image] = prepared.images;
    expect(image.imageId).toMatch(/^bg_\d+$/);
    expect(image.fileName).toBe(`${image.imageId}.png`);
    expect(image.dataUrl.startsWith("data:image/png;base64,")).toBe(true);
    expect(image.type).toBe("background");
    expect(prepared.skippedImages).toEqual(["..\\..\\..\\Startup\\evil.png", "gone.png"]);

    const { layout } = prepared;
    expect(layout.layoutId).toBe("layout-open");
    expect(layout.effectType).toBe("delay_digital");
    expect(layout.blendId).toBeUndefined();
    expect(layout.name).toBe("Shared Face");
    expect(layout.createdAt).toBe(target.createdAt);
    expect(layout.thumbnailDataUrl).toBeUndefined();
    expect(layout.backgrounds[0].value).toBe(image.imageId);
    expect(layout.controls[0].style?.knobImageId).toBe(image.imageId);
    expect(prepared.unmatchedControls).toBe(1);
  });

  it("keeps the open layout's blend and name when the archive has none", async () => {
    const prepared = await prepareLayoutImport(
      archiveWith({ effectType: "amp_nam_blend", backgrounds: [], controls: [], textLabels: [] }, []),
      async () => null,
      { ...target, blendId: "blend-1" },
      new Set(),
    );
    if ("error" in prepared) throw new Error(prepared.error);
    expect(prepared.layout.blendId).toBe("blend-1");
    expect(prepared.layout.name).toBe("Open layout");
  });

  it("reports an archive with no layout", async () => {
    expect(await prepareLayoutImport({ images: [] }, async () => null, target, new Set())).toEqual({
      error: "the archive has no layout data",
    });
    expect(await prepareLayoutImport("nope", async () => null, target, new Set())).toHaveProperty("error");
  });
});

describe("background layers", () => {
  const bg = (layerIndex: number, value = "#000"): LayoutBackground => ({ layerIndex, type: "color", value });

  it("finds the lowest free layer, not the count", () => {
    expect(nextFreeBackgroundLayer([])).toBe(0);
    // The bottom layer was deleted: index 1 is still taken, so the new layer goes under it.
    expect(nextFreeBackgroundLayer([bg(1)])).toBe(0);
    expect(nextFreeBackgroundLayer([bg(0)])).toBe(1);
    expect(nextFreeBackgroundLayer([bg(0), bg(1)])).toBeNull();
  });

  it("separates layers that share an index, keeping their order", () => {
    const layout = { backgrounds: [bg(1, "#111"), bg(1, "#222")] } as EffectLayout;
    normalizeBackgroundLayers(layout);
    expect(layout.backgrounds.map((layer) => [layer.layerIndex, layer.value])).toEqual([[0, "#111"], [1, "#222"]]);
  });

  it("leaves valid layers alone", () => {
    const backgrounds = [bg(1, "#111")];
    const layout = { backgrounds } as EffectLayout;
    normalizeBackgroundLayers(layout);
    expect(layout.backgrounds).toBe(backgrounds);
  });
});
