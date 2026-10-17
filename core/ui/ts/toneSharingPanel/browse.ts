/**
 * The browse dispatcher: which feed each mode shows, and the refresh that has to
 * happen once an install has changed what "installed" means.
 */

import { escapeHtml } from "../utils.js";
import { apiFetch, isToneSharingAdmin } from "./api.js";
import { element, setUploadStatus } from "./dom.js";
import { applyFeaturedLayout, buildSingleRow, loadStandardBrowsePage, renderFeedRows, resetBrowseCollections, updateActiveSharedFilter, updateBrowseFooter, updateCommunityPresetSearchUi, updateFeaturedMoreLink } from "./feed.js";
import { renderInstalledPacks } from "./installedPacks.js";
import { clearPackDetail, viewPack } from "./packDetail.js";
import { setBrowseLoaders } from "./refresh.js";
import { browseState, toneSharingState } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingRow } from "./types.js";

export async function loadBrowse(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }
  clearPackDetail();
  feed.innerHTML = `<div class="tone-sharing-status">Loading...</div>`;
  resetBrowseCollections();
  // Every reply is checked against this, and rendered with it, before it is shown: switching tab
  // starts another load, and a slow reply for the old tab (/home most often) used to land last and
  // replace the new tab's feed under the new tab's name. renderFeedRows' own default version is
  // read when it is called, after the await, so it could never catch that.
  const loadVersion = browseState.requestVersion;
  const isStale = (): boolean => browseState.requestVersion !== loadVersion;
  browseState.featuredHiddenPresetCount = 0;
  updateActiveSharedFilter();
  updateCommunityPresetSearchUi();
  updateFeaturedMoreLink();
  updateBrowseFooter();

  try {
    if (browseState.activeSharedTarget) {
      if (browseState.activeSharedTarget.kind === "item") {
        const itemResult = await apiFetch<{ item: ToneSharingItem }>(`/items/${encodeURIComponent(browseState.activeSharedTarget.id)}`);
        if (isStale()) return;
        await renderFeedRows([buildSingleRow("Shared Preset", [itemResult.item], "item")], loadVersion);
        if (isStale()) return;
        updateFeaturedMoreLink();
        setUploadStatus(`Opened shared preset: ${itemResult.item.title}`);
        return;
      }

      const packId = browseState.activeSharedTarget.id;
      const packResult = await apiFetch<{ pack: ToneSharingPack }>(`/packs/${encodeURIComponent(packId)}`);
      if (isStale()) return;
      await renderFeedRows([buildSingleRow("Shared Pack", [packResult.pack], "pack")], loadVersion);
      if (isStale()) return;
      await viewPack(packId);
      if (isStale()) return;
      updateFeaturedMoreLink();
      setUploadStatus(`Opened shared pack: ${packResult.pack.title}`);
      return;
    }

    if (browseState.mode === "featured") {
      const home = await apiFetch<{ rows: ToneSharingRow[] }>("/home");
      if (isStale()) return;
      const featured = applyFeaturedLayout(home.rows);
      browseState.featuredHiddenPresetCount = featured.hiddenPresetCount;
      await renderFeedRows(featured.rows, loadVersion);
      if (isStale()) return;
      updateFeaturedMoreLink();
      return;
    }

    if (browseState.mode === "items") {
      await loadStandardBrowsePage(1);
      return;
    }

    if (browseState.mode === "packs") {
      await loadStandardBrowsePage(1);
      return;
    }

    if (browseState.mode === "installed") {
      await renderInstalledPacks();
      return;
    }

    if (browseState.mode === "review") {
      if (!isToneSharingAdmin()) {
        feed.innerHTML = `<div class="tone-sharing-status">Admin access required.</div>`;
        return;
      }
      const [items, packs] = await Promise.all([
        apiFetch<{ items: ToneSharingItem[] }>("/items/pending/list"),
        apiFetch<{ packs: ToneSharingPack[] }>("/packs/pending/list"),
      ]);
      if (isStale()) return;
      await renderFeedRows([
        buildSingleRow("Presets Awaiting Approval", items.items, "item"),
        buildSingleRow("Packs Awaiting Approval", packs.packs, "pack"),
      ], loadVersion);
      return;
    }

    await loadMine();
  } catch (error) {
    if (isStale()) return;
    browseState.featuredHiddenPresetCount = 0;
    updateFeaturedMoreLink();
    feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${escapeHtml((error as Error).message)}</div>`;
  }
}

export async function loadMine(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  // Showing My Content is a load of its own: one still in flight for another tab must not land on
  // it. Fetching for something else (the list of my items) leaves the feed alone.
  const showing = browseState.mode === "mine";
  const loadVersion = showing ? ++browseState.requestVersion : browseState.requestVersion;
  const isStale = (): boolean => browseState.requestVersion !== loadVersion || browseState.mode !== "mine";

  if (showing) {
    clearPackDetail();
  }

  if (!toneSharingState.user) {
    if (browseState.mode === "mine") {
      feed.innerHTML = `<div class="tone-sharing-status">Sign in to view your content.</div>`;
    }
    return;
  }

  try {
    const [itemsData, packsData] = await Promise.all([
      apiFetch<{ items: ToneSharingItem[] }>("/items/me/list"),
      apiFetch<{ packs: ToneSharingPack[] }>("/packs/me/list")
    ]);

    toneSharingState.myItems = itemsData.items;

    if (showing && !isStale()) {
      await renderFeedRows([
        buildSingleRow("My Presets on Tone Sharing", itemsData.items, "item"),
        buildSingleRow("My Packs on Tone Sharing", packsData.packs, "pack")
      ], loadVersion);
    }
  } catch (error) {
    if (showing && !isStale()) {
      feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${escapeHtml((error as Error).message)}</div>`;
    }
  }
}

export async function refreshBrowseResultsAfterInstall(): Promise<void> {
  try {
    if (browseState.mode === "featured" || browseState.mode === "items" || browseState.mode === "packs") {
      await loadBrowse();
    } else if (browseState.mode === "installed") {
      await renderInstalledPacks();
    } else if (browseState.mode === "mine") {
      await loadMine();
    }

    const packViewModal = element<HTMLElement>("tone-sharing-pack-view-modal");
    if (packViewModal && packViewModal.style.display === "flex") {
      const activePackId = (packViewModal.dataset.packId ?? "").trim();
      if (activePackId) {
        await viewPack(activePackId);
      }
    }
  } catch {
  }
}

setBrowseLoaders({ reload: loadBrowse, refreshAfterInstall: refreshBrowseResultsAfterInstall });
