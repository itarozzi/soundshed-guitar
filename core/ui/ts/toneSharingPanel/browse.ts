/**
 * The browse dispatcher: which feed each mode shows, and the refresh that has to
 * happen once an install has changed what "installed" means.
 */

import { renderAiSearchView } from "./aiSearch.js";
import { apiFetch, isToneSharingAdmin } from "./api.js";
import { element, setUploadStatus } from "./dom.js";
import { applyFeaturedLayout, buildSingleRow, loadStandardBrowsePage, renderFeedRows, resetBrowseCollections, updateActiveSharedFilter, updateBrowseFooter, updateCommunityPresetSearchUi, updateFeaturedMoreLink } from "./feed.js";
import { renderInstalledPacks } from "./installedPacks.js";
import { clearPackDetail, viewPack } from "./packDetail.js";
import { setBrowseLoaders } from "./refresh.js";
import { browseState, isAiToneSearchEnabled, toneSharingState } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingRow } from "./types.js";

export async function loadBrowse(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }
  if (browseState.mode === "ai-search" && !isAiToneSearchEnabled()) {
    browseState.mode = "featured";
  }
  clearPackDetail();
  feed.innerHTML = `<div class="tone-sharing-status">Loading...</div>`;
  resetBrowseCollections();
  const loadVersion = browseState.requestVersion;
  browseState.featuredHiddenPresetCount = 0;
  updateActiveSharedFilter();
  updateCommunityPresetSearchUi();
  updateFeaturedMoreLink();
  updateBrowseFooter();

  try {
    if (browseState.activeSharedTarget) {
      if (browseState.activeSharedTarget.kind === "item") {
        const itemResult = await apiFetch<{ item: ToneSharingItem }>(`/items/${browseState.activeSharedTarget.id}`);
        await renderFeedRows([buildSingleRow("Shared Preset", [itemResult.item], "item")]);
        updateFeaturedMoreLink();
        setUploadStatus(`Opened shared preset: ${itemResult.item.title}`);
        return;
      }

      const packResult = await apiFetch<{ pack: ToneSharingPack }>(`/packs/${browseState.activeSharedTarget.id}`);
      await renderFeedRows([buildSingleRow("Shared Pack", [packResult.pack], "pack")]);
      await viewPack(browseState.activeSharedTarget.id);
      updateFeaturedMoreLink();
      setUploadStatus(`Opened shared pack: ${packResult.pack.title}`);
      return;
    }

    if (browseState.mode === "featured") {
      const home = await apiFetch<{ rows: ToneSharingRow[] }>("/home");
      const featured = applyFeaturedLayout(home.rows);
      browseState.featuredHiddenPresetCount = featured.hiddenPresetCount;
      await renderFeedRows(featured.rows);
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

    if (browseState.mode === "ai-search") {
      renderAiSearchView();
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
      await renderFeedRows([
        buildSingleRow("Presets Awaiting Approval", items.items, "item"),
        buildSingleRow("Packs Awaiting Approval", packs.packs, "pack"),
      ]);
      return;
    }

    await loadMine();
  } catch (error) {
    if (browseState.requestVersion !== loadVersion) return;
    browseState.featuredHiddenPresetCount = 0;
    updateFeaturedMoreLink();
    feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${(error as Error).message}</div>`;
  }
}

export async function loadMine(): Promise<void> {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  if (browseState.mode === "mine") {
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

    if (browseState.mode === "mine") {
      await renderFeedRows([
        buildSingleRow("My Presets on Tone Sharing", itemsData.items, "item"),
        buildSingleRow("My Packs on Tone Sharing", packsData.packs, "pack")
      ]);
    }
  } catch (error) {
    if (browseState.mode === "mine") {
      feed.innerHTML = `<div class="tone-sharing-status">Load failed: ${(error as Error).message}</div>`;
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
