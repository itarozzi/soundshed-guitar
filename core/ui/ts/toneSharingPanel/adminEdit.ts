/**
 * Admin tools for anyone's published tones: editing a preset's or pack's details,
 * featuring it on the home page, and deleting it.
 *
 * The server checks the admin role on every one of these calls; hiding the buttons
 * from other users is only so they are not offered something that would fail.
 */

import { showConfirm } from "../dialogs.js";
import { STANDARD_TAGS, renderTagChips } from "../presetTags.js";
import { escapeHtml } from "../utils.js";
import { apiFetch, isToneSharingAdmin } from "./api.js";
import { deleteAsset } from "./assets.js";
import { element, setText, setUploadStatus } from "./dom.js";
import { canFeature } from "./format.js";
import { buildPackImageVariants } from "./images.js";
import { uploadToneSharingAsset } from "./itemUpload.js";
import { clearPackDetail, viewPack } from "./packDetail.js";
import { requestBrowseReload } from "./refresh.js";
import { browseCollections, browseState } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingPackDetails } from "./types.js";

type AdminTarget = { kind: "item" | "pack"; id: string };

type AdminEditSession = AdminTarget & {
  title: string;
  featured: boolean;
  approved: boolean;
  /** The pack's presets as loaded, in order; empty for an item. */
  packItemIds: string[];
};

let session: AdminEditSession | null = null;
let saving = false;

export async function setFeatured(target: AdminTarget, featured: boolean): Promise<void> {
  const path = target.kind === "item" ? `/items/${target.id}/featured` : `/packs/${target.id}/featured`;
  await apiFetch(path, {
    method: "PATCH",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ featured }),
  });
  // Keep the cached page in step, so the next render of it shows the new state.
  const cached = target.kind === "item"
    ? browseCollections.items.find((entry) => entry.id === target.id)
    : browseCollections.packs.find((entry) => entry.id === target.id);
  if (cached) {
    cached.featured = featured;
  }
}

/** Asks first, then deletes; true when it was deleted. A deleted pack's open view is closed. */
export async function confirmAndDelete(target: AdminTarget, title: string): Promise<boolean> {
  const noun = target.kind === "item" ? "preset" : "pack";
  const consequence = target.kind === "item"
    ? "It is removed from every pack it is in, and its ratings and favourites go with it."
    : "The presets in it are kept; only the pack is removed.";
  const confirmed = await showConfirm(
    `Delete the ${noun} "${title}" from Tone Sharing for everyone?\n\n${consequence}\n\nThis cannot be undone.`,
    `Delete ${target.kind === "item" ? "Preset" : "Pack"}`,
  );
  if (!confirmed) {
    return false;
  }
  setUploadStatus(`Deleting ${noun}...`);
  await deleteAsset(target.kind, target.id);
  setUploadStatus(`${target.kind === "item" ? "Preset" : "Pack"} deleted.`);
  if (target.kind === "pack" && openPackViewId() === target.id) {
    clearPackDetail();
  }
  return true;
}

/** A message replaces the status line, an empty one clears it; none leaves it as it is. */
function setBusy(busy: boolean, message?: string): void {
  saving = busy;
  for (const id of ["tone-sharing-admin-edit-save", "tone-sharing-admin-edit-delete", "tone-sharing-admin-edit-cancel", "tone-sharing-admin-edit-close"]) {
    const button = element<HTMLButtonElement>(id);
    if (button) button.disabled = busy;
  }
  if (message !== undefined) setText("tone-sharing-admin-edit-status", message);
}

function describeStatus(target: ToneSharingItem | ToneSharingPack): string {
  const status = (target.moderationStatus ?? "approved").replace(/_/g, " ");
  const creator = target.creatorDisplayName || target.creatorProfileHandle || target.creatorEmail || target.creatorUserId || "unknown";
  return `Status: ${status} · Creator: ${creator}`;
}

function renderPackItems(items: ToneSharingPackDetails["items"]): void {
  const host = element<HTMLElement>("tone-sharing-admin-edit-items");
  if (!host) return;
  if (!items.length) {
    host.innerHTML = `<div class="tone-sharing-select-item">No presets in this pack.</div>`;
    return;
  }
  host.innerHTML = [...items]
    .sort((a, b) => a.sortOrder - b.sortOrder)
    .map((item) => `
        <label class="tone-sharing-select-item">
          <input type="checkbox" data-admin-pack-item-id="${escapeHtml(item.itemId)}" checked />
          <span>${escapeHtml(item.title)}</span>
        </label>`)
    .join("");
}

function syncFeaturedControl(): void {
  const checkbox = element<HTMLInputElement>("tone-sharing-admin-edit-featured");
  if (!checkbox || !session) return;
  checkbox.checked = session.featured;
  checkbox.disabled = !session.approved && !session.featured;
  setText("tone-sharing-admin-edit-featured-hint", session.approved ? "" : "Only approved content can be featured.");
}

export async function openAdminEditModal(target: AdminTarget): Promise<void> {
  const modal = element<HTMLElement>("tone-sharing-admin-edit-modal");
  if (!modal || !isToneSharingAdmin()) {
    return;
  }
  session = null;
  modal.dataset.kind = target.kind;
  setText("tone-sharing-admin-edit-title", target.kind === "item" ? "Edit Preset" : "Edit Pack");
  setText("tone-sharing-admin-edit-meta", "");
  const nameInput = element<HTMLInputElement>("tone-sharing-admin-edit-name");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-admin-edit-description");
  const imageInput = element<HTMLInputElement>("tone-sharing-admin-edit-image");
  if (nameInput) nameInput.value = "";
  if (descriptionInput) descriptionInput.value = "";
  if (imageInput) imageInput.value = "";
  const itemsHost = element<HTMLElement>("tone-sharing-admin-edit-items");
  if (itemsHost) itemsHost.innerHTML = "";
  setBusy(true, "Loading...");
  modal.style.display = "flex";

  try {
    let record: ToneSharingItem | ToneSharingPack;
    let packItemIds: string[] = [];
    if (target.kind === "item") {
      record = (await apiFetch<{ item: ToneSharingItem }>(`/items/${target.id}`)).item;
      const tagsHost = element<HTMLElement>("tone-sharing-admin-edit-tags");
      if (tagsHost) {
        // Tags the standard list lacks stay selectable, so saving does not drop them.
        const current = (record as ToneSharingItem).tags ?? [];
        const all = [...STANDARD_TAGS, ...current.filter((tag) => !STANDARD_TAGS.includes(tag))];
        renderTagChips(tagsHost, all, "tone-sharing-tag-chip");
        tagsHost.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((chip) => {
          chip.classList.toggle("active", current.includes(chip.dataset.tag ?? ""));
        });
      }
    } else {
      const details = await apiFetch<ToneSharingPackDetails>(`/packs/${target.id}`);
      record = details.pack;
      packItemIds = [...details.items].sort((a, b) => a.sortOrder - b.sortOrder).map((item) => item.itemId);
      renderPackItems(details.items);
    }

    session = {
      ...target,
      title: record.title,
      featured: Boolean(record.featured),
      approved: canFeature(record.moderationStatus),
      packItemIds,
    };
    if (nameInput) nameInput.value = record.title;
    if (descriptionInput) descriptionInput.value = record.description ?? "";
    setText("tone-sharing-admin-edit-meta", describeStatus(record));
    syncFeaturedControl();
    setBusy(false, "");
  } catch (error) {
    setBusy(false, `Load failed: ${(error as Error).message}`);
    const save = element<HTMLButtonElement>("tone-sharing-admin-edit-save");
    const remove = element<HTMLButtonElement>("tone-sharing-admin-edit-delete");
    if (save) save.disabled = true;
    if (remove) remove.disabled = true;
  }
}

export function closeAdminEditModal(force = false): void {
  if (saving && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-admin-edit-modal");
  if (modal) {
    modal.style.display = "none";
  }
  session = null;
}

function openPackViewId(): string {
  const view = element<HTMLElement>("tone-sharing-pack-view-modal");
  return view && view.style.display === "flex" ? (view.dataset.packId ?? "") : "";
}

/** Reloads what is on screen after an admin change: the feed, and the pack view if it was open. */
export async function refreshAfterAdminChange(): Promise<void> {
  const packId = openPackViewId();
  await requestBrowseReload();
  if (packId) {
    try {
      await viewPack(packId);
    } catch {
      // Gone or no longer visible; the feed has already been redrawn.
    }
  }
}

export async function saveAdminEdit(): Promise<void> {
  if (!session || saving) {
    return;
  }
  const current = session;
  const title = element<HTMLInputElement>("tone-sharing-admin-edit-name")?.value.trim() ?? "";
  const description = element<HTMLTextAreaElement>("tone-sharing-admin-edit-description")?.value.trim() ?? "";
  if (!title) {
    setText("tone-sharing-admin-edit-status", "Title is required.");
    return;
  }
  const featured = element<HTMLInputElement>("tone-sharing-admin-edit-featured")?.checked ?? current.featured;

  setBusy(true, "Saving...");
  try {
    if (current.kind === "item") {
      const tags = Array.from(document.querySelectorAll<HTMLButtonElement>("#tone-sharing-admin-edit-tags .tone-sharing-tag-chip.active"))
        .map((chip) => chip.dataset.tag ?? "")
        .filter(Boolean);
      await apiFetch(`/items/${current.id}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, tags }),
      });
    } else {
      const imageFile = element<HTMLInputElement>("tone-sharing-admin-edit-image")?.files?.[0] ?? null;
      let thumbnailAssetId: string | undefined;
      if (imageFile) {
        setText("tone-sharing-admin-edit-status", "Uploading image...");
        const variants = await buildPackImageVariants(imageFile);
        thumbnailAssetId = await uploadToneSharingAsset("thumbnail", variants.small.blob);
      }
      await apiFetch(`/packs/${current.id}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, ...(thumbnailAssetId ? { thumbnailAssetId } : {}) }),
      });
      const keptItemIds = Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-admin-edit-items input[data-admin-pack-item-id]:checked"))
        .map((input) => input.dataset.adminPackItemId ?? "")
        .filter(Boolean);
      if (keptItemIds.join("\n") !== current.packItemIds.join("\n")) {
        await apiFetch(`/packs/${current.id}/items`, {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ itemIds: keptItemIds }),
        });
      }
    }

    if (featured !== current.featured) {
      await setFeatured(current, featured);
    }

    setUploadStatus(`${current.kind === "item" ? "Preset" : "Pack"} updated.`);
    setBusy(false);
    closeAdminEditModal(true);
    await refreshAfterAdminChange();
  } catch (error) {
    setBusy(false, `Save failed: ${(error as Error).message}`);
  }
}

async function deleteFromModal(): Promise<void> {
  if (!session || saving) {
    return;
  }
  const current = session;
  const title = element<HTMLInputElement>("tone-sharing-admin-edit-name")?.value.trim() || current.title;
  try {
    if (!(await confirmAndDelete(current, title))) {
      return;
    }
    closeAdminEditModal(true);
    await refreshAfterAdminChange();
  } catch (error) {
    setText("tone-sharing-admin-edit-status", `Delete failed: ${(error as Error).message}`);
  }
}

/** Whether a card in the current browse mode gets the admin buttons. */
export function showAdminCardActions(): boolean {
  return isToneSharingAdmin() && browseState.mode !== "installed";
}

export function bindAdminEditModal(): void {
  const tagsHost = element<HTMLElement>("tone-sharing-admin-edit-tags");
  tagsHost?.addEventListener("click", (event) => {
    const chip = (event.target as HTMLElement).closest<HTMLButtonElement>(".tone-sharing-tag-chip");
    chip?.classList.toggle("active");
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-save")?.addEventListener("click", () => {
    void saveAdminEdit();
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-delete")?.addEventListener("click", () => {
    void deleteFromModal();
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-cancel")?.addEventListener("click", () => {
    closeAdminEditModal();
  });
  element<HTMLButtonElement>("tone-sharing-admin-edit-close")?.addEventListener("click", () => {
    closeAdminEditModal();
  });
  element<HTMLElement>("tone-sharing-admin-edit-modal")?.addEventListener("mousedown", (event) => {
    if (event.target === event.currentTarget) {
      closeAdminEditModal();
    }
  });
}
