/**
 * Delegated click handling for the browse area: one listener per region rather
 * than one per card, so re-rendering a feed does not have to rebind anything.
 */

import { showConfirm } from "../dialogs.js";
import { showNotification } from "../notifications.js";
import { setActionButtonBusy, syncFeatureToggleButton } from "./actionButtons.js";
import { confirmAndDelete, openAdminEditModal, refreshAfterAdminChange, setFeatured } from "./adminEdit.js";
import { deleteAsset, downloadAsset, moderateTarget } from "./assets.js";
import { loadBrowse, loadMine } from "./browse.js";
import { element, setUploadStatus } from "./dom.js";
import { updateActiveSharedFilter, updateCommunityPresetSearchUi, updateFeaturedMoreLink } from "./feed.js";
import { buildInstalledPackDeletionPlan, deleteInstalledPackById } from "./installedPacks.js";
import { clearPackDetail, viewPack } from "./packDetail.js";
import { openPackModal } from "./packEditor.js";
import { previewPreset } from "./preview.js";
import { copyToneSharingShareLink } from "./shareLinks.js";
import { browseState, toneSharingState } from "./state.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";

export function bindBrowseActions(): void {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) {
    return;
  }

  feed.addEventListener("click", async (event) => {
    const target = event.target as HTMLElement;
    const button = target.closest("button[data-action]") as HTMLButtonElement | null;
    const card = target.closest(".tone-sharing-card-item") as HTMLElement | null;
    if (!button || !card) {
      return;
    }

    const kind = (card.dataset.kind ?? "item") as "item" | "pack";
    const id = card.dataset.id ?? "";
    if (!id) {
      return;
    }

    if (button.dataset.action === "delete-installed") {
      const title = card.querySelector(".tone-sharing-card-item-title")?.textContent?.trim() || id;
      const pack = toneSharingState.installedPacks.find((entry) => entry.id === id);
      const plan = pack ? buildInstalledPackDeletionPlan(pack) : null;
      const presetCount = pack?.presetIds.length ?? 0;
      const resourceCount = pack?.resources.length ?? 0;
      const archiveLine = pack?.archiveFileName ? `\nArchive: ${pack.archiveFileName}` : "";
      const summaryLines = plan
        ? `\nPresets to remove: ${plan.removablePresetIds.length}`
          + (plan.missingPresetIds.length > 0 ? ` (${plan.missingPresetIds.length} already missing from library)` : "")
          + `\nLinked resources to remove: ${plan.removableResourceEntries.length}`
          + (plan.preservedResourceEntries.length > 0 ? `\nResources kept (used by other presets): ${plan.preservedResourceEntries.length}` : "")
        : "";
      const confirmed = await showConfirm(
        `Delete installed pack "${title}"?\nPresets in pack: ${presetCount}\nResources in pack: ${resourceCount}${summaryLines}${archiveLine}\n\nAll presets from this pack will be removed. Resources still in use by other presets will be kept.`,
        "Delete Installed Pack",
      );
      if (!confirmed) {
        return;
      }

      try {
        setUploadStatus("Deleting installed pack...");
        const result = await deleteInstalledPackById(id, plan ?? undefined);
        setUploadStatus(`Installed pack deleted. Removed ${result.removablePresetIds.length} preset(s) and ${result.removableResourceEntries.length} resource(s).`);
      } catch (error) {
        setUploadStatus(`Delete failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "view" && kind === "pack") {
      try {
        await viewPack(id);
        setUploadStatus("Pack details loaded.");
      } catch (error) {
        setUploadStatus(`View failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "edit-pack" && kind === "pack") {
      void openPackModal(id);
      return;
    }

    if (button.dataset.action === "preview" && kind === "item") {
      const itemTitle = card.dataset.title ?? "";
      const restoreBusy = setActionButtonBusy(button, "Previewing...");
      try {
        await previewPreset(id, itemTitle);
        setUploadStatus("Preset preview applied (not installed).");
      } catch (error) {
        setUploadStatus(`Preview failed: ${(error as Error).message}`);
      } finally {
        restoreBusy();
      }
      return;
    }

    if (button.dataset.action === "download") {
      const restoreBusy = setActionButtonBusy(button, "Downloading...");
      try {
        await downloadAsset(kind, id);
      } catch (error) {
        setUploadStatus(`Download failed: ${(error as Error).message}`);
      } finally {
        restoreBusy();
      }
      return;
    }

    if (button.dataset.action === "share") {
      try {
        const link = await copyToneSharingShareLink({ kind, id });
        setUploadStatus("Share link copied to clipboard.");
        showNotification("Share link copied", link);
      } catch (error) {
        setUploadStatus(`Share failed: ${(error as Error).message}`);
      }
      return;
    }

    if ((button.dataset.action === "approve" || button.dataset.action === "reject") && browseState.mode === "review") {
      try {
        const action = button.dataset.action === "approve" ? "approve" : "reject";
        await moderateTarget(kind, id, action);
        setUploadStatus(`${kind === "item" ? "Preset" : "Pack"} ${action === "approve" ? "approved" : "rejected"}.`);
        await loadBrowse();
      } catch (error) {
        setUploadStatus(`Moderation failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "feature") {
      const featured = button.dataset.featured !== "true";
      const restoreBusy = setActionButtonBusy(button, featured ? "Featuring..." : "Unfeaturing...");
      try {
        await setFeatured({ kind, id }, featured);
        restoreBusy();
        syncFeatureToggleButton(button, featured);
        setUploadStatus(featured ? "Featured on the home page." : "Removed from the home page.");
        if (browseState.mode === "featured") {
          await loadBrowse();
        }
      } catch (error) {
        restoreBusy();
        setUploadStatus(`Feature change failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "admin-edit") {
      void openAdminEditModal({ kind, id });
      return;
    }

    if (button.dataset.action === "admin-delete") {
      const title = card.dataset.title || id;
      try {
        if (await confirmAndDelete({ kind, id }, title)) {
          await refreshAfterAdminChange();
        }
      } catch (error) {
        setUploadStatus(`Delete failed: ${(error as Error).message}`);
      }
      return;
    }

    if (button.dataset.action === "delete") {
      const title = card.querySelector(".tone-sharing-card-item-title")?.textContent?.trim() || `${kind} ${id}`;
      const confirmed = await showConfirm(`Delete ${kind} "${title}"? This cannot be undone.`);
      if (!confirmed) {
        return;
      }

      try {
        setUploadStatus(`Deleting ${kind === "item" ? "preset" : "pack"}...`);
        await deleteAsset(kind, id);
        setUploadStatus(`${kind === "item" ? "Preset" : "Pack"} deleted.`);
        await loadMine();
        void loadBrowse().catch(() => {
        });
      } catch (error) {
        setUploadStatus(`Delete failed: ${(error as Error).message}`);
      }
    }

  });
}

export function bindBrowseModeButtons(): void {
  const modes: Array<{ id: string; mode: typeof browseState.mode }> = [
    { id: "tone-sharing-browse-featured", mode: "featured" },
    { id: "tone-sharing-browse-items", mode: "items" },
    { id: "tone-sharing-browse-packs", mode: "packs" },
    { id: "tone-sharing-browse-review", mode: "review" },
    { id: "tone-sharing-browse-installed", mode: "installed" },
    { id: "tone-sharing-browse-mine", mode: "mine" }
  ];

  // The Tone Builder lives in its own feature (toneBuilder/panel.ts) and opens on this event.
  const toneBuilderButton = element<HTMLButtonElement>("tone-sharing-tone-builder");
  toneBuilderButton?.addEventListener("click", () => {
    document.dispatchEvent(new CustomEvent("tone-builder:open", { detail: {} }));
  });
  const syncToneBuilderButtonVisibility = () => {
    if (toneBuilderButton) {
      toneBuilderButton.style.display = isFeatureEnabled(Features.ToneBuilder) ? "" : "none";
    }
  };

  const setActive = () => {
    syncToneBuilderButtonVisibility();
    for (const entry of modes) {
      const button = element<HTMLButtonElement>(entry.id);
      if (button) {
        button.classList.toggle("active", entry.mode === browseState.mode);
      }
    }
    updateCommunityPresetSearchUi();
  };

  for (const entry of modes) {
    const button = element<HTMLButtonElement>(entry.id);
    if (!button) {
      continue;
    }
    button.addEventListener("click", async () => {
      browseState.activeSharedTarget = null;
      updateActiveSharedFilter();
      browseState.mode = entry.mode;
      setActive();
      clearPackDetail();
      if (browseState.mode === "mine") {
        await loadMine();
      } else {
        await loadBrowse();
      }
      updateFeaturedMoreLink();
    });
  }

  setActive();
}
