/**
 * Acting on someone else's published tone: installing it, deleting your own, and
 * the moderation verdicts an admin can issue.
 */

import { showPrompt } from "../dialogs.js";
import { uiState } from "../state.js";
import { apiFetch, parseApiErrorMessage, toneSharingFetch } from "./api.js";
import { installDownloadedArchives } from "./install.js";
import type { DownloadedArchive } from "./install.js";
import { syncInstalledPacksFromAppSettings } from "./installedPacks.js";
import { requestBrowseRefreshAfterInstall } from "./refresh.js";
import { toneSharingState } from "./state.js";
import type { ToneSharingItem, ToneSharingPackDetails } from "./types.js";

export async function moderateTarget(kind: "item" | "pack", id: string, action: "approve" | "reject"): Promise<void> {
  // Cancel still rejects, without a reason, as it did when this was window.prompt().
  const notes = action === "reject"
    ? ((await showPrompt("Optional rejection reason", "", { title: "Reject" })) ?? "").trim()
    : "";
  const endpoint = kind === "item" ? `/items/${id}/moderate` : `/packs/${id}/moderate`;
  await apiFetch(endpoint, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ action, notes: notes || undefined }),
  });
}

function hasInstallStateChanged(beforeInstalledSnapshot: string, beforePresetCount: number): boolean {
  return beforePresetCount !== uiState.presets.length || beforeInstalledSnapshot !== JSON.stringify(toneSharingState.installedPacks);
}

/** A pack with no archive of its own (409): each of its presets, downloaded one by one. */
async function downloadPackItems(details: ToneSharingPackDetails): Promise<DownloadedArchive[]> {
  const downloads: DownloadedArchive[] = [];
  for (const item of [...details.items].sort((a, b) => a.sortOrder - b.sortOrder)) {
    const response = await toneSharingFetch(`/items/${item.itemId}/download`);
    if (!response.ok) {
      throw new Error(`Failed to download pack item ${item.title}: ${await parseApiErrorMessage(response)}`);
    }
    downloads.push({ title: item.title || "Imported Preset", data: await response.arrayBuffer(), itemId: item.itemId });
  }
  if (downloads.length === 0) {
    throw new Error("Pack has no importable presets");
  }
  return downloads;
}

/** Downloads a shared preset or pack and has the engine install it (install.ts). */
export async function downloadAsset(kind: "item" | "pack", id: string): Promise<void> {
  const installedSnapshotBefore = JSON.stringify(toneSharingState.installedPacks);
  const presetCountBefore = uiState.presets.length;
  const path = kind === "item" ? `/items/${id}/download` : `/packs/${id}/download`;
  const response = await toneSharingFetch(path);

  if (!response.ok && !(kind === "pack" && response.status === 409)) {
    const message = await parseApiErrorMessage(response);
    throw new Error(message);
  }

  const disposition = response.headers.get("content-disposition") || "";
  const fileMatch = disposition.match(/filename="?([^"]+)"?/i);
  const fileName = fileMatch?.[1] || `${kind}-${id}${kind === "pack" ? ".zip" : ""}`;

  if (kind === "pack") {
    let details: ToneSharingPackDetails | null = null;
    try {
      details = await apiFetch<ToneSharingPackDetails>(`/packs/${id}`);
    } catch (error) {
      // Only the title is wanted from it when the pack has an archive of its own.
      if (!response.ok) {
        throw error;
      }
    }

    const title = details?.pack.title || fileName.replace(/\.zip$/i, "");
    const downloads = response.ok || !details
      ? [{ title, data: await response.arrayBuffer() }]
      : await downloadPackItems(details);
    await installDownloadedArchives({
      entry: {
        id: `tone-sharing-api:${id}`,
        title,
        source: "toneSharingApi",
        packId: id,
        ...(response.ok ? { archiveFileName: fileName } : {}),
      },
      folder: title,
      downloads,
      confirm: true,
    });
  } else {
    let itemMeta: ToneSharingItem | null = null;
    try {
      const itemResponse = await apiFetch<{ item: ToneSharingItem }>(`/items/${id}`);
      itemMeta = itemResponse.item;
    } catch {
      itemMeta = null;
    }

    const title = itemMeta?.title ?? fileName.replace(/\.(soundshed\.preset|soundshed\.presets|preset|zip)$/i, "");
    const presetIds = await installDownloadedArchives({
      entry: { id: `tone-sharing-api:item:${id}`, title, source: "toneSharingApi", archiveFileName: fileName },
      downloads: [{ title, data: await response.arrayBuffer(), itemId: id }],
    });
    if (presetIds?.length === 0) {
      throw new Error("Downloaded preset archive contained no importable presets");
    }
  }

  // The engine recorded the install in its app setting, which reached uiState before its answer.
  syncInstalledPacksFromAppSettings();
  if (hasInstallStateChanged(installedSnapshotBefore, presetCountBefore)) {
    await requestBrowseRefreshAfterInstall();
  }
}

export async function deleteAsset(kind: "item" | "pack", id: string): Promise<void> {
  const path = kind === "item" ? `/items/${id}` : `/packs/${id}`;
  await apiFetch(path, { method: "DELETE" });
}
