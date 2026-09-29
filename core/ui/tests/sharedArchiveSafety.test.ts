/**
 * What a shared archive, a Tone Sharing pack or the update server can make the UI do beyond
 * showing text: which node ids and paths it can store, which hosts get a credential, and
 * what the page will run.
 */
import { createRequire } from "node:module";
import { createHash } from "node:crypto";
import JSZip from "jszip";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { Preset } from "../ts/types";

vi.mock("../ts/notifications.js", () => ({ clearNotification: vi.fn(), showNotification: vi.fn() }));

const sent: Array<Record<string, unknown>> = [];
window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
(window as unknown as { JSZip: typeof JSZip }).JSZip = JSZip;

const { uiState } = await import("../ts/state.js");
const { sanitizeImportedPreset, SAFE_NODE_ID } = await import("../ts/presets/sanitize.js");
const { importPresetArchive } = await import("../ts/presets/archive.js");
const { tone3000AuthenticatedFetch, isTone3000RequestUrlAllowed } = await import("../ts/tone3000.js");
const { toneSharingFetch } = await import("../ts/toneSharingPanel/api.js");
const { toneSharingState } = await import("../ts/toneSharingPanel/state.js");
const { sanitizeDebugValue } = await import("../ts/messages/debugSnapshot.js");
const { renderMarkdown } = await import("../ts/updateCheck.js");

const HOSTILE = `amp"><img src=x onerror=alert(1)>`;
const EVIL_MODEL_URL = "https://evil.example/steal.nam";
const originalFetch = globalThis.fetch;

function hostilePreset(): Preset {
  const amp = {
    id: HOSTILE,
    type: "amp",
    displayName: "Amp",
    category: "",
    bypassed: false,
    params: {},
    config: {},
    resources: [{ resourceType: "nam", resourceId: "t3k-model", filePath: String.raw`\\attacker.example\share\amp.nam` }],
  };
  const graph = {
    nodes: [{ ...amp, id: "__input__", type: "input", resources: [] }, amp, { ...amp, id: "__output__", type: "output", resources: [] }],
    edges: [
      { from: "__input__", to: HOSTILE, fromPort: 0, toPort: 0, gain: 1 },
      { from: HOSTILE, to: "__output__", fromPort: 0, toPort: 0, gain: 1 },
      { from: "__input__", to: "<no such node>", fromPort: 0, toPort: 0, gain: 1 },
    ],
  };
  return {
    id: "shared-1",
    name: "Shared",
    customIrPath: "C:/Users/sharer/cab.wav",
    graph,
    scenes: [{ id: "scene-b", title: "B", graph: structuredClone(graph) }],
  };
}

async function archiveFile(): Promise<File> {
  const zip = new JSZip();
  zip.file("preset.json", JSON.stringify({
    formatVersion: 1,
    preset: hostilePreset(),
    resources: [{ id: "bundled-ir", name: "Cab", type: "ir", fileName: "cab.wav", hash: "not-in-library" }],
    tone3000Resources: [{ id: "t3k-model", name: "Amp", type: "nam", toneId: "tone-1", modelId: "m1", modelUrl: EVIL_MODEL_URL }],
    blends: [{ id: "blend-1", name: "Blend", models: ["t3k-model"] }],
  }));
  zip.file("resources/cab.wav", new Uint8Array([1, 2, 3]));
  const bytes = await zip.generateAsync({ type: "uint8array" });
  return new File([bytes], "shared.soundshed.preset");
}

function okBytes(): Response {
  return { ok: true, status: 200, arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer, text: async () => "", headers: new Headers() } as Response;
}

beforeEach(() => {
  sent.length = 0;
  uiState.resourceLibrary = {};
  // Well clear of the refresh lead, so BYOK mode uses this session rather than starting one.
  uiState.tone3000Session = { accessToken: "session-token", refreshToken: "", expiresAt: Date.now() + 600_000 };
  uiState.appSettings = { ...uiState.appSettings, "tone3000.apiKey": "test-api-key", "tone3000.useSoundshedToneSearchApi": true };
});

afterEach(() => {
  globalThis.fetch = originalFetch;
  uiState.tone3000Session = null;
});

describe("sanitizeImportedPreset", () => {
  it("replaces an unsafe node id everywhere it appears, and drops file paths", () => {
    const preset = sanitizeImportedPreset(hostilePreset());
    const amp = preset.graph!.nodes[1];

    expect(amp.id).not.toBe(HOSTILE);
    expect(SAFE_NODE_ID.test(amp.id)).toBe(true);
    expect(preset.graph!.edges.map((edge) => [edge.from, edge.to])).toEqual([["__input__", amp.id], [amp.id, "__output__"]]);
    expect(preset.scenes![0].graph.nodes[1].id).toBe(amp.id);
    expect(amp.resources![0]).toEqual({ resourceType: "nam", resourceId: "t3k-model" });
    expect(preset.customIrPath).toBeUndefined();
  });

  it("leaves safe ids alone", () => {
    const preset = sanitizeImportedPreset(hostilePreset());
    expect(preset.graph!.nodes.map((node) => node.id).filter((id) => id.startsWith("__"))).toEqual(["__input__", "__output__"]);
  });
});

describe("importing a shared archive", () => {
  it("previews without downloading, importing or saving anything", async () => {
    const fetchMock = vi.fn(async () => okBytes());
    globalThis.fetch = fetchMock as typeof fetch;

    const [preset] = await importPresetArchive(await archiveFile(), { source: "toneSharingApi", itemId: "item-1" }, {
      previewOnly: true,
      suppressNotifications: true,
    });

    expect(fetchMock).not.toHaveBeenCalled();
    expect(sent.map((message) => message.type)).toEqual([]);
    expect(preset.graph!.nodes.every((node) => SAFE_NODE_ID.test(node.id))).toBe(true);
  });

  it("never fetches the archive's own modelUrl: the model is found by its Tone3000 ids", async () => {
    const fetched: string[] = [];
    globalThis.fetch = vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      fetched.push(url);
      if (url.includes("/models?") || url.includes("tone_id=")) {
        return {
          ok: true,
          status: 200,
          json: async () => ({ data: [{ id: "m1", model_url: "https://www.tone3000.com/api/v1/models/m1/download/m1.nam" }], page: 1, total_pages: 1 }),
          text: async () => "",
          headers: new Headers(),
        } as Response;
      }
      return okBytes();
    }) as typeof fetch;

    await importPresetArchive(await archiveFile(), { source: "zipImport" }, { suppressNotifications: true });

    expect(fetched.length).toBeGreaterThan(0);
    expect(fetched.some((url) => url.includes("evil.example"))).toBe(false);
    const imported = sent.find((message) => message.type === "importRemoteResource" && message.provider === "tone3000");
    expect(imported).toBeDefined();
    expect((imported!.metadata as Record<string, unknown>).modelUrl).toBeUndefined();
    const saved = sent.find((message) => message.type === "savePreset")?.preset as Preset | undefined;
    expect(saved?.graph?.nodes.every((node) => SAFE_NODE_ID.test(node.id))).toBe(true);
  });
});

describe("Tone3000 credentials", () => {
  it("go only to the Tone3000 API", async () => {
    uiState.appSettings = { ...uiState.appSettings, "tone3000.useSoundshedToneSearchApi": false };
    const fetchMock = vi.fn(async () => okBytes());
    globalThis.fetch = fetchMock as typeof fetch;

    await expect(tone3000AuthenticatedFetch(EVIL_MODEL_URL)).rejects.toThrow(/evil\.example/);
    expect(fetchMock).not.toHaveBeenCalled();

    await tone3000AuthenticatedFetch("https://www.tone3000.com/api/v1/models/m1/download/m1.nam");
    const init = (fetchMock.mock.calls[0] as unknown[])[1] as RequestInit;
    expect(new Headers(init.headers).get("authorization")).toBe("Bearer session-token");
  });

  it("allow the configured proxy in proxy mode, and no other host", () => {
    expect(isTone3000RequestUrlAllowed("https://api-guitar.soundshed.com/v1/resourcesearch/models")).toBe(true);
    expect(isTone3000RequestUrlAllowed("https://www.tone3000.com.evil.example/api/v1/models")).toBe(false);
    expect(isTone3000RequestUrlAllowed("not a url")).toBe(false);
  });
});

describe("the Tone Sharing session", () => {
  it("goes to the API's own origin and nowhere else", async () => {
    toneSharingState.sessionId = "tone-sharing-session";
    const fetchMock = vi.fn(async () => okBytes());
    globalThis.fetch = fetchMock as typeof fetch;

    await toneSharingFetch("https://thumbnails.evil.example/pack.png");
    await toneSharingFetch("/items/item-1/download");

    const [foreign, own] = fetchMock.mock.calls as unknown as Array<[string, RequestInit]>;
    expect(new Headers(foreign[1].headers).get("x-session-id")).toBeNull();
    expect(foreign[1].credentials).toBe("omit");
    expect(own[0].startsWith(toneSharingState.apiBase)).toBe(true);
    expect(new Headers(own[1].headers).get("x-session-id")).toBe("tone-sharing-session");
    expect(own[1].credentials).toBe("include");
  });

  it("is redacted from debug snapshots", () => {
    const snapshot = sanitizeDebugValue({ appSettings: { "toneSharing.sessionId": "secret", "tone3000.apiKey": "key" } });
    expect(JSON.stringify(snapshot)).not.toMatch(/secret|"key"/);
  });
});

describe("update release notes", () => {
  it("are text with http(s) links only", () => {
    const html = renderMarkdown("## New <img src=x onerror=alert(1)>\n[bad](javascript:alert(1)) [good](https://soundshed.com/download)");
    const root = document.createElement("div");
    root.innerHTML = html;

    expect(root.querySelector("img")).toBeNull();
    expect(root.querySelector("h2")?.textContent).toBe("New <img src=x onerror=alert(1)>");
    expect(Array.from(root.querySelectorAll("a"), (a) => a.getAttribute("href"))).toEqual(["https://soundshed.com/download"]);
  });
});

describe("the page's Content-Security-Policy", () => {
  const require = createRequire(import.meta.url);
  const { assembleHtml } = require("../scripts/assemble-html.js") as { assembleHtml: () => { html: string } };
  const { html } = assembleHtml();
  const policy = /<meta http-equiv="Content-Security-Policy" content="([^"]+)"/.exec(html)?.[1] ?? "";
  const directive = (name: string) => policy.split(";").map((part) => part.trim()).find((part) => part.startsWith(`${name} `)) ?? "";

  it("runs no inline script it was not built with, and no plugins or eval", () => {
    const scriptSrc = directive("script-src");
    expect(scriptSrc).not.toMatch(/unsafe-inline|unsafe-eval/);
    expect(directive("object-src")).toBe("object-src 'none'");
    expect(directive("base-uri")).toBe("base-uri 'none'");

    // Every inline script after the policy matches a hash in it (the browser hashes LF text).
    const afterPolicy = html.slice(html.indexOf("Content-Security-Policy"));
    const inline = Array.from(afterPolicy.matchAll(/<script>([\s\S]*?)<\/script>/g), (match) => match[1]);
    expect(inline.length).toBeGreaterThan(0);
    for (const text of inline) {
      const hash = createHash("sha256").update(text.replace(/\r\n?/g, "\n"), "utf8").digest("base64");
      expect(scriptSrc).toContain(`'sha256-${hash}'`);
    }
  });

  it("loads no template library that evaluates markup", () => {
    expect(html).not.toMatch(/alpine|x-data|x-on:/i);
  });
});
