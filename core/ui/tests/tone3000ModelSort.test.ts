import { afterEach, describe, expect, it, vi } from "vitest";
import { uiState } from "../ts/state.js";
import { sortTone3000ModelsByName } from "../ts/tone3000Api";
import type { Tone3000Model, Tone3000Tone } from "../ts/tone3000ApiTypes";
import { fetchTone3000Models } from "../ts/tone3000Shared";

function makeModel(id: string | number, name: string): Tone3000Model {
  return { id, name, model_url: `https://tone3000.test/${id}` };
}

const names = (models: Tone3000Model[]): string[] => models.map((model) => model.name);

describe("sortTone3000ModelsByName", () => {
  it("sorts alphabetically, ignoring case", () => {
    const models = [makeModel(1, "clean"), makeModel(2, "Crunch"), makeModel(3, "Boost")];
    expect(names(sortTone3000ModelsByName(models))).toEqual(["Boost", "clean", "Crunch"]);
  });

  it("orders numbers in names by value", () => {
    const models = [makeModel(1, "Gain 10"), makeModel(2, "Gain 2"), makeModel(3, "Gain 1")];
    expect(names(sortTone3000ModelsByName(models))).toEqual(["Gain 1", "Gain 2", "Gain 10"]);
  });

  it("breaks name ties by model id", () => {
    const models = [makeModel(20, "Lead"), makeModel(3, "Lead")];
    expect(sortTone3000ModelsByName(models).map((model) => model.id)).toEqual([3, 20]);
  });

  it("does not reorder the list it was given", () => {
    const models = [makeModel(1, "B"), makeModel(2, "A")];
    sortTone3000ModelsByName(models);
    expect(names(models)).toEqual(["B", "A"]);
  });
});

/// Both the resource browser and the Settings browser list what this returns.
describe("fetchTone3000Models", () => {
  const originalFetch = globalThis.fetch;

  afterEach(() => {
    globalThis.fetch = originalFetch;
    uiState.tone3000Session = null;
  });

  it("returns the tone's models sorted by name", async () => {
    uiState.tone3000Session = { accessToken: "session-token", refreshToken: "", expiresAt: Date.now() + 60_000 };
    uiState.appSettings = {
      ...uiState.appSettings,
      "tone3000.apiKey": "test-api-key",
      "tone3000.useSoundshedToneSearchApi": true,
    };
    globalThis.fetch = vi.fn(async () => ({
      ok: true,
      status: 200,
      json: async () => ({ models: [makeModel(1, "Gain 10"), makeModel(2, "clean"), makeModel(3, "Gain 2")] }),
      text: async () => "",
      headers: new Headers(),
    }) as Response) as typeof fetch;

    const models = await fetchTone3000Models({ id: "tone-1", title: "Tone" } as Tone3000Tone);

    expect(names(models)).toEqual(["clean", "Gain 2", "Gain 10"]);
  });
});
