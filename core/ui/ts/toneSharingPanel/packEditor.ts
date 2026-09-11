/**
 * Authoring a pack: the modal, the draft list it can be added to, and the save
 * that either keeps it a draft or publishes it.
 */

import { apiFetch, buildApiUrl } from "./api.js";
import { loadBrowse, loadMine } from "./browse.js";
import { element, setText } from "./dom.js";
import { buildPackImageVariants } from "./images.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem, ToneSharingPack, ToneSharingPackDetails } from "./types.js";

let publishPackInFlight = false;

let editingPackId: string | null = null;

export function setPublishPackBusy(busy: boolean, message?: string): void {
  publishPackInFlight = busy;
  const closeButton = element<HTMLButtonElement>("tone-sharing-pack-modal-close");
  const cancelButton = element<HTMLButtonElement>("tone-sharing-pack-modal-cancel");
  const saveDraftButton = element<HTMLButtonElement>("tone-sharing-save-pack-draft");
  const publishButton = element<HTMLButtonElement>("tone-sharing-create-pack");
  const titleInput = element<HTMLInputElement>("tone-sharing-pack-title");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-pack-description");
  const imageInput = element<HTMLInputElement>("tone-sharing-pack-image");
  const progress = element<HTMLElement>("tone-sharing-pack-progress");
  [closeButton, cancelButton, saveDraftButton, publishButton, titleInput, descriptionInput, imageInput].forEach((control) => {
    if (control) {
      control.disabled = busy;
    }
  });
  document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[type='checkbox']").forEach((checkbox) => {
    checkbox.disabled = busy;
  });
  if (progress) {
    progress.style.display = busy ? "flex" : "none";
    const label = progress.querySelector<HTMLElement>(".tone-sharing-progress-label");
    if (label) {
      label.textContent = message ?? "Working...";
    }
  }
}

export async function openPackModal(packId?: string, preCheckItemId?: string): Promise<void> {
  editingPackId = packId ?? null;
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (!modal) {
    return;
  }
  setPublishPackBusy(false, "Publishing pack...");

  const titleEl = element<HTMLInputElement>("tone-sharing-pack-title");
  const descEl = element<HTMLTextAreaElement>("tone-sharing-pack-description");
  const imageEl = element<HTMLInputElement>("tone-sharing-pack-image");
  const titleHeader = element<HTMLElement>("tone-sharing-pack-modal-title");

  if (titleEl) titleEl.value = "";
  if (descEl) descEl.value = "";
  if (imageEl) imageEl.value = "";
  setText("tone-sharing-pack-status", "");

  let checkedItemIds = new Set<string>(preCheckItemId ? [preCheckItemId] : []);

  if (packId) {
    if (titleHeader) titleHeader.textContent = "Edit Pack";
    setText("tone-sharing-pack-status", "Loading...");
    try {
      const details = await apiFetch<ToneSharingPackDetails>(`/packs/${packId}`);
      if (titleEl) titleEl.value = details.pack.title;
      if (descEl) descEl.value = details.pack.description ?? "";
      checkedItemIds = new Set(details.items.map((item) => item.itemId));
      if (preCheckItemId) {
        checkedItemIds.add(preCheckItemId);
      }
    } catch (error) {
      setText("tone-sharing-pack-status", `Load failed: ${(error as Error).message}`);
    }
  } else {
    if (titleHeader) titleHeader.textContent = "Create Pack";
  }

  renderPackItemSelection(toneSharingState.myItems, checkedItemIds);
  setText("tone-sharing-pack-status", "");
  modal.style.display = "flex";
}

export function closePackModal(force = false): void {
  if (publishPackInFlight && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-pack-modal");
  if (modal) {
    modal.style.display = "none";
  }
  editingPackId = null;
}

export async function loadMyDraftPacksForSelect(): Promise<void> {
  const select = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (!select || !toneSharingState.user) {
    return;
  }
  try {
    const data = await apiFetch<{ packs: ToneSharingPack[] }>("/packs/me/list");
    const drafts = data.packs.filter((p) => p.moderationStatus === "draft");
    select.innerHTML =
      `<option value="">Or add to existing draft pack…</option>` +
      drafts.map((p) => `<option value="${p.id}">${p.title}</option>`).join("");
  } catch {
  }
}

export async function addItemToExistingPack(packId: string, itemId: string): Promise<void> {
  const details = await apiFetch<ToneSharingPackDetails>(`/packs/${packId}`);
  const existingIds = details.items.map((item) => item.itemId);
  if (!existingIds.includes(itemId)) {
    existingIds.push(itemId);
  }
  await apiFetch(`/packs/${packId}/items`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ itemIds: existingIds })
  });
}

export async function savePack(publish: boolean): Promise<void> {
  if (publishPackInFlight) {
    return;
  }
  if (!toneSharingState.user) {
    setText("tone-sharing-pack-status", "Sign in first.");
    return;
  }

  const title = element<HTMLInputElement>("tone-sharing-pack-title")?.value.trim() ?? "";
  const description = element<HTMLTextAreaElement>("tone-sharing-pack-description")?.value.trim() ?? "";
  const imageFile = element<HTMLInputElement>("tone-sharing-pack-image")?.files?.[0] ?? null;

  if (!title) {
    setText("tone-sharing-pack-status", "Pack title is required.");
    return;
  }

  if (publish) {
    setPublishPackBusy(true, editingPackId ? "Submitting pack for approval..." : "Publishing pack...");
  }

  const itemIds = Array.from(document.querySelectorAll<HTMLInputElement>("#tone-sharing-pack-items input[data-pack-item-id]:checked"))
    .map((input) => input.dataset.packItemId ?? "")
    .filter(Boolean);

  if (itemIds.length === 0) {
    setText("tone-sharing-pack-status", "Select at least one preset.");
    if (publish) {
      setPublishPackBusy(false, "Publishing pack...");
    }
    return;
  }

  setText("tone-sharing-pack-status", editingPackId ? "Saving..." : "Creating pack...");

  try {
    let thumbnailAssetId: string | undefined;
    if (imageFile) {
      const variants = await buildPackImageVariants(imageFile);
      const thumbnailBlob = variants.small.blob;
      const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          kind: "thumbnail",
          mimeType: thumbnailBlob.type || "application/octet-stream",
          byteSize: thumbnailBlob.size
        })
      });

      const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
        method: "PUT",
        headers: {
          "content-type": thumbnailBlob.type || "application/octet-stream",
          ...(toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {})
        },
        body: thumbnailBlob,
        credentials: "include"
      });
      const uploadPayload = await uploadResponse.json().catch(() => null);
      if (!uploadResponse.ok || uploadPayload?.ok === false) {
        throw new Error(uploadPayload?.error?.message || `Pack image upload failed (${uploadResponse.status})`);
      }

      const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ uploadId: init.uploadId })
      });
      thumbnailAssetId = complete.assetId;
    }

    let packId: string;
    if (editingPackId) {
      await apiFetch(`/packs/${editingPackId}`, {
        method: "PATCH",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, ...(thumbnailAssetId ? { thumbnailAssetId } : {}) })
      });
      packId = editingPackId;
    } else {
      const pack = await apiFetch<{ pack: ToneSharingPack }>("/packs", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ title, description, thumbnailAssetId })
      });
      packId = pack.pack.id;
      editingPackId = packId;
    }

    await apiFetch(`/packs/${packId}/items`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ itemIds })
    });

    if (publish) {
      await apiFetch(`/packs/${packId}/publish`, { method: "POST" });
      setText("tone-sharing-pack-status", "Pack submitted for moderator approval.");
      setPublishPackBusy(false, "Publishing pack...");
      closePackModal(true);
    } else {
      setText("tone-sharing-pack-status", "Draft saved.");
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setText("tone-sharing-pack-status", `${publish ? "Publish" : "Save"} failed: ${(error as Error).message}`);
  } finally {
    if (publish) {
      setPublishPackBusy(false, "Publishing pack...");
    }
  }
}

export function renderPackItemSelection(items: ToneSharingItem[], checked = new Set<string>()): void {
  const host = element<HTMLElement>("tone-sharing-pack-items");
  if (!host) {
    return;
  }

  if (!items.length) {
    host.innerHTML = `<div class="tone-sharing-select-item">No published presets yet. Publish a preset first.</div>`;
    return;
  }

  host.innerHTML = items
    .map(
      (item) => `
        <label class="tone-sharing-select-item">
          <input type="checkbox" data-pack-item-id="${item.id}" ${checked.has(item.id) ? "checked" : ""} />
          <span>${item.title}</span>
        </label>
      `
    )
    .join("");
}
