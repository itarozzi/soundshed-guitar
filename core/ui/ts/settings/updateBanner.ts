/**
 * The version readout and the update banner at the top of the panel.
 */

import { uiState } from "../state.js";

/** Updates the current version display in the Software Updates section. */
export function updateCurrentVersionDisplay(): void {
  const versionEl = document.getElementById("current-version-display");
  if (!versionEl) return;

  const version = uiState.environment?.version ?? "Unknown";
  versionEl.textContent = version;
}

export function updateSettingsSessionStatus(): void {
  // Session lifecycle is managed silently in tone3000.ts.
}

export function refreshSettingsUpdateBanner(): void {
  const banner = document.getElementById("settings-update-banner");
  if (!banner) return;

  const update = uiState.availableUpdate;
  if (!update) {
    banner.style.display = "none";
    return;
  }

  const titleEl = document.getElementById("settings-update-banner-title");
  const notesEl = document.getElementById("settings-update-banner-notes");
  const linkEl = document.getElementById("settings-update-banner-link") as HTMLAnchorElement | null;

  if (titleEl) {
    titleEl.textContent = `Software Update Version ${update.version} is available`;
  }

  if (notesEl && update.releaseNotes) {
    const firstLine = update.releaseNotes
      .split("\n")
      .map((l) => l.trim())
      .find((l) => l.length > 0) ?? "";
    notesEl.textContent = firstLine.replace(/^#+\s*/, "").replace(/\*\*/g, "");
  }

  if (linkEl && update.downloadUrl) {
    linkEl.href = update.downloadUrl;
  }

  banner.style.display = "";
}
