import { afterEach, describe, expect, it, vi } from "vitest";

vi.mock("../ts/bridge.js", () => ({ setAppSetting: vi.fn() }));

import {
  LibraryPackGrouping,
  groupLibraryByPack,
  isPackGroupingEnabled,
  libraryPackOf,
  nameWithinPack,
} from "../ts/resourceBrowser/libraryPacks.js";
import { LIBRARY_GROUP_PACKS_SETTING } from "../ts/resourceBrowser/settings.js";
import { uiState } from "../ts/state.js";
import type { LibraryResource } from "../ts/types.js";

function model(id: string, name: string, metadata: Record<string, string> = {}, filePath = ""): LibraryResource {
  return { id, name, category: "amp", filePath, metadata } as unknown as LibraryResource;
}

const jcm = { toneId: "1071", toneTitle: "Marshall JCM 800 2203", provider: "tone3000" };
const library = [
  model("j2", "Marshall JCM 800 2203 - G2", jcm),
  model("a1", "Alpha Clean", { provider: "local" }, "C:/NAM/Alpha/clean.nam"),
  model("j8", "Marshall JCM 800 2203 - G8", jcm),
  model("a2", "Alpha Drive", { provider: "local" }, "C:/NAM/Alpha/drive.nam"),
  model("solo", "Bright Boost", { toneId: "77", toneTitle: "Boosts", provider: "tone3000" }),
  model("f1", "Factory Lead", { factoryArchiveKey: "Factory-Preset-Pack", provider: "factory-archives" }),
  model("f2", "Factory Clean", { factoryArchiveKey: "Factory-Preset-Pack", provider: "factory-archives" }),
];
const noFavourites = (): boolean => false;

describe("library pack grouping", () => {
  const originalSettings = uiState.appSettings;
  afterEach(() => {
    uiState.appSettings = originalSettings;
  });

  it("finds a model's pack from its tone or its factory archive, and never from its folder", () => {
    expect(libraryPackOf(library[0])).toEqual({ key: "tone:1071", title: "Marshall JCM 800 2203" });
    expect(libraryPackOf(library[5])).toEqual({ key: "archive:Factory-Preset-Pack", title: "Factory Preset Pack" });
    // One folder often holds every capture the user owns, so a folder is not a pack.
    expect(libraryPackOf(library[1])).toBeNull();
    expect(libraryPackOf(model("t", "Untitled", { toneId: "9" }))?.title).toBe("Tone3000 pack 9");
  });

  it("keeps a pack's models together in the order given, and draws a pack of one as a lone model", () => {
    const entries = groupLibraryByPack(library, noFavourites);
    expect(entries.map((entry) => entry.pack?.title ?? entry.items[0].name)).toEqual([
      "Alpha Clean", "Alpha Drive", "Bright Boost", "Factory Preset Pack", "Marshall JCM 800 2203",
    ]);
    expect(entries[4].items.map((item) => item.id)).toEqual(["j2", "j8"]);
    expect(entries[2].pack).toBeNull();
  });

  it("puts entries holding a favourite first", () => {
    const entries = groupLibraryByPack(library, (id) => id === "j8");
    expect(entries[0].pack?.title).toBe("Marshall JCM 800 2203");
  });

  it("shortens a model's name inside its pack when the name repeats the pack's", () => {
    const pack = { key: "tone:1071", title: "Marshall JCM 800 2203" };
    expect(nameWithinPack("Marshall JCM 800 2203 - G2", pack)).toBe("G2");
    expect(nameWithinPack("JCM800 G2", pack)).toBe("JCM800 G2");
  });

  it("is on unless turned off", () => {
    uiState.appSettings = { ...uiState.appSettings };
    delete uiState.appSettings[LIBRARY_GROUP_PACKS_SETTING];
    expect(isPackGroupingEnabled()).toBe(true);
    uiState.appSettings = { ...uiState.appSettings, [LIBRARY_GROUP_PACKS_SETTING]: false };
    expect(isPackGroupingEnabled()).toBe(false);
  });

  it("opens the pack holding the loaded model, keeps the rest closed, and remembers a click", () => {
    uiState.appSettings = { ...uiState.appSettings, [LIBRARY_GROUP_PACKS_SETTING]: true };
    const list = document.createElement("div");
    const redraw = vi.fn(() => {
      list.innerHTML = grouping.render(list, library, { selectedId: "j8", openAll: false, isFavorite: noFavourites }, row);
    });
    const grouping = new LibraryPackGrouping(redraw);
    const row = (resource: LibraryResource, titleInPack?: string): string =>
      `<div class="row" data-id="${resource.id}">${titleInPack ?? resource.name}</div>`;
    redraw();

    const open = [...list.querySelectorAll(".resource-browser-pack.is-open .resource-browser-pack-title")].map((el) => el.textContent);
    expect(open).toEqual(["Marshall JCM 800 2203"]);
    expect([...list.querySelectorAll(".row")].map((el) => el.textContent)).toEqual(["Alpha Clean", "Alpha Drive", "Bright Boost", "G2", "G8"]);

    list.querySelector<HTMLElement>('[data-pack-key="archive:Factory-Preset-Pack"]')!.click();
    expect([...list.querySelectorAll(".row")].map((el) => el.getAttribute("data-id"))).toEqual(["a1", "a2", "solo", "f1", "f2", "j2", "j8"]);
  });

  it("opens every pack while a search or Favourites narrows the list, and lists flat when off", () => {
    const list = document.createElement("div");
    const grouping = new LibraryPackGrouping(() => undefined);
    const row = (resource: LibraryResource): string => `<div class="row">${resource.id}</div>`;
    uiState.appSettings = { ...uiState.appSettings, [LIBRARY_GROUP_PACKS_SETTING]: true };
    list.innerHTML = grouping.render(list, library, { selectedId: "", openAll: true, isFavorite: noFavourites }, row);
    expect(list.querySelectorAll(".row")).toHaveLength(library.length);

    uiState.appSettings = { ...uiState.appSettings, [LIBRARY_GROUP_PACKS_SETTING]: false };
    list.innerHTML = grouping.render(list, library, { selectedId: "", openAll: false, isFavorite: noFavourites }, row);
    expect(list.querySelector(".resource-browser-pack")).toBeNull();
    expect(grouping.order(library, noFavourites)).toBe(library);
  });
});
