/**
 * The expanded view of a single pack: its artwork, its presets, and which of
 * them are already installed.
 */

import { escapeHtml } from "../utils.js";
import { renderToneIconButton } from "./actionButtons.js";
import { apiFetch } from "./api.js";
import { element, setText } from "./dom.js";
import { getToneSharingDisplayTags } from "./format.js";
import { resolvePackThumbnailUrl } from "./images.js";
import { buildInstalledToneSharingLookup } from "./presetLinks.js";
import type { ToneSharingPackDetails } from "./types.js";

export function clearPackDetail(): void {
  const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
  if (modal) {
    modal.style.display = "none";
  }
}

export async function renderPackDetail(details: ToneSharingPackDetails): Promise<void> {
  const modal = element<HTMLElement>("tone-sharing-pack-view-modal");
  if (!modal) {
    return;
  }
  modal.dataset.packId = details.pack.id;

  const pack = details.pack;
  const installedLookup = buildInstalledToneSharingLookup();
  const isPackInstalled = installedLookup.packIds.has(pack.id);
  const heroEl = element<HTMLElement>("tone-sharing-pack-view-hero");
  if (heroEl) {
    let imageUrl = "";
    if (pack.thumbnailUrl) {
      try {
        imageUrl = await resolvePackThumbnailUrl(pack);
      } catch { }
    }
    heroEl.style.backgroundImage = imageUrl
      ? `linear-gradient(180deg, rgba(5,6,12,0.08) 0%, rgba(5,6,12,0.65) 55%, rgba(5,6,12,0.97) 100%), url('${imageUrl}')`
      : "";
  }

  setText("tone-sharing-pack-view-title", pack.title);
  const descEl = element<HTMLElement>("tone-sharing-pack-view-description");
  if (descEl) {
    descEl.textContent = pack.description ?? "";
    descEl.style.display = pack.description ? "" : "none";
  }

  setText("tone-sharing-pack-view-count", `${details.items.length} preset${details.items.length !== 1 ? "s" : ""}`);
  const actionsEl = element<HTMLElement>("tone-sharing-pack-view-actions");
  if (actionsEl) {
    actionsEl.innerHTML = `
      ${renderToneIconButton({ kind: "pack-action", value: "share-pack", icon: "share", label: "Share pack" })}
      ${renderToneIconButton({ kind: "pack-action", value: "download-pack", icon: "download", label: isPackInstalled ? "Pack already installed" : "Download pack", primary: true, disabled: isPackInstalled })}
    `;
  }

  const presetsEl = element<HTMLElement>("tone-sharing-pack-view-presets");
  if (presetsEl) {
    if (!details.items.length) {
      presetsEl.innerHTML = `<div class="tone-sharing-status">No presets in this pack.</div>`;
    } else {
      presetsEl.innerHTML = [...details.items]
        .sort((a, b) => a.sortOrder - b.sortOrder)
        .map(
          (item) => {
            const isItemInstalled = installedLookup.itemIds.has(item.itemId);
            const displayTags = getToneSharingDisplayTags(item.tags);
            return `
          <div class="tone-sharing-pack-preset-row" data-item-id="${escapeHtml(item.itemId)}">
            <div class="tone-sharing-pack-preset-info">
              <div class="tone-sharing-pack-preset-title">${escapeHtml(item.title)}</div>
              ${item.type ? `<div class="tone-sharing-pack-preset-type">${escapeHtml(item.type)}</div>` : ""}
              ${item.description ? `<div class="tone-sharing-pack-preset-desc">${escapeHtml(item.description)}</div>` : ""}
              ${displayTags.length > 0 ? `<div class="tone-sharing-pack-preset-tags">${displayTags.map((tag) => `<span class="tone-sharing-tag-badge">${escapeHtml(tag)}</span>`).join("")}</div>` : ""}
            </div>
            <div class="tone-sharing-pack-preset-actions">
              ${renderToneIconButton({
                kind: "pack-action",
                value: "preview",
                icon: "preview",
                label: "Preview preset",
                attrs: {
                  "data-item-id": item.itemId,
                  "data-item-title": item.title,
                },
              })}
              ${renderToneIconButton({
                kind: "pack-action",
                value: "download",
                icon: "download",
                label: isItemInstalled ? "Preset already installed" : "Download preset",
                primary: true,
                disabled: isItemInstalled,
                attrs: {
                  "data-item-id": item.itemId,
                  "data-item-title": item.title,
                },
              })}
            </div>
          </div>`
          }
        )
        .join("");
    }
  }

  modal.style.display = "flex";
}

export async function viewPack(packId: string): Promise<void> {
  const details = await apiFetch<ToneSharingPackDetails>(`/packs/${packId}`);
  await renderPackDetail(details);
}
