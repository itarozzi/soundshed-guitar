/**
 * The AI tone search tab: asking the service for rig suggestions for a band or
 * song, and searching the local library for the gear it names.
 */

import { Features, isFeatureEnabled } from "../featureFlags.js";
import { switchMainPanel } from "../navigation.js";
import { activateEquipmentTab, activateLibraryTab } from "../settings.js";
import { setTone3000Search } from "../tone3000Browser.js";
import { escapeHtml } from "../utils.js";
import { renderToneIconButton, setActionButtonBusy } from "./actionButtons.js";
import { apiFetch } from "./api.js";
import { downloadAsset } from "./assets.js";
import { element, setUploadStatus } from "./dom.js";
import { buildInstalledToneSharingLookup } from "./presetLinks.js";
import { previewPreset } from "./preview.js";
import { aiSearchState } from "./state.js";
import type { AiToneSearchResult } from "./types.js";

// ===== AI Tone Search =====

export function gearLinkButton(label: string, category: string): string {
  return `<button type="button" class="ai-tone-gear-link" data-t3k-query="${escapeHtml(label)}" data-t3k-category="${escapeHtml(category)}" title="Search Tone3000 for \u201c${escapeHtml(label)}\u201d">${escapeHtml(label)}</button>`;
}

export function renderAiCombinations(): string {
  if (!aiSearchState.combinations.length) return "";

  const parts: string[] = [];
  if (aiSearchState.band) parts.push(aiSearchState.band);
  if (aiSearchState.song) parts.push(`"${aiSearchState.song}"`);
  const heading = parts.length ? `AI Tone Analysis: ${parts.join(" \u2014 ")}` : "AI Tone Analysis";

  const cards = aiSearchState.combinations.map((combo) => {
    const pedalsList = combo.pedals.length
      ? combo.pedals.map((p) => gearLinkButton(p, "pedal")).join("")
      : `<span class="ai-tone-tag ai-tone-tag--empty">None</span>`;
    const effectsList = combo.effects.length
      ? combo.effects.map((e) => gearLinkButton(e.name, "pedal")).join("")
      : `<span class="ai-tone-tag ai-tone-tag--empty">None</span>`;
    const libraryQuery = [combo.amp, ...(aiSearchState.band ? [aiSearchState.band] : [])].filter(Boolean).join(" ");
    return `
      <div class="ai-tone-combo-card">
        <div class="ai-tone-combo-header">
          <div class="ai-tone-combo-name">${escapeHtml(combo.name)}</div>
          <div class="ai-tone-combo-desc">${escapeHtml(combo.description)}</div>
        </div>
        <div class="ai-tone-combo-gear">
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Amp</span>
            <div class="ai-tone-tags">${gearLinkButton(combo.amp, "amp")}</div>
          </div>
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Cabinet</span>
            <div class="ai-tone-tags">${gearLinkButton(combo.cabinet, "cab")}</div>
          </div>
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Pedals</span>
            <div class="ai-tone-tags">${pedalsList}</div>
          </div>
          <div class="ai-tone-gear-row">
            <span class="ai-tone-gear-label">Effects</span>
            <div class="ai-tone-tags">${effectsList}</div>
          </div>
        </div>
        <div class="ai-tone-combo-actions">
          <button type="button" class="btn btn-secondary tone-sharing-card-btn" data-ai-library-query="${escapeHtml(libraryQuery)}">Find in Tone Sharing</button>
        </div>
        <div class="ai-tone-library-results"></div>
      </div>`;
  }).join("");

  return `
    <div class="ai-tone-results">
      <div class="ai-tone-results-heading">${escapeHtml(heading)}</div>
      ${cards}
    </div>`;
}

export function renderAiSearchView(): void {
  const feed = element<HTMLElement>("tone-sharing-feed");
  if (!feed) return;

  feed.innerHTML = `
    <div class="ai-tone-search-panel">
      <div class="ai-tone-search-form">
        <div class="ai-tone-search-inputs">
          <div class="ai-tone-input-group">
            <label for="ai-tone-band">Band / Artist</label>
            <input id="ai-tone-band" type="text" placeholder="e.g. Metallica, Jimi Hendrix" value="${escapeHtml(aiSearchState.band)}" />
          </div>
          <div class="ai-tone-input-group">
            <label for="ai-tone-song">Song (optional)</label>
            <input id="ai-tone-song" type="text" placeholder="e.g. Enter Sandman" value="${escapeHtml(aiSearchState.song)}" />
          </div>
          <button id="ai-tone-search-btn" type="button" class="btn btn-primary ai-tone-search-submit" ${aiSearchState.loading ? "disabled" : ""}>
            ${aiSearchState.loading ? "Searching\u2026" : "Find Tones"}
          </button>
        </div>
        <p class="ai-tone-search-hint">Discover the amps, cabinets and effects behind famous sounds, then find matching presets in the library.</p>
      </div>
      ${aiSearchState.error ? `<div class="tone-sharing-status ai-tone-error">${escapeHtml(aiSearchState.error)}</div>` : ""}
      ${renderAiCombinations()}
    </div>`;

  element<HTMLButtonElement>("ai-tone-search-btn")?.addEventListener("click", () => {
    aiSearchState.band = (element<HTMLInputElement>("ai-tone-band")?.value ?? "").trim();
    aiSearchState.song = (element<HTMLInputElement>("ai-tone-song")?.value ?? "").trim();
    void runAiToneSearch();
  });

  for (const id of ["ai-tone-band", "ai-tone-song"]) {
    element<HTMLInputElement>(id)?.addEventListener("keydown", (e) => {
      if (e.key === "Enter") element<HTMLButtonElement>("ai-tone-search-btn")?.click();
    });
  }

  feed.querySelectorAll<HTMLButtonElement>(".ai-tone-gear-link").forEach((btn) => {
    btn.addEventListener("click", () => {
      const query = btn.dataset.t3kQuery ?? "";
      const category = btn.dataset.t3kCategory ?? "";
      if (!query) return;
      switchMainPanel("settings");
      activateEquipmentTab("library");
      if (isFeatureEnabled(Features.Tone3000)) {
        activateLibraryTab("tone3000");
        setTone3000Search(query, category || undefined);
      } else {
        activateLibraryTab("resources");
      }
    });
  });

  feed.querySelectorAll<HTMLButtonElement>("[data-ai-library-query]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const query = btn.dataset.aiLibraryQuery ?? "";
      const resultsEl = btn.closest(".ai-tone-combo-card")?.querySelector<HTMLElement>(".ai-tone-library-results");
      if (!query || !resultsEl) return;
      await runAiLibrarySearch(query, resultsEl);
    });
  });
}

export async function runAiToneSearch(): Promise<void> {
  if (!aiSearchState.band && !aiSearchState.song) {
    aiSearchState.error = "Enter a band or song name to search.";
    renderAiSearchView();
    return;
  }
  aiSearchState.loading = true;
  aiSearchState.error = "";
  aiSearchState.combinations = [];
  renderAiSearchView();
  try {
    const params = new URLSearchParams();
    if (aiSearchState.band) params.set("band", aiSearchState.band);
    if (aiSearchState.song) params.set("song", aiSearchState.song);
    const result = await apiFetch<AiToneSearchResult>(`/tone-advisor?${params.toString()}`);
    aiSearchState.combinations = result.combinations;
    aiSearchState.error = "";
  } catch (err) {
    aiSearchState.error = `AI search failed: ${(err as Error).message}`;
    aiSearchState.combinations = [];
  } finally {
    aiSearchState.loading = false;
    renderAiSearchView();
  }
}

export async function runAiLibrarySearch(query: string, resultsEl: HTMLElement): Promise<void> {
  resultsEl.innerHTML = `<div class="ai-tone-library-loading">Searching library for \u201c${escapeHtml(query)}\u201d\u2026</div>`;
  try {
    const params = new URLSearchParams({ q: query });
    const data = await apiFetch<{ items: Array<{ id: string; title: string; type: string }> }>(`/search?${params.toString()}`);
    if (!data.items.length) {
      resultsEl.innerHTML = `<div class="ai-tone-library-empty">No matching presets found in library.</div>`;
      return;
    }
    const installedLookup = buildInstalledToneSharingLookup();
    const itemsHtml = data.items.slice(0, 8).map((item) => {
      const isInstalled = installedLookup.itemIds.has(item.id);
      return `
      <div class="ai-tone-library-item">
        <div class="ai-tone-library-item-info">
          <span class="ai-tone-library-item-title">${escapeHtml(item.title)}</span>
          <span class="ai-tone-library-item-type">${escapeHtml(item.type)}</span>
        </div>
        <div class="ai-tone-library-item-actions">
          ${renderToneIconButton({ kind: "action", value: "ai-preview", icon: "preview", label: "Preview preset", attrs: { "data-ai-preview": item.id, "data-ai-preview-title": item.title } })}
          ${renderToneIconButton({ kind: "action", value: "ai-download", icon: "download", label: isInstalled ? "Preset already installed" : "Download preset", primary: true, disabled: isInstalled, attrs: { "data-ai-download": item.id } })}
        </div>
      </div>`;
    }).join("");
    resultsEl.innerHTML = `
      <div class="ai-tone-library-list">
        <div class="ai-tone-library-heading">Library matches for \u201c${escapeHtml(query)}\u201d</div>
        ${itemsHtml}
      </div>`;
    resultsEl.querySelectorAll<HTMLButtonElement>("[data-ai-preview]").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const id = btn.dataset.aiPreview ?? "";
        const title = btn.dataset.aiPreviewTitle ?? "";
        const restoreBusy = setActionButtonBusy(btn, "Previewing...");
        try {
          await previewPreset(id, title);
          setUploadStatus("Preset preview applied (not installed).");
        }
        catch (err) {
          setUploadStatus(`Preview failed: ${(err as Error).message}`);
        } finally {
          restoreBusy();
        }
      });
    });
    resultsEl.querySelectorAll<HTMLButtonElement>("[data-ai-download]").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const id = btn.dataset.aiDownload ?? "";
        const restoreBusy = setActionButtonBusy(btn, "Downloading...");
        try {
          await downloadAsset("item", id);
        }
        catch (err) {
          setUploadStatus(`Download failed: ${(err as Error).message}`);
        } finally {
          restoreBusy();
        }
      });
    });
  } catch (err) {
    resultsEl.innerHTML = `<div class="ai-tone-library-empty">Search failed: ${escapeHtml((err as Error).message)}</div>`;
  }
}
