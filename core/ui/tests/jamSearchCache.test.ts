import { beforeEach, describe, expect, it, vi } from "vitest";
import type { JamVideoSummary } from "../ts/types";

const { fetchBackingTracks } = vi.hoisted(() => ({ fetchBackingTracks: vi.fn<(query: string, apiKey: string) => Promise<JamVideoSummary[]>>() }));
vi.mock("../ts/bridge.js", () => ({ postMessage: vi.fn(), setAppSetting: vi.fn() }));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn() }));
vi.mock("../ts/buildFlags.js", () => ({ isJamEnabled: () => true }));
vi.mock("../ts/featureFlags.js", () => ({
  FEATURE_FLAGS_CHANGED_EVENT: "featureFlagsChanged",
  Features: { Jam: "jam" },
  isFeatureEnabled: () => true,
  isJamExperienceEnabled: () => true,
}));
vi.mock("../ts/riffLibrary.js", () => ({ renderRiffLibraryPanel: vi.fn() }));
vi.mock("../ts/practiceTool.js", () => ({ renderPracticeToolPanel: vi.fn() }));
vi.mock("../ts/jam/panel.js", () => ({ renderJamPanel: vi.fn() }));
vi.mock("../ts/jam/player.js", () => ({ openPlayer: vi.fn(), renderFloatingPlayer: vi.fn(), toggleFavorite: vi.fn() }));
vi.mock("../ts/jam/search.js", () => ({ fetchBackingTracks }));

function video(id: string): JamVideoSummary {
  return { videoId: id, title: id, channelTitle: "c", thumbnailUrl: "" };
}

function deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void } {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((r) => { resolve = r; });
  return { promise, resolve };
}

const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

// The panel binds its input once per module, so each test starts from a fresh copy.
async function loadJam(appSettings: Record<string, unknown>) {
  vi.resetModules();
  document.body.innerHTML = `<input id="jam-search-input"><button id="jam-search-button"></button>`;
  const { uiState } = await import("../ts/state.js");
  uiState.appSettings = { "jam.youtubeApiKey": "key", ...appSettings } as never;
  const { initializeJamPanel } = await import("../ts/jam.js");
  initializeJamPanel();
  return { uiState };
}

function search(query: string): void {
  const input = document.getElementById("jam-search-input") as HTMLInputElement;
  input.value = query;
  input.dispatchEvent(new Event("input"));
  input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter" }));
}

beforeEach(() => {
  fetchBackingTracks.mockReset();
});

describe("jam search", () => {
  it("caches the results under the query that was searched, not what the box says now", async () => {
    const { uiState } = await loadJam({});
    const blues = deferred<JamVideoSummary[]>();
    fetchBackingTracks.mockReturnValueOnce(blues.promise);

    search("blues");
    const input = document.getElementById("jam-search-input") as HTMLInputElement;
    input.value = "blues slow";
    input.dispatchEvent(new Event("input"));
    blues.resolve([video("b1")]);
    await flush();

    const cache = uiState.appSettings?.["jam.searchCache"] as { query: string; normalizedQuery: string } | undefined;
    expect(cache?.query).toBe("blues");
    expect(cache?.normalizedQuery).toBe("blues backing track");
  });

  it("keeps a cache hit over an older reply that lands after it", async () => {
    const { uiState } = await loadJam({
      "jam.searchCache": { query: "blues", normalizedQuery: "blues backing track", results: [video("cached")] },
    });
    const rock = deferred<JamVideoSummary[]>();
    fetchBackingTracks.mockReturnValueOnce(rock.promise);

    search("rock");
    search("blues");
    expect(uiState.jam?.results.map((r) => r.videoId)).toEqual(["cached"]);

    rock.resolve([video("r1")]);
    await flush();
    expect(uiState.jam?.results.map((r) => r.videoId)).toEqual(["cached"]);
  });
});
