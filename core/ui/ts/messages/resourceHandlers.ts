/**
 * Resource library traffic: imports, removals, folder browsing, usage counts,
 * and the exports that write a resource back out.
 */

import { handleResourceDataMessage } from "../archiveUtils.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { renderActivePreset } from "../presets.js";
import { refreshSettingsView } from "../settings.js";
import { handleHostedPluginResourceLoadCompleted, handleHostedPluginResourceLoadFailed, handleNodeResourceBrowseCancelled } from "../signalPath.js";
import { removeResourceFromUiState, upsertImportedResourceInUiState } from "./normalize.js";
import type { IncomingPayload } from "./types.js";

export function onResourceCleanupResult(payload: IncomingPayload): void {
  const removed = typeof (payload as { removed?: number }).removed === "number"
    ? (payload as { removed?: number }).removed as number
    : 0;
  const skipped = typeof (payload as { skipped?: number }).skipped === "number"
    ? (payload as { skipped?: number }).skipped as number
    : 0;
  const skippedUsed = typeof (payload as { skippedUsed?: number }).skippedUsed === "number"
    ? (payload as { skippedUsed?: number }).skippedUsed as number
    : 0;
  const message = removed > 0 ? `Removed ${removed} resources.` : "No resources removed.";
  const parts: string[] = [];
  if (skipped > 0) {
    parts.push(`${skipped} skipped`);
  }
  if (skippedUsed > 0) {
    parts.push(`${skippedUsed} in use`);
  }
  const detail = parts.length ? `${parts.join("; ")}.` : "";
  showNotification(message, detail);
  refreshSettingsView();
}

export function onModelLoaded(payload: IncomingPayload): void {
  appendLog(`model loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
  renderActivePreset();
  showNotification("Model loaded", (payload as { path?: string }).path ?? "");
}

export function onIrLoaded(payload: IncomingPayload): void {
  console.log("[JS] IR loaded event received, path:", (payload as { path?: string }).path);
  appendLog(`IR loaded ← ${(payload as { path?: string }).path ?? "unknown"}`);
  renderActivePreset();
  showNotification("IR loaded", (payload as { path?: string }).path ?? "");
}

export function onResourceImported(payload: IncomingPayload): void {
  const info = payload as { id?: string; name?: string; resourceType?: string; filePath?: string; requestId?: string };
  upsertImportedResourceInUiState(info);
  appendLog(`resource imported ← ${info.name ?? "unknown"}`);
  if (!info.requestId) {
    showNotification("Resource imported", info.name ?? info.filePath ?? "");
  }
  document.dispatchEvent(new CustomEvent("resource-browser:resource-imported", {
    detail: {
      id: info.id ?? "",
      name: info.name ?? "",
      resourceType: info.resourceType ?? "",
      filePath: info.filePath ?? "",
      requestId: info.requestId ?? "",
    },
  }));
}

export function onResourceImportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string; detail?: string; requestId?: string };
  appendLog(`resource import failed ← ${info.message ?? "unknown"}`);
  if (!info.requestId) {
    showNotification(info.message ?? "Import failed", info.detail ?? "");
  }
  document.dispatchEvent(new CustomEvent("resource-browser:resource-import-failed", {
    detail: { requestId: info.requestId ?? "", message: info.detail ?? info.message ?? "Import failed" },
  }));
}

export function onResourceRemoved(payload: IncomingPayload): void {
  const info = payload as { id?: string; resourceType?: string };
  removeResourceFromUiState(info);
  appendLog(`resource removed ← ${info.id ?? "unknown"}`);
  document.dispatchEvent(new CustomEvent("resource-browser:resource-removed", {
    detail: {
      id: info.id ?? "",
      resourceType: info.resourceType ?? "",
    },
  }));
}

export function onResourceDeleteFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string; detail?: string; presetName?: string };
  appendLog(`resource delete failed ← ${info.message ?? "unknown"}`);
  showNotification(info.message ?? "Resource delete failed", info.detail ?? info.presetName ?? "");
}

export function onResourceUsageInfo(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:usage-info", { detail: payload }));
}

export function onResourceFolderPicked(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:folder-picked", { detail: payload }));
}

export function onResourceFolderListing(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:folder-listing", { detail: payload }));
}

export function onResourceFolderMetadata(payload: IncomingPayload): void {
  document.dispatchEvent(new CustomEvent("resource-browser:folder-metadata", { detail: payload }));
}

export function onResourceFolderListingFailed(payload: IncomingPayload): void {
  const info = payload as { path?: string; message?: string };
  appendLog(`folder listing failed ← ${info.message ?? "unknown"}`);
  document.dispatchEvent(new CustomEvent("resource-browser:folder-listing-failed", { detail: payload }));
}

export function onHostedPluginResourceLoadFailed(payload: IncomingPayload): void {
  handleHostedPluginResourceLoadFailed(payload as {
    nodeId?: string;
    resourceType?: string;
    resourceId?: string;
    filePath?: string;
    resourceIndex?: number;
    message?: string;
    errorCode?: string;
  });
}

export function onHostedPluginResourceLoadCompleted(payload: IncomingPayload): void {
  handleHostedPluginResourceLoadCompleted(payload as {
    nodeId?: string;
    resourceType?: string;
  });
}

export function onNodeResourceBrowseCancelled(payload: IncomingPayload): void {
  handleNodeResourceBrowseCancelled(payload as {
    nodeId?: string;
    resourceType?: string;
  });
}

/**
 * The engine could not delete an installed pack's archive. The pack is already gone from
 * the installed list, so this only says its files were left behind.
 */
export function onToneSharingPackDeleteFailed(payload: IncomingPayload): void {
  const message = (payload as { message?: string }).message ?? "";
  appendLog(`tone sharing pack delete failed ← ${message}`);
  showNotification("Pack files not deleted", message);
}

export function onResourceData(payload: IncomingPayload): void {
  handleResourceDataMessage(payload as { requestId: string; data?: string; fileName?: string; message?: string });
}

export function onResourceDataFailed(payload: IncomingPayload): void {
  handleResourceDataMessage(payload as { requestId: string; data?: string; fileName?: string; message?: string });
}

export function onBlendExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Blend exported", info.path ?? "");
}

export function onBlendExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Blend export failed", info.message ?? "");
}

export function onLibraryExportSaved(payload: IncomingPayload): void {
  const info = payload as { path?: string };
  showNotification("Library exported", info.path ?? "");
}

export function onLibraryExportFailed(payload: IncomingPayload): void {
  const info = payload as { message?: string };
  showNotification("Library export failed", info.message ?? "");
}
