/**
 * Layouts made before the high-resolution work (docs/plans/layout-designer-hires.md) must keep
 * rendering exactly as they do now. Each fixture in tests/fixtures/layout-compat is a layout as
 * it is stored or exported, and its rendered markup is pinned in __snapshots__/layout-compat.
 *
 * The markup is compared in a canonical form: elements, attributes in name order, each inline
 * style's declarations in source order with whitespace collapsed, and text. Formatting the
 * renderer's templates differently changes nothing; changing what a layout draws does. Image URLs
 * are written as the image id they resolve, so serving images another way leaves the snapshots
 * alone so long as each id still draws the same image.
 *
 * A snapshot changes only when a layout really is meant to render differently. Say why in the
 * commit; for an old layout the answer should almost always be that it is not.
 */

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { afterEach, beforeAll, describe, expect, it, vi } from "vitest";
import type { BlendParamDef } from "../ts/blendUtils.js";
import { prepareLayoutImport } from "../ts/layoutDesigner/layoutImport.js";
import {
  renderCustomLayout,
  renderCustomLayoutBackdrop,
  type LayoutResourceControlDef,
} from "../ts/layoutRenderer.js";
import type { EffectLayout, LayoutImageRef } from "../ts/layoutTypes.js";
import { applyCustomLayoutScaling } from "../ts/signalPath/paramsPanel/layoutOverlay.js";
import { uiState } from "../ts/state.js";
import type { GraphNode } from "../ts/types.js";

interface Fixture {
  description: string;
  kind: "stored" | "archive";
  images: LayoutImageRef[];
  data: unknown;
}

const FIXTURE_DIR = join(__dirname, "fixtures", "layout-compat");
const SNAPSHOT_DIR = "./__snapshots__/layout-compat";

const FIXTURES = [
  "export-1-6-0",
  "control-size-and-styles",
  "background-image-modes",
  "image-assets-data-urls",
  "backdrop-offset-scale",
  "resource-selector",
];

const PARAM_DEFS: BlendParamDef[] = [
  { key: "time", name: "Time", default: 350, min: 1, max: 2000, unit: "ms", taper: "log" },
  { key: "feedback", name: "Feedback", default: 0.4, min: 0, max: 1, unit: "amount" },
  { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, step: 0.01, unit: "amount" },
  { key: "sync", name: "Sync", default: 0, min: 0, max: 1, unit: "toggle" },
  { key: "mode", name: "Mode", default: 1, min: 0, max: 2, step: 1, unit: "enum", labels: ["Digital", "Tape", "Analog"] },
  { key: "level", name: "Level", default: 0, min: -24, max: 12, unit: "dB" },
  { key: "cutoff", name: "Cutoff", default: 4000, min: 20, max: 20000, unit: "Hz", taper: "log" },
];

const NODE_PARAMS = { time: 500, feedback: 0.35, mix: 0.7, sync: 1, mode: 2, level: -3.5, cutoff: 1200 };

const RESOURCES: LayoutResourceControlDef[] = [
  {
    resourceControlKey: "__resource__:nam:0",
    displayName: "Model",
    resourceType: "nam",
    resourceIndex: 0,
    currentResourceId: "nam-fixture-1",
    currentDisplayName: "Plexi Crunch",
    currentFilePath: "D:/NAM/plexi crunch.nam",
  },
  {
    resourceControlKey: "__resource__:ir:1",
    displayName: "IR",
    resourceType: "ir",
    resourceIndex: 1,
    currentDisplayName: "",
    isOptional: true,
    allowBrowseFile: false,
  },
];

const DEFAULT_CONTROLS_STUB = `<div class="default-controls-stub">default controls</div>`;

function readFixture(name: string): Fixture {
  return JSON.parse(readFileSync(join(FIXTURE_DIR, `${name}.json`), "utf8")) as Fixture;
}

function node(bypassed: boolean): GraphNode {
  return { id: "node-fixture", type: "delay_digital", params: { ...NODE_PARAMS }, bypassed } as unknown as GraphNode;
}

function useLibraryImages(images: LayoutImageRef[]): void {
  uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: images.map((image) => ({ ...image })) };
}

/**
 * Collapse a style attribute's declarations, writing each layout image URL as the id it
 * resolves. Other URLs (the app's own icons) are kept as they are.
 */
function canonicalStyle(style: string, urlToId: Map<string, string>): string {
  const withIds = style.replace(/url\((["']?)(.*?)\1\)/g, (match, _quote, url: string) => {
    if (!/^(data:|blob:|layout-images\/)/.test(url)) return match;
    const id = urlToId.get(url);
    return `url(<image:${id ?? `unresolved ${url.slice(0, 48)}`}>)`;
  });
  return withIds
    .split(";")
    .map((declaration) => declaration.replace(/\s+/g, " ").trim())
    .filter(Boolean)
    .join("; ");
}

function canonicalMarkup(html: string, images: LayoutImageRef[]): string {
  const urlToId = new Map<string, string>();
  for (const image of images) {
    if (image.dataUrl) urlToId.set(image.dataUrl, image.imageId);
    if (image.fileName) urlToId.set(`layout-images/${image.fileName}`, image.imageId);
  }

  const host = document.createElement("div");
  host.innerHTML = html;
  const lines: string[] = [];
  const walk = (element: Element, depth: number): void => {
    const indent = "  ".repeat(depth);
    const attributes = [...element.attributes]
      .filter((attribute) => attribute.name !== "style")
      .map((attribute) => `${attribute.name}="${attribute.value.replace(/\s+/g, " ").trim()}"`)
      .sort();
    const style = element.getAttribute("style");
    const styleText = style ? ` {${canonicalStyle(style, urlToId)}}` : "";
    lines.push(`${indent}<${element.tagName.toLowerCase()}${attributes.length ? ` ${attributes.join(" ")}` : ""}>${styleText}`);
    for (const child of element.childNodes) {
      if (child.nodeType === Node.ELEMENT_NODE) {
        walk(child as Element, depth + 1);
      } else if (child.nodeType === Node.TEXT_NODE && child.textContent?.trim()) {
        lines.push(`${indent}  "${child.textContent.replace(/\s+/g, " ").trim()}"`);
      }
    }
  };
  for (const child of host.children) walk(child, 0);
  return `${lines.join("\n")}\n`;
}

/** The layout as the params panel draws it, enabled and then bypassed (overlays depend on it). */
function renderBothStates(layout: EffectLayout, images: LayoutImageRef[], description: string): string {
  useLibraryImages(images);
  const sections = [`# ${description}`];
  for (const bypassed of [false, true]) {
    const html = layout.useDefaultControls
      ? renderCustomLayoutBackdrop(node(bypassed), structuredClone(layout), DEFAULT_CONTROLS_STUB)
      : renderCustomLayout(node(bypassed), structuredClone(layout), PARAM_DEFS, RESOURCES);
    sections.push(`## ${bypassed ? "bypassed" : "enabled"}`, canonicalMarkup(html, images));
  }
  return sections.join("\n");
}

beforeAll(() => {
  // The designer posts requestLayoutImages for an id the library lacks; nothing answers here.
  window.IPlugSendMsg = undefined;
});

afterEach(() => {
  vi.useRealTimers();
});

describe("layouts made before the high-resolution work render as they always have", () => {
  for (const name of FIXTURES) {
    it(`${name} (as stored)`, async () => {
      const fixture = readFixture(name);
      const layout = (fixture.kind === "archive" ? (fixture.data as { layout: unknown }).layout : fixture.data) as EffectLayout;
      await expect(renderBothStates(layout, fixture.images, fixture.description)).toMatchFileSnapshot(
        `${SNAPSHOT_DIR}/${name}.txt`,
      );
    });
  }

  it("the factory Chaos Engine layout", async () => {
    const archive = JSON.parse(
      readFileSync(join(__dirname, "..", "assets", "layouts", "chaos-engine", "layout.json"), "utf8"),
    ) as { layout: EffectLayout; images: Array<{ imageId: string; fileName: string }> };
    const images = archive.images.map((image) => ({
      imageId: image.imageId,
      fileName: image.fileName,
      dataUrl: `data:image/png;base64,${btoa(image.imageId)}`,
    }));
    await expect(renderBothStates(archive.layout, images, "Factory layout chaos-engine, as bundled")).toMatchFileSnapshot(
      `${SNAPSHOT_DIR}/factory-chaos-engine.txt`,
    );
  });

  it("a 1.6.0 export imports to the same layout and renders the same", async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date("2026-10-23T12:00:00.000Z"));
    const fixture = readFixture("export-1-6-0");
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).buffer;
    const prepared = await prepareLayoutImport(
      fixture.data,
      async (path) => (path.startsWith("images/") ? png : null),
      { layoutId: "layout-open", effectType: "delay_digital", name: "Open layout", createdAt: "2026-10-01T10:00:00.000Z" },
      new Set(PARAM_DEFS.map((def) => def.key)),
    );
    if ("error" in prepared) throw new Error(prepared.error);

    const images = prepared.images.map(({ imageId, fileName, dataUrl, type }) => ({ imageId, fileName, dataUrl, type }));
    const record = [
      "# The layout prepareLayoutImport makes of the 1.6.0 archive",
      JSON.stringify({ layout: prepared.layout, images: images.map(({ dataUrl: _d, ...rest }) => rest), skippedImages: prepared.skippedImages, unmatchedControls: prepared.unmatchedControls }, null, 2),
      renderBothStates(prepared.layout, images, "Rendered after import"),
    ].join("\n");
    await expect(record).toMatchFileSnapshot(`${SNAPSHOT_DIR}/export-1-6-0-imported.txt`);
  });
});

describe("a layout scales to the panel as it always has", () => {
  function scaledAt(panelWidth: number): { transform: string; marginLeft: string; height: string } {
    const container = document.createElement("div");
    const layout: EffectLayout = {
      effectType: "delay_digital",
      version: 1,
      dimensions: { width: 480, height: 320 },
      backgrounds: [],
      controls: [],
      textLabels: [],
    };
    container.innerHTML = renderCustomLayout(node(false), layout, PARAM_DEFS);
    const outer = container.querySelector<HTMLElement>(".custom-layout-scale-outer")!;
    const inner = container.querySelector<HTMLElement>(".custom-layout-container")!;
    Object.defineProperty(outer, "offsetWidth", { configurable: true, value: panelWidth });
    applyCustomLayoutScaling(container);
    return { transform: inner.style.transform, marginLeft: inner.style.marginLeft, height: outer.style.height };
  }

  beforeAll(() => {
    globalThis.ResizeObserver ??= class {
      observe(): void {}
      unobserve(): void {}
      disconnect(): void {}
    } as unknown as typeof ResizeObserver;
  });

  it("draws at its own size, centred, when the panel is as wide or wider; it never grows", () => {
    expect(scaledAt(1200)).toEqual({ transform: "", marginLeft: "auto", height: "320px" });
    expect(scaledAt(480)).toEqual({ transform: "", marginLeft: "auto", height: "320px" });
  });

  it("shrinks as one piece to the panel's width when the panel is narrower, whatever its height", () => {
    expect(scaledAt(479)).toEqual({ transform: `scale(${479 / 480})`, marginLeft: "0px", height: `${Math.round(320 * (479 / 480))}px` });
    expect(scaledAt(240)).toEqual({ transform: "scale(0.5)", marginLeft: "0px", height: "160px" });
    expect(scaledAt(100)).toEqual({ transform: `scale(${100 / 480})`, marginLeft: "0px", height: `${Math.round(320 * (100 / 480))}px` });
  });

  it("leaves the layout alone when the panel has no width yet", () => {
    expect(scaledAt(0)).toEqual({ transform: "", marginLeft: "0px", height: "320px" });
  });
});
