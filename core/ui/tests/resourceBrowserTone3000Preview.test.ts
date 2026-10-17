import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ResourceBrowserModal } from "../ts/resourceBrowser.js";
import type { PreviewState, ResourceBrowserOptions } from "../ts/resourceBrowser/types.js";
import { uiState } from "../ts/state.js";

/// The modal's own preview state and entry points, reached past `private` for the test.
type PreviewInternals = {
  options: ResourceBrowserOptions | null;
  previewState: PreviewState | null;
  startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void>;
  cancelPreview(restoreOriginal?: boolean): void;
};

describe("Tone3000 preview in the resource browser", () => {
  const originalFetch = globalThis.fetch;
  let sent: Array<Record<string, unknown>>;

  beforeEach(() => {
    sent = [];
    window.IPlugSendMsg = (message: string) => sent.push(JSON.parse(message) as Record<string, unknown>);
    uiState.tone3000Session = { accessToken: "session-token", refreshToken: "", expiresAt: Date.now() + 60_000 };
    uiState.appSettings = {
      ...uiState.appSettings,
      "tone3000.apiKey": "test-api-key",
      "tone3000.useSoundshedToneSearchApi": true,
    };
    globalThis.fetch = vi.fn(async () => ({
      ok: true,
      status: 200,
      arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer,
      text: async () => "",
      headers: new Headers({ "content-type": "application/octet-stream" }),
    }) as Response) as typeof fetch;
  });

  afterEach(() => {
    delete window.IPlugSendMsg;
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  function previewingModal(): PreviewInternals {
    const modal = new ResourceBrowserModal() as unknown as PreviewInternals;
    modal.options = { resourceType: "nam", nodeId: "amp", resourceIndex: 0, onSelect: () => {} };
    return modal;
  }

  // A cancel without restore deleted the first temp file and left the node on it, and the
  // engine then took that file for the node's original, so closing restored a missing file.
  it("starts the next preview without cancelling the one playing", async () => {
    const modal = previewingModal();

    await modal.startPreview("1", "a", "https://www.tone3000.com/api/v1/models/1/download/a.nam");
    await modal.startPreview("1", "b", "https://www.tone3000.com/api/v1/models/1/download/b.nam");

    expect(sent.map((message) => message.type)).toEqual(["previewRemoteResource", "previewRemoteResource"]);
    expect(sent[1]).toMatchObject({ nodeId: "amp", resourceIndex: 0, tempResourceId: "preview:tone3000:1:b" });
    expect(modal.previewState).toMatchObject({ active: true, modelId: "b" });
  });

  // The download takes a while. One finishing after the picker was opened for another node used
  // to be sent with that node's id and resource type; one finishing after a newer preview started
  // replaced it.
  it("drops a download that is no longer the current picker's latest preview", async () => {
    const downloads: Array<() => void> = [];
    globalThis.fetch = vi.fn(() => new Promise<Response>((resolve) => {
      downloads.push(() => resolve({
        ok: true,
        status: 200,
        arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer,
        text: async () => "",
        headers: new Headers({ "content-type": "application/octet-stream" }),
      } as Response));
    })) as typeof fetch;

    const modal = previewingModal();
    const pending = modal.startPreview("1", "a", "https://www.tone3000.com/api/v1/models/1/download/a.nam");
    modal.options = { resourceType: "ir", nodeId: "cab", resourceIndex: 0, onSelect: () => {} };
    await vi.waitFor(() => expect(downloads).toHaveLength(1));
    downloads[0]();
    await pending;
    expect(sent).toEqual([]);

    const slow = modal.startPreview("1", "b", "https://www.tone3000.com/api/v1/models/1/download/b.nam");
    await vi.waitFor(() => expect(downloads).toHaveLength(2));
    const fast = modal.startPreview("1", "c", "https://www.tone3000.com/api/v1/models/1/download/c.nam");
    await vi.waitFor(() => expect(downloads).toHaveLength(3));
    downloads[2]();
    await fast;
    downloads[1]();
    await slow;
    expect(sent.map((message) => message.tempResourceId)).toEqual(["preview:tone3000:1:c"]);
    expect(sent[0]).toMatchObject({ nodeId: "cab", resourceType: "ir" });
  });

  it("keeps the playing preview when the next download fails, so closing still restores", async () => {
    const modal = previewingModal();
    await modal.startPreview("1", "a", "https://www.tone3000.com/api/v1/models/1/download/a.nam");
    globalThis.fetch = vi.fn(async () => ({ ok: false, status: 404 }) as Response) as typeof fetch;

    await modal.startPreview("1", "b", "https://www.tone3000.com/api/v1/models/1/download/b.nam");
    modal.cancelPreview();

    expect(sent.map((message) => message.type)).toEqual(["previewRemoteResource", "cancelPreviewResource"]);
    expect(sent[1]).toMatchObject({ restoreOriginal: true });
  });
});
