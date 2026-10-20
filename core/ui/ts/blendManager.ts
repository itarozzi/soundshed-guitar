import { uiState } from "./state.js";
import { openBlendEditorWithDefinition } from "./signalPathBlend.js";
import { postMessage } from "./bridge.js";
import { showNotification } from "./notifications.js";
import { showConfirm } from "./dialogs.js";
import type { BlendDefinition, Preset } from "./types.js";
import { EffectGuids } from "./effectGuids.js";
import { escapeHtml } from "./utils.js";
import { collectBlendModelIds } from "./blendUtils.js";
import { exportBlendArchive, importBlendArchive } from "./blendArchive.js";

const blendList = document.getElementById("blend-list");
const blendSearchInput = document.getElementById("blend-search-input") as HTMLInputElement | null;
const blendImportInput = document.getElementById("blend-import-input") as HTMLInputElement | null;

function newBlendId(): string {
  return typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

let initialized = false;
let searchFilter = "";

export function initBlendManager(): void {
  if (initialized) return;
  initialized = true;

  blendSearchInput?.addEventListener("input", () => {
    searchFilter = blendSearchInput.value.toLowerCase();
    renderBlendList();
  });

  // A new blend is saved only by the editor's Save.
  document.getElementById("blend-new-btn")?.addEventListener("click", () => {
    openBlendEditorWithDefinition({ id: newBlendId(), name: "New Blend", category: "amp", models: [], modelMappings: [], blendMode: "interpolate" });
  });
  document.getElementById("blend-import-btn")?.addEventListener("click", () => blendImportInput?.click());
  blendImportInput?.addEventListener("change", () => {
    const file = blendImportInput.files?.[0];
    blendImportInput.value = "";
    if (file) {
      void importBlendArchive(file, uiState.resourceLibrary).then((name) => {
        showNotification(name ? "Blend imported" : "Import failed", name ?? "The file is not a blend archive.");
      });
    }
  });

  renderBlendList();
}

export function renderBlendList(): void {
  if (!blendList) return;

  const blends = uiState.blendLibrary ?? [];
  const filtered = searchFilter
    ? blends.filter((blend) => {
        const haystack = [
          blend.id,
          blend.name,
          blend.category,
          blend.toneGroupId ?? "",
          blend.toneGroupTitle ?? "",
          ...(blend.parameters ?? []),
        ]
          .join(" ")
          .toLowerCase();
        return haystack.includes(searchFilter);
      })
    : blends;

  if (!filtered.length) {
    blendList.innerHTML = `
      <div class="composite-empty">
        ${searchFilter ? "No blends match your search." : "No blends defined yet."}
      </div>`;
    return;
  }

  blendList.innerHTML = filtered
    .map((blend) => {
      const modelCount = collectBlendModelIds(blend).length;
      const paramCount = blend.parameters?.length ?? 0;
      const mode = blend.blendMode ?? "interpolate";
      // A factory blend comes back with its archive, so it offers no Delete. Saving an edit
      // makes a copy of the user's own, which can be deleted to go back to the factory one.
      const deleteButton = blend.factory
        ? ""
        : `<button class="blend-delete-btn advanced-action-btn danger" data-blend-id="${escapeHtml(blend.id)}" title="Delete">Delete</button>`;
      return `
        <div class="composite-list-item" data-blend-id="${escapeHtml(blend.id)}">
          <div class="composite-list-info">
            <span class="composite-list-name">${escapeHtml(blend.name || blend.id)}</span>
            <span class="composite-list-meta">${blend.factory ? "Factory · " : ""}${escapeHtml(blend.category || "amp")} · ${modelCount} models · ${paramCount} params · ${escapeHtml(mode)}</span>
            ${blend.toneGroupTitle ? `<span class="composite-list-desc">${escapeHtml(blend.toneGroupTitle)}</span>` : ""}
          </div>
          <div class="composite-list-actions">
            <button class="blend-edit-btn advanced-action-btn" data-blend-id="${escapeHtml(blend.id)}" title="Edit">Edit</button>
            <button class="blend-export-btn advanced-action-btn" data-blend-id="${escapeHtml(blend.id)}" title="Export as .namz with its models">Export</button>
            ${deleteButton}
          </div>
        </div>
      `;
    })
    .join("");

  blendList.querySelectorAll(".blend-edit-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const blendId = (btn as HTMLElement).dataset.blendId ?? "";
      if (!blendId) return;
      const blend = (uiState.blendLibrary ?? []).find((entry) => entry.id === blendId);
      if (!blend) {
        showNotification("Blend not found");
        return;
      }
      const clone = JSON.parse(JSON.stringify(blend)) as BlendDefinition;
      openBlendEditorWithDefinition(clone);
    });
  });

  blendList.querySelectorAll(".blend-export-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const blend = (uiState.blendLibrary ?? []).find((entry) => entry.id === (btn as HTMLElement).dataset.blendId);
      if (blend) {
        void exportBlendArchive(blend, uiState.resourceLibrary);
      }
    });
  });

  blendList.querySelectorAll(".blend-delete-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const blendId = (btn as HTMLElement).dataset.blendId ?? "";
      if (!blendId) return;
      void confirmDeleteBlend(blendId);
    });
  });
}

async function confirmDeleteBlend(blendId: string): Promise<void> {
  const blend = (uiState.blendLibrary ?? []).find((entry) => entry.id === blendId);
  if (!blend) {
    showNotification("Blend not found");
    return;
  }

  const usageCount = countBlendUsage(blendId);
  if (usageCount > 0) {
    showNotification("Delete blocked", `Blend is used by ${usageCount} preset(s).`);
    return;
  }

  const confirmed = await showConfirm(
    `Delete blend "${blend.name || blend.id}"? This cannot be undone.`,
    "Delete blend",
  );
  if (!confirmed) {
    return;
  }

  postMessage({
    type: "deleteBlendDefinition",
    blendId,
  });
}

function countBlendUsage(blendId: string): number {
  let count = 0;
  const seen = new Set<string>();

  const scanPreset = (preset?: Preset | null): void => {
    if (!preset || seen.has(preset.id)) {
      return;
    }
    seen.add(preset.id);
    const nodes = preset.graph?.nodes ?? [];
    const used = nodes.some((node) => {
      if (node.type !== EffectGuids.kAmpNamBlend) {
        return false;
      }
      const nodeBlendId = (node.config?.blendId ?? "").trim();
      return nodeBlendId === blendId;
    });
    if (used) {
      count += 1;
    }
  };

  uiState.presets.forEach((preset) => scanPreset(preset));
  uiState.presetCache.forEach((preset) => scanPreset(preset));

  return count;
}


