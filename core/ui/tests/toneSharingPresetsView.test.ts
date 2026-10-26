import { readFileSync } from "node:fs";
import path from "node:path";
import { beforeEach, describe, expect, it, vi } from "vitest";
import type * as Feed from "../ts/toneSharingPanel/feed.js";

// Presets is the community search: an empty search is its default list (with the featured packs
// over it, feed.ts), so typing and clearing only ever search again. topBindings runs against the
// real panel markup, with the feed's loader and everything else it binds stubbed; the Cards/List
// switch runs through the real applyFeedLayout.
const stubs = vi.hoisted(() => ({
  scheduleCommunitySearch: vi.fn(),
  settings: {} as Record<string, unknown>,
}));
const fn = () => vi.fn();
vi.mock("../ts/appSettingsStore.js", () => ({
  getAppSetting: (key: string) => stubs.settings[key] ?? null,
  updateAppSetting: (key: string, value: unknown) => { stubs.settings[key] = value; },
}));
vi.mock("../ts/notifications.js", () => ({ showNotification: fn() }));
vi.mock("../ts/toneSharingPanel/account.js", () => ({
  closeSignInModal: fn(), openToneSharingSignInModal: fn(), previewSelectedProfileAvatar: fn(),
  saveProfile: fn(), sendCode: fn(), signOut: fn(), verifyCode: fn(),
}));
vi.mock("../ts/toneSharingPanel/actionButtons.js", () => ({ setActionButtonBusy: fn(), syncFeatureToggleButton: fn(), syncPackPreviewButtons: fn() }));
vi.mock("../ts/toneSharingPanel/adminEdit.js", () => ({ confirmAndDelete: fn(), openAdminEditModal: fn(), refreshAfterAdminChange: fn(), setFeatured: fn() }));
vi.mock("../ts/toneSharingPanel/assets.js", () => ({ downloadAsset: fn() }));
vi.mock("../ts/toneSharingPanel/packDetail.js", () => ({ clearPackDetail: fn() }));
vi.mock("../ts/toneSharingPanel/packEditor.js", () => ({ closePackModal: fn(), openPackModal: fn(), savePack: fn() }));
vi.mock("../ts/toneSharingPanel/preview.js", () => ({ clearPreviewPreset: fn(), previewPreset: fn() }));
vi.mock("../ts/toneSharingPanel/publish.js", () => ({ closeToneSharingPublishPresetModal: fn(), uploadAndPublishItem: fn() }));
vi.mock("../ts/toneSharingPanel/shareLinks.js", () => ({ clearActiveSharedFilter: fn(), copyToneSharingShareLink: fn() }));
vi.mock("../ts/toneSharingPanel/feed.js", async () => {
  const actual = await vi.importActual<typeof Feed>("../ts/toneSharingPanel/feed.js");
  return {
    applyFeedLayout: actual.applyFeedLayout,
    loadStandardBrowsePage: fn(),
    scheduleCommunitySearch: stubs.scheduleCommunitySearch,
    updateBrowseFooter: fn(),
    updateCommunityPresetSearchUi: fn(),
  };
});

const { browseState, storageKeys } = await import("../ts/toneSharingPanel/state.js");
const { bindTopControls } = await import("../ts/toneSharingPanel/topBindings.js");

document.body.innerHTML = readFileSync(path.resolve(__dirname, "../ui-components/panels/sharing-panel.html"), "utf8");
bindTopControls();

const input = () => document.getElementById("tone-sharing-community-search-input") as HTMLInputElement;
const tag = () => document.getElementById("tone-sharing-community-tag-filter") as HTMLSelectElement;
const feed = () => document.getElementById("tone-sharing-feed") as HTMLElement;
const layoutButton = (layout: string) => document.querySelector<HTMLButtonElement>(`[data-tone-sharing-layout="${layout}"]`)!;
const type = (value: string) => {
  input().value = value;
  input().dispatchEvent(new Event("input"));
};

beforeEach(() => {
  browseState.mode = "items";
  browseState.activeSharedTarget = null;
  browseState.searchQuery = "";
  browseState.tagFilter = "";
  input().value = "";
  tag().value = "";
  vi.clearAllMocks();
});

describe("the Presets section", () => {
  it("is one section, and the default mode", async () => {
    expect(document.getElementById("tone-sharing-browse-presets")?.textContent).toBe("Presets");
    expect(document.getElementById("tone-sharing-browse-featured")).toBeNull();
    expect(document.getElementById("tone-sharing-browse-items")).toBeNull();
    expect(document.getElementById("tone-sharing-featured-more")).toBeNull();
  });

  it("searches as you type, and an emptied search is the default list again", () => {
    type("plexi");
    expect(browseState.searchQuery).toBe("plexi");
    expect(stubs.scheduleCommunitySearch).toHaveBeenCalledTimes(1);

    type("");
    expect(browseState.mode).toBe("items");
    expect(stubs.scheduleCommunitySearch).toHaveBeenCalledTimes(2);
  });

  it("searches by tag at once, and Clear drops the search and the tag", () => {
    tag().value = tag().options[1].value;
    tag().dispatchEvent(new Event("change"));
    expect(stubs.scheduleCommunitySearch).toHaveBeenLastCalledWith(0);

    type("lead");
    (document.getElementById("tone-sharing-community-search-clear") as HTMLButtonElement).click();
    expect(browseState.searchQuery).toBe("");
    expect(browseState.tagFilter).toBe("");
    expect(input().value).toBe("");
    expect(stubs.scheduleCommunitySearch).toHaveBeenLastCalledWith(0);
  });

  it("leaves other sections and a shared tone alone", () => {
    browseState.mode = "packs";
    type("anything");
    browseState.mode = "items";
    browseState.activeSharedTarget = { kind: "item", id: "shared" };
    type("");
    expect(stubs.scheduleCommunitySearch).not.toHaveBeenCalled();
  });
});

describe("the Cards/List switch", () => {
  it("starts on cards, and saves and applies the choice", () => {
    expect(feed().classList.contains("tone-sharing-feed--list")).toBe(false);
    expect(layoutButton("cards").getAttribute("aria-pressed")).toBe("true");

    layoutButton("list").click();
    expect(stubs.settings[storageKeys.feedLayout]).toBe("list");
    expect(feed().classList.contains("tone-sharing-feed--list")).toBe(true);
    expect(layoutButton("list").getAttribute("aria-pressed")).toBe("true");
    expect(layoutButton("cards").getAttribute("aria-pressed")).toBe("false");

    layoutButton("cards").click();
    expect(stubs.settings[storageKeys.feedLayout]).toBe("cards");
    expect(feed().classList.contains("tone-sharing-feed--list")).toBe(false);
  });
});
