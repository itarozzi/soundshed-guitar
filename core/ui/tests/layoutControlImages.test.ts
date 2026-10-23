import { describe, expect, it } from "vitest";
import {
  imageKnobMarkup,
  imageSliderStyle,
  imageToggleChildren,
  knobDiameterStyle,
  layoutSize,
  usableKnobImage,
  type ImageCssResolver,
} from "../ts/layoutControlImages.js";

const resolve: ImageCssResolver = (id) => (id.startsWith("missing") ? null : `url("layout-images/id/${id}?v=1")`);

function parse(markup: { attributes: string; style: string; children: string }): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = `<div class="knob"${markup.attributes} style="${markup.style}">${markup.children}</div>`;
  return host.firstElementChild as HTMLElement;
}

describe("usableKnobImage", () => {
  it("accepts a rotating face, or a filmstrip with a usable frame count", () => {
    expect(usableKnobImage({ mode: "rotate", imageId: "cap" })).not.toBeNull();
    expect(usableKnobImage({ mode: "filmstrip", imageId: "strip", frames: 64 })).not.toBeNull();
  });

  it("refuses anything else, so the knob draws as it always did", () => {
    for (const odd of [null, "x", {}, { mode: "rotate" }, { mode: "spin", imageId: "x" }, { mode: "filmstrip", imageId: "x" }, { mode: "filmstrip", imageId: "x", frames: 1 }]) {
      expect(usableKnobImage(odd)).toBeNull();
    }
  });
});

describe("imageKnobMarkup", () => {
  it("draws a filmstrip at the frame for the knob's position, at its diameter", () => {
    const knob = parse(imageKnobMarkup({ mode: "filmstrip", imageId: "strip", frames: 5 }, 96, 0.5, resolve)!);
    expect(knob.dataset).toMatchObject({ customKnobImage: "true", knobImage: "filmstrip", frames: "5", columns: "1" });
    expect(knob.style.width).toBe("96px");
    const face = knob.querySelector<HTMLElement>(".custom-knob-face.is-filmstrip")!;
    expect(face.style.backgroundSize).toBe("100% 500%");
    expect(face.style.backgroundPosition).toBe("0% 50%");
  });

  it("turns a face over its own range, with a still base and glare", () => {
    const knob = parse(imageKnobMarkup({ mode: "rotate", imageId: "cap", minAngle: -150, maxAngle: 150, baseImageId: "skirt", glareImageId: "glare", clip: false }, 64, 1, resolve)!);
    expect(knob.dataset).toMatchObject({ knobImage: "rotate", minAngle: "-150", maxAngle: "150" });
    expect(knob.querySelector<HTMLElement>(".custom-knob-face")!.style.transform).toBe("rotate(150deg)");
    expect(knob.querySelector(".custom-knob-face")!.classList.contains("no-clip")).toBe(true);
    expect([...knob.children].map((c) => c.className)).toEqual(["custom-knob-base", "custom-knob-face no-clip", "custom-knob-glare"]);
  });

  it("draws nothing when the face image cannot be drawn, and skips a base or glare that cannot", () => {
    expect(imageKnobMarkup({ mode: "rotate", imageId: "missing-cap" }, 38, 0, resolve)).toBeNull();
    const knob = parse(imageKnobMarkup({ mode: "rotate", imageId: "cap", baseImageId: "missing-base" }, 38, 0, resolve)!);
    expect(knob.querySelector(".custom-knob-base")).toBeNull();
  });
});

describe("control sizes", () => {
  it("keeps the original 38 px knob when no diameter is set", () => {
    expect(knobDiameterStyle(38)).toBe("");
    expect(knobDiameterStyle(76)).toBe(" zoom: 2;");
  });

  it("clamps sizes from a layout and ignores what is not a number", () => {
    expect(layoutSize(500, 16, 256)).toBe(256);
    expect(layoutSize(12.4, 16, 256)).toBe(16);
    expect(layoutSize("40", 16, 256)).toBeUndefined();
  });
});

describe("image toggles and sliders", () => {
  it("draws a toggle's on and off pictures only when it has both", () => {
    const host = document.createElement("label");
    host.innerHTML = imageToggleChildren({ onImageId: "led-on", offImageId: "led-off" }, 48, resolve);
    expect([...host.children].map((c) => c.className)).toEqual(["custom-toggle-image custom-toggle-off", "custom-toggle-image custom-toggle-on"]);
    expect(imageToggleChildren({ onImageId: "led-on" }, 48, resolve)).toBe("");
    expect(imageToggleChildren(undefined, 48, resolve)).toBe("");
  });

  it("gives a slider absolute image URLs, which a custom property needs", () => {
    const style = imageSliderStyle({ trackImageId: "rail", thumbImageId: "cap" }, 160, resolve);
    expect(style).toContain(`--slider-track-image: url("${new URL("layout-images/id/rail?v=1", document.baseURI).href}")`);
    expect(style).toContain("--slider-thumb-image:");
    expect(imageSliderStyle({ trackImageId: "missing" }, 160, resolve)).toBe("");
  });
});
