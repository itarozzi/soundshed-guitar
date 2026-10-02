import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { uiState } from "../ts/state.js";
import { applySharedTone3000Settings, downloadTone3000ResourceByReference } from "../ts/tone3000.js";

describe("downloadTone3000ResourceByReference", () => {
  const originalFetch = globalThis.fetch;

  beforeEach(() => {
    uiState.tone3000Session = {
      accessToken: "session-token",
      refreshToken: "",
      expiresAt: Date.now() + 60_000,
    };
    uiState.appSettings = {
      ...uiState.appSettings,
      "tone3000.apiKey": "test-api-key",
      "tone3000.useSoundshedToneSearchApi": true,
    };
  });

  afterEach(() => {
    vi.restoreAllMocks();
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  it("resolves a known model id without forcing an architecture filter", async () => {
    const fetchMock = vi.fn(async (input: RequestInfo | URL) => {
      const url = typeof input === "string" ? input : input.toString();

      if (url.includes("/models") && !url.includes("/models/known-a1-model")) {
        return {
          ok: true,
          status: 200,
          json: async () => ({
            models: [
              { id: "known-a1-model", model_url: "https://www.tone3000.com/api/v1/models/known-a1-model" },
            ],
          }),
          text: async () => "",
          headers: new Headers(),
        } as Response;
      }

      if (url.includes("/models/known-a1-model")) {
        return {
          ok: true,
          status: 200,
          arrayBuffer: async () => new Uint8Array([1, 2, 3]).buffer,
          headers: new Headers({ "content-type": "application/octet-stream" }),
          text: async () => "",
        } as Response;
      }

      throw new Error(`Unexpected fetch URL: ${url}`);
    });

    globalThis.fetch = fetchMock as typeof fetch;

    await downloadTone3000ResourceByReference({ toneId: "tone-123", modelId: "known-a1-model" });

    expect(fetchMock).toHaveBeenCalledTimes(2);
    const modelLookupUrl = fetchMock.mock.calls[0]?.[0];
    expect(String(modelLookupUrl)).toContain("tone_id=tone-123");
    expect(String(modelLookupUrl)).not.toContain("architecture=");
  });
});

describe("applySharedTone3000Settings", () => {
  const session = { accessToken: "session-token", refreshToken: "", expiresAt: Date.now() + 600_000 };

  beforeEach(() => {
    uiState.tone3000Session = { ...session };
  });

  afterEach(() => {
    uiState.tone3000Session = null;
  });

  it("keeps the session when another instance changed something else", () => {
    const previous = { "tone3000.apiKey": "key-a", "tone3000.useSoundshedToneSearchApi": false, theme: "dark" };
    uiState.appSettings = { ...previous, theme: "light" };

    applySharedTone3000Settings(previous);

    expect(uiState.tone3000Session).not.toBeNull();
  });

  it.each([
    ["the key was replaced", { "tone3000.apiKey": "key-b", "tone3000.useSoundshedToneSearchApi": false }],
    ["the key was cleared", { "tone3000.useSoundshedToneSearchApi": false }],
    ["the proxy was turned on", { "tone3000.apiKey": "key-a", "tone3000.useSoundshedToneSearchApi": true }],
  ])("drops the session when %s in another instance", (_label, next) => {
    uiState.appSettings = next;

    applySharedTone3000Settings({ "tone3000.apiKey": "key-a", "tone3000.useSoundshedToneSearchApi": false });

    expect(uiState.tone3000Session).toBeNull();
  });
});
