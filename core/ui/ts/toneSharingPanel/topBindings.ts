/**
 * The header, the modals and the forms around the feed — everything bound once
 * at startup that is not a card action.
 */

import { showNotification } from "../notifications.js";
import { STANDARD_TAGS, renderTagChips } from "../presetTags.js";
import { escapeHtml } from "../utils.js";
import { closeSignInModal, openToneSharingSignInModal, previewSelectedProfileAvatar, saveProfile, sendCode, signOut, verifyCode } from "./account.js";
import { setActionButtonBusy, syncPackPreviewButtons } from "./actionButtons.js";
import { downloadAsset } from "./assets.js";
import { element, setUploadStatus } from "./dom.js";
import { loadStandardBrowsePage, scheduleCommunitySearch, updateBrowseFooter, updateCommunityPresetSearchUi } from "./feed.js";
import { clearPackDetail } from "./packDetail.js";
import { closePackModal, openPackModal, savePack } from "./packEditor.js";
import { clearPreviewPreset, previewPreset } from "./preview.js";
import { closeToneSharingPublishPresetModal, uploadAndPublishItem } from "./publish.js";
import { clearActiveSharedFilter, copyToneSharingShareLink } from "./shareLinks.js";
import { browseCollections, browseState } from "./state.js";

export function bindTopControls(): void {
  // Account chip opens sign-in modal
  element<HTMLButtonElement>("tone-sharing-account-btn")?.addEventListener("click", () => {
    openToneSharingSignInModal();
  });

  element<HTMLButtonElement>("tone-sharing-preview-clear")?.addEventListener("click", () => {
    void clearPreviewPreset();
  });
  element<HTMLButtonElement>("tone-sharing-clear-filter")?.addEventListener("click", () => {
    void clearActiveSharedFilter();
  });
  const communitySearchInput = element<HTMLInputElement>("tone-sharing-community-search-input");
  const tagFilter = element<HTMLSelectElement>("tone-sharing-community-tag-filter");
  if (tagFilter) {
    tagFilter.innerHTML = '<option value="">All tags</option>' + [...STANDARD_TAGS].sort((a, b) => a.localeCompare(b)).map(tag =>
      `<option value="${escapeHtml(tag)}">${escapeHtml(tag)}</option>`).join("");
    tagFilter.value = browseState.tagFilter;
    tagFilter.addEventListener("change", () => {
      browseState.tagFilter = tagFilter.value;
      updateCommunityPresetSearchUi();
      if (browseState.mode === "items" && browseState.activeSharedTarget === null) scheduleCommunitySearch(0);
    });
  }
  const communitySearchClearButton = element<HTMLButtonElement>("tone-sharing-community-search-clear");
  if (communitySearchInput) {
    communitySearchInput.addEventListener("input", () => {
      const nextValue = communitySearchInput.value;
      if (nextValue === browseState.searchQuery) {
        updateCommunityPresetSearchUi();
        return;
      }

      browseState.searchQuery = nextValue;
      updateCommunityPresetSearchUi();
      if (browseState.mode === "items" && browseState.activeSharedTarget === null) {
        scheduleCommunitySearch();
      }
    });
  }
  communitySearchClearButton?.addEventListener("click", () => {
    if (!browseState.searchQuery && !browseState.tagFilter && !(communitySearchInput?.value ?? "")) {
      updateCommunityPresetSearchUi();
      return;
    }

    browseState.searchQuery = "";
    browseState.tagFilter = "";
    if (communitySearchInput) {
      communitySearchInput.value = "";
      communitySearchInput.focus();
    }
    updateCommunityPresetSearchUi();
    if (browseState.mode === "items" && browseState.activeSharedTarget === null) {
      scheduleCommunitySearch();
    }
  });
  element<HTMLButtonElement>("tone-sharing-load-more")?.addEventListener("click", () => {
    if (browseCollections.loadingMore || !browseCollections.hasMore || browseState.activeSharedTarget !== null || browseState.mode !== "items") {
      return;
    }

    browseCollections.loadingMore = true;
    updateBrowseFooter();
    const pendingPage = loadStandardBrowsePage(browseCollections.page + 1, true);
    const version = browseState.requestVersion;
    void pendingPage
      .catch((error) => {
        if (version !== browseState.requestVersion) return;
        setUploadStatus(`Load more failed: ${(error as Error).message}`);
      })
      .finally(() => {
        if (version !== browseState.requestVersion) return;
        browseCollections.loadingMore = false;
        updateBrowseFooter();
      });
  });
  element<HTMLButtonElement>("tone-sharing-page-prev")?.addEventListener("click", () => {
    if (browseState.mode !== "packs" || browseCollections.loadingMore || browseState.activeSharedTarget !== null || browseCollections.page <= 1) {
      return;
    }

    browseCollections.loadingMore = true;
    updateBrowseFooter();
    const pendingPage = loadStandardBrowsePage(browseCollections.page - 1);
    const version = browseState.requestVersion;
    void pendingPage
      .catch((error) => {
        if (version !== browseState.requestVersion) return;
        setUploadStatus(`Failed to load previous page: ${(error as Error).message}`);
      })
      .finally(() => {
        if (version !== browseState.requestVersion) return;
        browseCollections.loadingMore = false;
        updateBrowseFooter();
      });
  });
  element<HTMLButtonElement>("tone-sharing-page-next")?.addEventListener("click", () => {
    if (browseState.mode !== "packs" || browseCollections.loadingMore || browseState.activeSharedTarget !== null || !browseCollections.hasMore) {
      return;
    }

    browseCollections.loadingMore = true;
    updateBrowseFooter();
    const pendingPage = loadStandardBrowsePage(browseCollections.page + 1);
    const version = browseState.requestVersion;
    void pendingPage
      .catch((error) => {
        if (version !== browseState.requestVersion) return;
        setUploadStatus(`Failed to load next page: ${(error as Error).message}`);
      })
      .finally(() => {
        if (version !== browseState.requestVersion) return;
        browseCollections.loadingMore = false;
        updateBrowseFooter();
      });
  });
  element<HTMLButtonElement>("tone-sharing-featured-more-btn")?.addEventListener("click", () => {
    const presetsButton = element<HTMLButtonElement>("tone-sharing-browse-items");
    presetsButton?.click();
  });
  element<HTMLButtonElement>("tone-sharing-signin-modal-close")?.addEventListener("click", () => {
    closeSignInModal();
  });
  element<HTMLElement>("tone-sharing-signin-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeSignInModal();
    }
  });

  element<HTMLButtonElement>("tone-sharing-send-code")?.addEventListener("click", () => {
    void sendCode();
  });
  element<HTMLButtonElement>("tone-sharing-verify")?.addEventListener("click", () => {
    void verifyCode();
  });
  element<HTMLButtonElement>("tone-sharing-logout")?.addEventListener("click", () => {
    void signOut();
  });
  element<HTMLButtonElement>("tone-sharing-save-profile")?.addEventListener("click", () => {
    void saveProfile();
  });
  element<HTMLInputElement>("tone-sharing-avatar-image")?.addEventListener("change", () => {
    previewSelectedProfileAvatar();
  });

  // Create Pack / Edit Pack modal
  element<HTMLButtonElement>("tone-sharing-open-pack-modal")?.addEventListener("click", () => {
    void openPackModal();
  });
  element<HTMLButtonElement>("tone-sharing-pack-modal-close")?.addEventListener("click", () => {
    closePackModal();
  });
  element<HTMLButtonElement>("tone-sharing-pack-modal-cancel")?.addEventListener("click", () => {
    closePackModal();
  });
  element<HTMLButtonElement>("tone-sharing-save-pack-draft")?.addEventListener("click", () => {
    void savePack(false);
  });
  element<HTMLButtonElement>("tone-sharing-create-pack")?.addEventListener("click", () => {
    void savePack(true);
  });
  element<HTMLElement>("tone-sharing-pack-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closePackModal();
    }
  });

  // Pack view modal (Netflix-style)
  element<HTMLButtonElement>("tone-sharing-pack-view-close")?.addEventListener("click", () => {
    clearPackDetail();
  });
  element<HTMLElement>("tone-sharing-pack-view-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      clearPackDetail();
    }
  });
  element<HTMLElement>("tone-sharing-pack-view-actions")?.addEventListener("click", async (event) => {
    const target = event.target as HTMLElement;
    const button = target.closest("[data-pack-action]") as HTMLButtonElement | null;
    if (!button) {
      return;
    }

    const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
    const packId = modal?.dataset.packId ?? "";
    if (!packId) {
      return;
    }

    if (button.dataset.packAction === "share-pack") {
      try {
        const link = await copyToneSharingShareLink({ kind: "pack", id: packId });
        setUploadStatus("Pack share link copied to clipboard.");
        showNotification("Pack share link copied", link);
      } catch (error) {
        setUploadStatus(`Share failed: ${(error as Error).message}`);
      }
      return;
    }

    const restoreBusy = setActionButtonBusy(button, "Downloading...");
    try {
      await downloadAsset("pack", packId);
    } catch (error) {
      setUploadStatus(`Download failed: ${(error as Error).message}`);
    } finally {
      restoreBusy();
    }
  });
  element<HTMLElement>("tone-sharing-pack-view-presets")?.addEventListener("click", async (event) => {
    const target = event.target as HTMLElement;
    const button = target.closest("[data-pack-action]") as HTMLButtonElement | null;
    if (!button) {
      return;
    }
    const action = button.dataset.packAction!;
    const itemId = button.dataset.itemId!;
    const itemTitle = button.dataset.itemTitle!;
    if (action === "preview") {
      const restoreBusy = setActionButtonBusy(button, "Previewing...");
      try {
        await previewPreset(itemId, itemTitle);
        const presetsEl = element<HTMLElement>("tone-sharing-pack-view-presets");
        if (presetsEl) {
          syncPackPreviewButtons(presetsEl, itemId);
        } else {
          restoreBusy();
        }
        setUploadStatus("Preset preview applied (not installed).");
      } catch (error) {
        setUploadStatus(`Preview failed: ${(error as Error).message}`);
        restoreBusy();
      }
    } else if (action === "download") {
      const restoreBusy = setActionButtonBusy(button, "Downloading...");
      try {
        await downloadAsset("item", itemId);
      } catch (error) {
        setUploadStatus(`Download failed: ${(error as Error).message}`);
      } finally {
        restoreBusy();
      }
    }
  });

  // Populate from the standard tag list, then wire tag chip toggles in publish modal
  const toneSharingTagsPicker = element<HTMLElement>("tone-sharing-tags-picker");
  if (toneSharingTagsPicker) {
    renderTagChips(toneSharingTagsPicker, STANDARD_TAGS, "tone-sharing-tag-chip");
    toneSharingTagsPicker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((btn) => {
      btn.addEventListener("click", () => btn.classList.toggle("active"));
    });
  }

  // Publish preset modal (opened from preset chooser)
  element<HTMLButtonElement>("tone-sharing-publish-modal-close")?.addEventListener("click", () => {
    closeToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-publish-modal-cancel")?.addEventListener("click", () => {
    closeToneSharingPublishPresetModal();
  });
  element<HTMLButtonElement>("tone-sharing-upload-item")?.addEventListener("click", () => {
    void uploadAndPublishItem();
  });
  element<HTMLElement>("tone-sharing-publish-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeToneSharingPublishPresetModal();
    }
  });

  // Pack assign controls in publish modal
  element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.addEventListener("click", () => {
    const btn = element<HTMLButtonElement>("tone-sharing-add-to-new-pack");
    if (!btn) return;
    btn.classList.toggle("active");
    if (btn.classList.contains("active")) {
      const sel = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
      if (sel) sel.value = "";
    }
  });
  element<HTMLSelectElement>("tone-sharing-pack-assign-select")?.addEventListener("change", () => {
    const sel = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
    if (sel?.value) {
      element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.remove("active");
    }
  });
}
