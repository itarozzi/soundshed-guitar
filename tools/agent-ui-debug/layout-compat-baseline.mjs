#!/usr/bin/env node
/**
 * layout-compat-baseline.mjs — screenshots of the layout compatibility fixtures, drawn by
 * the running Standalone's own WebView2, CSS and renderer, and a pixel comparison against
 * an earlier set.
 *
 * The jsdom snapshots in core/ui/tests/layoutCompat.test.ts pin the markup a layout renders
 * to; these pin what that markup looks like in the real browser engine with the real
 * stylesheets, at several panel widths and at 1x and 2x pixel density. Together they are the
 * baseline every phase of docs/plans/layout-designer-hires.md is checked against: a layout
 * made before that work must look the same after it.
 *
 * Nothing is saved to the profile. Each fixture is rendered with the app's own modules
 * (layoutRenderer, GenericKnob, applyCustomLayoutScaling) into a temporary overlay inside the
 * same wrappers the params panel uses, with a temporary image library; both are removed and
 * the real library put back when the run ends. Images are drawn on a canvas from each image
 * id (a labelled grid for backgrounds, a pointer knob for knob faces, SVG for .svg names), so
 * scaling, cropping, tiling, offsets and rotation are all visible. The factory layout uses its
 * real bundled image.
 *
 * What it does not cover: how the app finds an image on disk. Phase 1 changes that, so the
 * script supplies images itself; if a later phase changes how the renderer asks for images,
 * update supplyImages() to match and say so in the plan.
 *
 * Usage (app launched with WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333):
 *   node tools/agent-ui-debug/layout-compat-baseline.mjs --port 9333 --out <dir>
 *   node tools/agent-ui-debug/layout-compat-baseline.mjs --port 9333 --out <dir> --compare <baselineDir>
 *
 * --out defaults to docs/plans/layout-designer-hires-baseline. With --compare, every capture
 * is compared with the file of the same name in <baselineDir>; differing pixels (any channel
 * more than --tolerance, default 2, apart) are counted and a diff image is written beside the
 * capture as <name>.diff.png. Exit code 1 when anything differs.
 *
 * Requires Node 22+ (built-in WebSocket and fetch).
 */

import { execSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const FIXTURE_DIR = join(REPO, "core", "ui", "tests", "fixtures", "layout-compat");
const FACTORY_LAYOUT = join(REPO, "core", "ui", "assets", "layouts", "chaos-engine", "layout.json");

const STORED_FIXTURES = [
  "export-1-6-0",
  "control-size-and-styles",
  "background-image-modes",
  "image-assets-data-urls",
  "backdrop-offset-scale",
  "resource-selector",
];
/** Fixtures whose overlays change with bypass, also shot bypassed. */
const BYPASS_FIXTURES = new Set(["export-1-6-0", "backdrop-offset-scale", "factory-chaos-engine"]);
/** Panel widths: narrower than every fixture, the smaller designs, the larger ones, wider than all. */
const WIDTHS_1X = [360, 520, 760, 1200];
const WIDTHS_2X = [360, 1200];

/** Same parameters and values as layoutCompat.test.ts, so the two baselines describe one state. */
const PARAM_DEFS = [
  { key: "time", name: "Time", default: 350, min: 1, max: 2000, unit: "ms", taper: "log" },
  { key: "feedback", name: "Feedback", default: 0.4, min: 0, max: 1, unit: "amount" },
  { key: "mix", name: "Mix", default: 0.5, min: 0, max: 1, step: 0.01, unit: "amount" },
  { key: "sync", name: "Sync", default: 0, min: 0, max: 1, unit: "toggle" },
  { key: "mode", name: "Mode", default: 1, min: 0, max: 2, step: 1, unit: "enum", labels: ["Digital", "Tape", "Analog"] },
  { key: "level", name: "Level", default: 0, min: -24, max: 12, unit: "dB" },
  { key: "cutoff", name: "Cutoff", default: 4000, min: 20, max: 20000, unit: "Hz", taper: "log" },
];
const NODE_PARAMS = { time: 500, feedback: 0.35, mix: 0.7, sync: 1, mode: 2, level: -3.5, cutoff: 1200 };
const RESOURCES = [
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
/** Stands in for the standard controls a backdrop layout draws over, so their placement shows. */
const DEFAULT_CONTROLS_STUB =
  `<div style="display:flex;align-items:center;justify-content:center;height:120px;margin:8px;` +
  `background:rgba(255,0,255,0.28);border:2px dashed #ff00ff;color:#fff;font:bold 14px Arial,sans-serif">` +
  `default controls stand-in</div>`;

function parseArgs(argv) {
  const out = { port: 9333, out: join(REPO, "docs", "plans", "layout-designer-hires-baseline"), compare: null, tolerance: 2 };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--port") out.port = Number(argv[++i]);
    else if (argv[i] === "--out") out.out = resolve(argv[++i]);
    else if (argv[i] === "--compare") out.compare = resolve(argv[++i]);
    else if (argv[i] === "--tolerance") out.tolerance = Number(argv[++i]);
  }
  return out;
}

function loadFixtures() {
  const fixtures = {};
  for (const name of STORED_FIXTURES) {
    const fixture = JSON.parse(readFileSync(join(FIXTURE_DIR, `${name}.json`), "utf8"));
    const layout = fixture.kind === "archive" ? fixture.data.layout : fixture.data;
    fixtures[name] = { layout, images: fixture.images, realImages: false };
  }
  // Each background layer fills the whole canvas, so in the full stack the cover layer hides
  // the tile, custom-scale, contain and stretch layers beneath it. Shoot every layer alone too.
  const modes = fixtures["background-image-modes"];
  modes.layout.backgrounds.forEach((background, index) => {
    fixtures[`background-image-modes-layer${index}`] = {
      layout: { ...modes.layout, backgrounds: [background] },
      images: modes.images,
      realImages: false,
      layerOnly: true,
    };
  });
  const factory = JSON.parse(readFileSync(FACTORY_LAYOUT, "utf8"));
  fixtures["factory-chaos-engine"] = {
    layout: factory.layout,
    images: factory.images.map((image) => ({ imageId: image.imageId, fileName: image.fileName })),
    realImages: true,
    realImageBase: "assets/layouts/chaos-engine/images/",
  };
  return fixtures;
}

async function findPageTarget(port) {
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = targets.find((t) => t.type === "page" && t.url && t.url !== "about:blank");
      if (page) return page;
    } catch {
      // Not listening yet.
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`No app page on port ${port} after 60 s`);
}

function connect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  let nextId = 1;
  const pending = new Map();
  ws.addEventListener("message", (event) => {
    const msg = JSON.parse(event.data.toString());
    if (msg.id && pending.has(msg.id)) {
      const { resolve: ok, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) reject(new Error(JSON.stringify(msg.error)));
      else ok(msg.result);
    }
  });
  const ready = new Promise((ok, reject) => {
    ws.addEventListener("open", ok);
    ws.addEventListener("error", reject);
  });
  const send = (method, params = {}) => {
    const id = nextId++;
    return new Promise((ok, reject) => {
      pending.set(id, { resolve: ok, reject });
      ws.send(JSON.stringify({ id, method, params }));
    });
  };
  return { ready, send, close: () => ws.close() };
}

/** Runs `fn(arg)` in the page and returns its (awaited) value. */
async function inPage(send, fn, arg) {
  const result = await send("Runtime.evaluate", {
    expression: `(${fn.toString()})(${JSON.stringify(arg)})`,
    awaitPromise: true,
    returnByValue: true,
  });
  if (result.exceptionDetails) {
    throw new Error(`In page: ${result.exceptionDetails.exception?.description ?? result.exceptionDetails.text}`);
  }
  return result.result.value;
}

// ---- In-page functions (serialised with toString, so they must be self-contained) ----

/** Loads the app's modules, keeps the real library aside, and builds every fixture's images. */
async function pageSetup({ fixtures }) {
  const load = (path) => import(new URL(path, location.href).href);
  const [renderer, knob, overlay, state] = await Promise.all([
    load("dist/layoutRenderer.js"),
    load("dist/knob.js"),
    load("dist/signalPath/paramsPanel/layoutOverlay.js"),
    load("dist/state.js"),
  ]);

  const hue = (id) => [...id].reduce((h, c) => (h * 31 + c.charCodeAt(0)) % 360, 7);
  const background = (id) => {
    const canvas = document.createElement("canvas");
    canvas.width = 800;
    canvas.height = 500;
    const g = canvas.getContext("2d");
    const h = hue(id);
    const fill = g.createLinearGradient(0, 0, 800, 500);
    fill.addColorStop(0, `hsl(${h}, 60%, 38%)`);
    fill.addColorStop(1, `hsl(${(h + 50) % 360}, 60%, 18%)`);
    g.fillStyle = fill;
    g.fillRect(0, 0, 800, 500);
    g.font = "12px Arial";
    for (let x = 0; x <= 800; x += 50) {
      g.strokeStyle = x % 100 === 0 ? "rgba(255,255,255,0.55)" : "rgba(255,255,255,0.2)";
      g.beginPath(); g.moveTo(x + 0.5, 0); g.lineTo(x + 0.5, 500); g.stroke();
      if (x % 100 === 0) { g.fillStyle = "#fff"; g.fillText(String(x), x + 3, 14); }
    }
    for (let y = 0; y <= 500; y += 50) {
      g.strokeStyle = y % 100 === 0 ? "rgba(255,255,255,0.55)" : "rgba(255,255,255,0.2)";
      g.beginPath(); g.moveTo(0, y + 0.5); g.lineTo(800, y + 0.5); g.stroke();
      if (y % 100 === 0 && y > 0) { g.fillStyle = "#fff"; g.fillText(String(y), 3, y - 3); }
    }
    g.strokeStyle = "#ff3030"; g.lineWidth = 6; g.strokeRect(3, 3, 794, 494);
    g.fillStyle = "#fff"; g.font = "bold 40px Arial"; g.textAlign = "center";
    g.fillText(id.split("_")[0], 400, 240);
    g.font = "16px Arial"; g.fillText(id, 400, 272);
    g.textAlign = "left"; g.font = "bold 22px Arial"; g.fillStyle = "#ffe14d";
    g.fillText("TL", 14, 40);
    g.textAlign = "right"; g.fillText("BR", 786, 486);
    return canvas.toDataURL("image/png");
  };
  const knobFace = (id) => {
    const canvas = document.createElement("canvas");
    canvas.width = 256;
    canvas.height = 256;
    const g = canvas.getContext("2d");
    const body = g.createRadialGradient(100, 90, 10, 128, 128, 124);
    body.addColorStop(0, `hsl(${hue(id)}, 30%, 75%)`);
    body.addColorStop(1, `hsl(${hue(id)}, 30%, 22%)`);
    g.fillStyle = body;
    g.beginPath(); g.arc(128, 128, 122, 0, Math.PI * 2); g.fill();
    g.strokeStyle = "#111"; g.lineWidth = 4; g.stroke();
    g.strokeStyle = "#fff"; g.lineWidth = 14; g.lineCap = "round";
    g.beginPath(); g.moveTo(128, 128); g.lineTo(128, 22); g.stroke();
    g.fillStyle = "#ff3030"; g.beginPath(); g.arc(128, 128, 10, 0, Math.PI * 2); g.fill();
    return canvas.toDataURL("image/png");
  };
  const svgLogo = (id) => {
    const text = id.split("_")[0].toUpperCase();
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="400" height="160" viewBox="0 0 400 160">`
      + `<rect x="4" y="4" width="392" height="152" rx="24" fill="none" stroke="#ffe14d" stroke-width="8"/>`
      + `<text x="200" y="104" text-anchor="middle" font-family="Arial" font-weight="bold" font-size="64" fill="#ffe14d">${text}</text></svg>`;
    return `data:image/svg+xml;base64,${btoa(svg)}`;
  };
  const asDataUrl = async (url) => {
    const blob = await (await fetch(url)).blob();
    return await new Promise((ok, reject) => {
      const reader = new FileReader();
      reader.onload = () => ok(reader.result);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  };

  const libraries = {};
  const imageSources = {};
  for (const [key, fixture] of Object.entries(fixtures)) {
    const knobIds = new Set((fixture.layout.controls ?? []).map((c) => c.style?.knobImageId).filter(Boolean));
    const images = [];
    for (const image of fixture.images) {
      let dataUrl;
      if (fixture.realImages) {
        dataUrl = await asDataUrl(new URL(fixture.realImageBase + image.fileName, location.href).href);
        imageSources[image.imageId] = "bundled file";
      } else if (/\.svg$/i.test(image.fileName ?? "")) {
        dataUrl = svgLogo(image.imageId);
        imageSources[image.imageId] = "generated svg";
      } else if (knobIds.has(image.imageId) || image.type === "knob") {
        dataUrl = knobFace(image.imageId);
        imageSources[image.imageId] = "generated knob";
      } else {
        dataUrl = background(image.imageId);
        imageSources[image.imageId] = "generated background";
      }
      images.push({ imageId: image.imageId, fileName: image.fileName, dataUrl });
    }
    libraries[key] = images;
  }
  await Promise.all(Object.values(libraries).flat().map((image) => {
    const img = new Image();
    img.src = image.dataUrl;
    return img.decode().catch(() => undefined);
  }));

  window.__layoutBaseline = { renderer, knob, overlay, state, libraries, savedLibrary: state.uiState.layoutLibrary };
  const root = document.documentElement;
  return {
    imageSources,
    userAgent: navigator.userAgent,
    theme: root.dataset.theme ?? root.className ?? "",
    density: root.dataset.density ?? "",
    bodyClass: document.body.className,
  };
}

/** Draws one fixture at one panel width into the overlay and reports where it is and how it scaled. */
async function pageRender({ key, layout, width, bypassed, paramDefs, nodeParams, resources, stub }) {
  const { renderer, knob, overlay, state, libraries } = window.__layoutBaseline;
  // A minimised window stops painting: what would be shot is not what the user sees.
  if (document.hidden) throw new Error("The app window is minimised or hidden; restore it and run again");
  state.uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: libraries[key] };

  let host = document.getElementById("layout-baseline-host");
  if (!host) {
    host = document.createElement("div");
    host.id = "layout-baseline-host";
    document.body.appendChild(host);
    // The real mouse may rest over the window, and :hover would then change what is shot.
    const noHover = document.createElement("style");
    noHover.id = "layout-baseline-no-hover";
    noHover.textContent = "#layout-baseline-host, #layout-baseline-host * { pointer-events: none !important; }";
    document.head.appendChild(noHover);
  }
  host.style.cssText = `position:fixed;left:0;top:0;z-index:2147483647;width:${width}px;background:#7f7f7f;margin:0;padding:0;`;

  const node = { id: "layout-baseline-node", type: layout.effectType, params: { ...nodeParams }, bypassed };
  const html = layout.useDefaultControls
    ? renderer.renderCustomLayoutBackdrop(node, structuredClone(layout), stub)
    : renderer.renderCustomLayout(node, structuredClone(layout), paramDefs, resources);
  host.innerHTML = `<section class="default-effect-shell has-custom-layout${bypassed ? " is-bypassed" : ""}">`
    + `<div class="default-effect-shell-main">`
    + `<div class="default-effect-section default-effect-section-controls default-effect-section-custom-layout">${html}</div>`
    + `</div></section>`;

  host.querySelectorAll(".node-param-knob").forEach((element) => {
    const step = element.dataset.step ? Number(element.dataset.step) : undefined;
    new knob.GenericKnob({
      knobElement: element,
      paramId: `layout-baseline_${element.dataset.paramKey}`,
      minValue: Number(element.dataset.min),
      maxValue: Number(element.dataset.max),
      defaultValue: Number(element.dataset.default),
      displayFormat: (value) => String(value),
      stepValue: step,
      taper: element.dataset.taper === "log" ? "log" : undefined,
      sendParameter: false,
    });
  });
  overlay.applyCustomLayoutScaling(host);

  await new Promise((ok) => { requestAnimationFrame(() => requestAnimationFrame(ok)); setTimeout(ok, 250); });
  await new Promise((ok) => setTimeout(ok, 200));
  const outer = host.querySelector(".custom-layout-scale-outer");
  const inner = host.querySelector(".custom-layout-container");
  // The layout's own box. The shell around it still applies (its bypass filter shows), but
  // its 1 px top highlight rasterises a few levels differently from run to run, so it is left
  // out of the picture rather than loosening the tolerance for everything. The app's UI zoom
  // puts the box on a fractional pixel, and a clip there takes in a blended row of the shell,
  // so nudge the overlay until the box starts on a whole pixel.
  const target = outer ?? host;
  let left = 0;
  let top = 0;
  for (let pass = 0; pass < 4; pass++) {
    const r = target.getBoundingClientRect();
    const dx = Math.round(r.x) - r.x;
    const dy = Math.round(r.y) - r.y;
    if (Math.abs(dx) < 0.005 && Math.abs(dy) < 0.005) break;
    left += dx;
    top += dy;
    host.style.left = `${left}px`;
    host.style.top = `${top}px`;
    await new Promise((ok) => { requestAnimationFrame(() => requestAnimationFrame(ok)); setTimeout(ok, 250); });
  }
  const exact = target.getBoundingClientRect();
  const rect = {
    x: Math.round(exact.x),
    y: Math.round(exact.y),
    width: Math.round(exact.width),
    height: Math.round(exact.height),
    measured: [exact.x, exact.y, exact.width, exact.height].map((v) => Math.round(v * 1000) / 1000),
  };
  return {
    rect,
    outerWidth: outer?.offsetWidth ?? 0,
    outerHeight: outer?.style.height ?? "",
    transform: inner?.style.transform ?? "",
  };
}

/** Removes the overlay and puts the app's own library and panel scaling back. */
function pageTeardown() {
  const baseline = window.__layoutBaseline;
  if (!baseline) return false;
  document.getElementById("layout-baseline-host")?.remove();
  document.getElementById("layout-baseline-no-hover")?.remove();
  baseline.state.uiState.layoutLibrary = baseline.savedLibrary;
  baseline.overlay.applyCustomLayoutScaling(document.getElementById("node-params-panel"));
  delete window.__layoutBaseline;
  return true;
}

/** Counts pixels that differ by more than `tolerance` in any channel and draws where. */
async function pageCompare({ baseline, current, tolerance }) {
  const decode = async (b64) => createImageBitmap(await (await fetch(`data:image/png;base64,${b64}`)).blob());
  const [a, b] = await Promise.all([decode(baseline), decode(current)]);
  if (a.width !== b.width || a.height !== b.height) {
    return { sameSize: false, baselineSize: [a.width, a.height], currentSize: [b.width, b.height] };
  }
  const read = (bitmap) => {
    const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
    const g = canvas.getContext("2d");
    g.drawImage(bitmap, 0, 0);
    return g.getImageData(0, 0, bitmap.width, bitmap.height);
  };
  const pa = read(a);
  const pb = read(b);
  const diff = new ImageData(a.width, a.height);
  let differing = 0;
  let maxDelta = 0;
  for (let i = 0; i < pa.data.length; i += 4) {
    const delta = Math.max(
      Math.abs(pa.data[i] - pb.data[i]),
      Math.abs(pa.data[i + 1] - pb.data[i + 1]),
      Math.abs(pa.data[i + 2] - pb.data[i + 2]),
      Math.abs(pa.data[i + 3] - pb.data[i + 3]),
    );
    maxDelta = Math.max(maxDelta, delta);
    const grey = (pa.data[i] + pa.data[i + 1] + pa.data[i + 2]) / 3;
    if (delta > tolerance) {
      differing++;
      diff.data.set([255, 0, 0, 255], i);
    } else {
      diff.data.set([grey * 0.35, grey * 0.35, grey * 0.35, 255], i);
    }
  }
  let diffPng = null;
  if (differing > 0) {
    const canvas = new OffscreenCanvas(a.width, a.height);
    canvas.getContext("2d").putImageData(diff, 0, 0);
    const bytes = new Uint8Array(await (await canvas.convertToBlob({ type: "image/png" })).arrayBuffer());
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    diffPng = btoa(binary);
  }
  return { sameSize: true, differing, total: a.width * a.height, maxDelta, diffPng };
}

// ---- Driver ----

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const fixtures = loadFixtures();
  mkdirSync(args.out, { recursive: true });

  const target = await findPageTarget(args.port);
  const cdp = connect(target.webSocketDebuggerUrl);
  await cdp.ready;
  const { send } = cdp;

  // The app has to have booted: its modules loaded and its message handler registered.
  await inPage(send, async () => {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      if (document.readyState === "complete" && typeof window.IPlugReceiveData === "function") return true;
      await new Promise((ok) => setTimeout(ok, 250));
    }
    throw new Error("The app did not finish booting");
  });
  await new Promise((r) => setTimeout(r, 2000));

  const setup = await inPage(send, pageSetup, { fixtures });
  const shots = [];
  for (const [dpr, widths] of [[1, WIDTHS_1X], [2, WIDTHS_2X]]) {
    for (const key of Object.keys(fixtures)) {
      // A layer on its own only needs the narrowest and widest panel, at 1x.
      const fixtureWidths = fixtures[key].layerOnly ? (dpr === 1 ? [WIDTHS_1X[0], WIDTHS_1X.at(-1)] : []) : widths;
      for (const width of fixtureWidths) {
        const states = BYPASS_FIXTURES.has(key) && dpr === 1 && width === WIDTHS_1X.at(-1) ? [false, true] : [false];
        for (const bypassed of states) shots.push({ key, width, dpr, bypassed });
      }
    }
  }

  const manifest = {
    capturedAt: new Date().toISOString(),
    commit: execSync("git rev-parse --short HEAD", { cwd: REPO }).toString().trim(),
    appUrl: target.url,
    ...setup,
    note: "Rendered with the app's renderer, knobs and scaling in an overlay; images supplied by the script (see its header).",
    entries: [],
  };
  const comparisons = [];
  let failed = 0;

  try {
    let currentDpr = 0;
    for (const shot of shots) {
      if (shot.dpr !== currentDpr) {
        currentDpr = shot.dpr;
        // Set explicitly every time: an override outlives the connection that sent it.
        await send("Emulation.setDeviceMetricsOverride", { width: 1600, height: 1200, deviceScaleFactor: currentDpr, mobile: false });
        await new Promise((r) => setTimeout(r, 1500));
      }
      const metrics = await inPage(send, pageRender, {
        key: shot.key,
        layout: fixtures[shot.key].layout,
        width: shot.width,
        bypassed: shot.bypassed,
        paramDefs: PARAM_DEFS,
        nodeParams: NODE_PARAMS,
        resources: RESOURCES,
        stub: DEFAULT_CONTROLS_STUB,
      });
      const { rect } = metrics;
      const screenshot = async () => {
        await new Promise((r) => setTimeout(r, 120));
        const { data } = await send("Page.captureScreenshot", {
          format: "png",
          captureBeyondViewport: true,
          clip: { x: rect.x, y: rect.y, width: rect.width, height: rect.height, scale: 1 },
        });
        return data;
      };
      // The GPU now and then rasterises a tile edge a few levels differently. Keep a shot only
      // once two in a row agree, so the baseline holds the steady picture.
      let data = await screenshot();
      let stable = false;
      for (let attempt = 0; attempt < 4 && !stable; attempt++) {
        const again = await screenshot();
        stable = again === data;
        data = again;
      }
      const shotResult = { data };
      const file = `${shot.key}--w${shot.width}--${shot.dpr}x${shot.bypassed ? "--bypassed" : ""}.png`;
      writeFileSync(join(args.out, file), Buffer.from(shotResult.data, "base64"));
      if (!stable) console.warn(`unsteady: ${file} never gave two identical screenshots in a row`);
      manifest.entries.push({
        file,
        fixture: shot.key,
        panelWidth: shot.width,
        dpr: shot.dpr,
        state: shot.bypassed ? "bypassed" : "enabled",
        layoutSize: fixtures[shot.key].layout.dimensions,
        outerWidth: metrics.outerWidth,
        outerHeight: metrics.outerHeight,
        transform: metrics.transform,
        clip: rect,
      });

      if (args.compare) {
        const baselinePath = join(args.compare, file);
        if (!existsSync(baselinePath)) {
          comparisons.push({ file, result: "missing from baseline" });
          failed++;
          continue;
        }
        const baselineData = readFileSync(baselinePath).toString("base64");
        let result = await inPage(send, pageCompare, { baseline: baselineData, current: shotResult.data, tolerance: args.tolerance });
        // Raster noise comes and goes; a real change stays. Retake twice before calling it a difference.
        for (let retake = 1; retake <= 2 && !(result.sameSize && result.differing === 0); retake++) {
          const again = await screenshot();
          const retaken = await inPage(send, pageCompare, { baseline: baselineData, current: again, tolerance: args.tolerance });
          if (retaken.sameSize && retaken.differing === 0) {
            writeFileSync(join(args.out, file), Buffer.from(again, "base64"));
            result = { ...retaken, matchedOnRetake: retake };
          } else if (retake === 2) {
            result.retakes = 2;
          }
        }
        if (result.diffPng) {
          writeFileSync(join(args.out, file.replace(/\.png$/, ".diff.png")), Buffer.from(result.diffPng, "base64"));
        }
        delete result.diffPng;
        const same = result.sameSize && result.differing === 0;
        if (!same) failed++;
        comparisons.push({ file, ...result });
        const retakeNote = result.matchedOnRetake ? ` (matched on retake ${result.matchedOnRetake})` : "";
        console.log(`${same ? "same   " : "DIFFERS"} ${file}${same ? retakeNote : ` ${JSON.stringify(result)}`}`);
      } else {
        console.log(`captured ${file} (${metrics.transform || "native"}, outer ${metrics.outerWidth}px)`);
      }
    }
  } finally {
    await inPage(send, pageTeardown).catch((e) => console.error(`Teardown failed: ${e.message}`));
    cdp.close();
  }

  if (args.compare) manifest.comparedWith = args.compare;
  if (comparisons.length) manifest.comparisons = comparisons;
  writeFileSync(join(args.out, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
  console.log(`${manifest.entries.length} captures in ${args.out}${args.compare ? `; ${failed} differ` : ""}`);
  process.exit(failed ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(2);
});
