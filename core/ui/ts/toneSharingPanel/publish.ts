/**
 * Publishing the active preset as a community item: the modal, the tag picker,
 * the consent gate, and the upload itself.
 */

import { showNotification } from "../notifications.js";
import { clonePreset, uiState } from "../state.js";
import { apiFetch, buildApiUrl } from "./api.js";
import { loadBrowse, loadMine } from "./browse.js";
import { ensurePublishConsent } from "./consent.js";
import { element, setUploadStatus } from "./dom.js";
import { getToneSharingHostActions } from "./hostActions.js";
import { addItemToExistingPack, loadMyDraftPacksForSelect, openPackModal } from "./packEditor.js";
import { getPresetToneSharingOrigin } from "./presetLinks.js";
import { copyToneSharingShareLink } from "./shareLinks.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem } from "./types.js";

let publishItemInFlight = false;

export function setPublishItemBusy(busy: boolean, message?: string): void {
  publishItemInFlight = busy;
  const closeButton = element<HTMLButtonElement>("tone-sharing-publish-modal-close");
  const cancelButton = element<HTMLButtonElement>("tone-sharing-publish-modal-cancel");
  const submitButton = element<HTMLButtonElement>("tone-sharing-upload-item");
  const progress = element<HTMLElement>("tone-sharing-publish-progress");
  const titleInput = element<HTMLInputElement>("tone-sharing-item-title");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-item-description");
  const packSelect = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  const newPackButton = element<HTMLButtonElement>("tone-sharing-add-to-new-pack");
  document.querySelectorAll<HTMLButtonElement>("#tone-sharing-tags-picker .tone-sharing-tag-chip").forEach((button) => {
    button.disabled = busy;
  });
  [closeButton, cancelButton, submitButton, titleInput, descriptionInput, packSelect, newPackButton].forEach((control) => {
    if (control) {
      control.disabled = busy;
    }
  });
  if (progress) {
    progress.style.display = busy ? "flex" : "none";
    const label = progress.querySelector<HTMLElement>(".tone-sharing-progress-label");
    if (label) {
      label.textContent = message ?? "Working...";
    }
  }
}

export function openToneSharingPublishPresetModal(defaultTitle?: string, defaultDescription?: string): void {
  const modal = element<HTMLElement>("tone-sharing-publish-modal");
  if (!modal) {
    return;
  }

  const titleInput = element<HTMLInputElement>("tone-sharing-item-title");
  const descriptionInput = element<HTMLTextAreaElement>("tone-sharing-item-description");

  // Pre-fill from active preset if not overridden
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  const resolvedTitle = defaultTitle ?? activePreset?.name ?? "";
  const resolvedDescription = defaultDescription ?? activePreset?.description ?? "";

  if (titleInput) {
    titleInput.value = resolvedTitle;
  }
  if (descriptionInput) {
    descriptionInput.value = resolvedDescription;
  }
  setToneSharingTagsPickerValue(activePreset?.tags ?? []);
  setPublishItemBusy(false, "Publishing preset...");

  setUploadStatus(activePreset ? `Publishing: ${activePreset.name ?? activePreset.id}` : "");

  const publishButton = element<HTMLButtonElement>("tone-sharing-upload-item");
  const origin = getPresetToneSharingOrigin(activePreset);
  if (publishButton) {
    publishButton.disabled = false;
    publishButton.title = "";
  }
  if (origin?.republishBlocked) {
    setUploadStatus("This preset came from Tone Sharing. Use Save As to create a local copy before publishing.");
    if (publishButton) {
      publishButton.disabled = true;
      publishButton.title = "Use Save As to create a new local preset before publishing";
    }
  }

  // Reset pack assign UI
  element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.remove("active");
  const packSelect = element<HTMLSelectElement>("tone-sharing-pack-assign-select");
  if (packSelect) packSelect.value = "";
  void loadMyDraftPacksForSelect();

  modal.style.display = "flex";
}

export function closeToneSharingPublishPresetModal(force = false): void {
  if (publishItemInFlight && !force) {
    return;
  }
  const modal = element<HTMLElement>("tone-sharing-publish-modal");
  if (!modal) {
    return;
  }
  modal.style.display = "none";
}

export function getToneSharingTagsPickerValue(): string[] {
  const picker = element<HTMLElement>("tone-sharing-tags-picker");
  if (!picker) return [];
  return Array.from(picker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip.active"))
    .map((btn) => btn.dataset.tag ?? "")
    .filter(Boolean);
}

export function setToneSharingTagsPickerValue(tags: string[]): void {
  const picker = element<HTMLElement>("tone-sharing-tags-picker");
  if (!picker) return;
  const tagSet = new Set(tags);
  picker.querySelectorAll<HTMLButtonElement>(".tone-sharing-tag-chip").forEach((btn) => {
    btn.classList.toggle("active", tagSet.has(btn.dataset.tag ?? ""));
  });
}

export async function uploadAndPublishItem(): Promise<void> {
  if (publishItemInFlight) {
    return;
  }
  let title = element<HTMLInputElement>("tone-sharing-item-title")?.value.trim() ?? "";
  let description = element<HTMLTextAreaElement>("tone-sharing-item-description")?.value.trim() ?? "";
  const selectedTags = getToneSharingTagsPickerValue();

  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  if (!activePreset) {
    setUploadStatus("No preset selected.");
    return;
  }

  if (!title) {
    title = activePreset.name ?? activePreset.id;
  }
  if (!description) {
    description = activePreset.description ?? "";
  }

  if (!toneSharingState.user) {
    setUploadStatus("Sign in first.");
    return;
  }
  if (!title) {
    setUploadStatus("Title is required.");
    return;
  }

  const origin = getPresetToneSharingOrigin(activePreset);
  if (origin?.republishBlocked) {
    setUploadStatus("This preset came from Tone Sharing. Use Save As before publishing it again.");
    return;
  }

  try {
    await ensurePublishConsent();
  } catch (error) {
    setUploadStatus(`Publish cancelled: ${(error as Error).message}`);
    return;
  }

  // Validate signal chain: requires input + output + at least one effect node
  const graphNodes = activePreset.graph?.nodes ?? [];
  const hasInput = graphNodes.some((n) => n.type === "input");
  const hasOutput = graphNodes.some((n) => n.type === "output");
  const effectNodes = graphNodes.filter((n) => n.type !== "input" && n.type !== "output");
  if (!hasInput || !hasOutput || effectNodes.length === 0) {
    setUploadStatus("Preset must have a signal chain with at least one effect node to publish.");
    return;
  }

  let publicPayload: Blob;
  let privatePayload: Blob;
  try {
    const archiveBlobs = await getToneSharingHostActions().buildToneSharingPresetArchiveBlobs(clonePreset(activePreset));
    publicPayload = archiveBlobs.publicBlob;
    privatePayload = archiveBlobs.privateBlob;
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
    return;
  }

  setUploadStatus("Building & uploading archive...");
  setPublishItemBusy(true, "Uploading preset for approval...");

  try {
    const init = await apiFetch<{ uploadId: string }>("/uploads/init", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind: "item_payload",
        mimeType: publicPayload.type || "application/octet-stream",
        byteSize: publicPayload.size
      })
    });

    const uploadResponse = await fetch(buildApiUrl(`/uploads/${init.uploadId}`), {
      method: "PUT",
      headers: {
        "content-type": publicPayload.type || "application/octet-stream",
        ...(toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {})
      },
      body: publicPayload,
      credentials: "include"
    });

    const uploadResult = await uploadResponse.json();
    if (!uploadResponse.ok || uploadResult?.ok === false) {
      throw new Error(uploadResult?.error?.message || `Upload failed (${uploadResponse.status})`);
    }

    const complete = await apiFetch<{ assetId: string }>("/uploads/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ uploadId: init.uploadId })
    });

    const backupInit = await apiFetch<{ uploadId: string }>("/uploads/init", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        kind: "item_payload",
        mimeType: privatePayload.type || "application/octet-stream",
        byteSize: privatePayload.size
      })
    });

    const backupUploadResponse = await fetch(buildApiUrl(`/uploads/${backupInit.uploadId}`), {
      method: "PUT",
      headers: {
        "content-type": privatePayload.type || "application/octet-stream",
        ...(toneSharingState.sessionId ? { "x-session-id": toneSharingState.sessionId } : {})
      },
      body: privatePayload,
      credentials: "include"
    });

    const backupUploadResult = await backupUploadResponse.json();
    if (!backupUploadResponse.ok || backupUploadResult?.ok === false) {
      throw new Error(backupUploadResult?.error?.message || `Backup upload failed (${backupUploadResponse.status})`);
    }

    const backupComplete = await apiFetch<{ assetId: string }>("/uploads/complete", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ uploadId: backupInit.uploadId })
    });

    const item = await apiFetch<{ item: ToneSharingItem }>("/items", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        type: "preset",
        title,
        description,
        tags: selectedTags.length > 0 ? selectedTags : undefined,
        payloadAssetId: complete.assetId,
        privatePayloadAssetId: backupComplete.assetId
      })
    });

    await apiFetch(`/items/${item.item.id}/publish`, { method: "POST" });

    let shareCopied = false;
    try {
      const link = await copyToneSharingShareLink({ kind: "item", id: item.item.id });
      showNotification("Share link copied", link);
      shareCopied = true;
    } catch {
      shareCopied = false;
    }

    const addToNewPack = element<HTMLButtonElement>("tone-sharing-add-to-new-pack")?.classList.contains("active") ?? false;
    const assignPackId = element<HTMLSelectElement>("tone-sharing-pack-assign-select")?.value ?? "";
    setPublishItemBusy(false, "Uploading preset for approval...");
    closeToneSharingPublishPresetModal(true);

    const shareNote = shareCopied
      ? " Share link copied (it will open once approved)."
      : " Open the item card and press Share to copy the public link once approved.";

    if (addToNewPack) {
      await openPackModal(undefined, item.item.id);
      setUploadStatus(`Preset submitted for moderator approval.${shareNote}`);
    } else if (assignPackId) {
      try {
        await addItemToExistingPack(assignPackId, item.item.id);
        setUploadStatus(`Submitted for approval and added to pack.${shareNote}`);
      } catch (error) {
        setUploadStatus(`Submitted for approval but pack update failed: ${(error as Error).message}.${shareNote}`);
      }
    } else {
      setUploadStatus(`Preset submitted for moderator approval.${shareNote}`);
    }

    await Promise.all([loadBrowse(), loadMine()]);
  } catch (error) {
    setUploadStatus(`Publish failed: ${(error as Error).message}`);
  } finally {
    setPublishItemBusy(false, "Uploading preset for approval...");
  }
}
