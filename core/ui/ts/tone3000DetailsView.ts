import { showNotification } from "./notifications.js";
import { escapeHtml } from "./utils.js";
import type { Tone3000Model, Tone3000Tone } from "./tone3000ApiTypes.js";
import { getTone3000ImageUrl } from "./tone3000Shared.js";

interface Tone3000DetailsElements {
  modal: HTMLElement;
  close: HTMLElement;
  title: HTMLElement;
  image: HTMLElement;
  meta: HTMLElement;
  description: HTMLElement;
  tags: HTMLElement;
  modelsStatus: HTMLElement;
  models: HTMLElement;
  selectAll: HTMLButtonElement;
  selectNone: HTMLButtonElement;
  importSelected: HTMLButtonElement;
  importBlend: HTMLButtonElement;
  progress: HTMLElement;
}

export interface OpenTone3000DetailsOptions {
  allowMultipleSelection: boolean;
  allowBlendCreation: boolean;
}

export class Tone3000DetailsView {
  private readonly els: Tone3000DetailsElements | null;
  private readonly fetchModels: (tone: Tone3000Tone) => Promise<Tone3000Model[]>;
  private readonly onImport: (tone: Tone3000Tone, models: Tone3000Model[], createBlend: boolean) => Promise<void>;

  private currentTone: Tone3000Tone | null = null;
  private currentModels: Tone3000Model[] = [];
  private detailsImporting = false;
  private allowMultipleSelection = true;
  private allowBlendCreation = true;

  constructor(deps: {
    fetchModels: (tone: Tone3000Tone) => Promise<Tone3000Model[]>;
    onImport: (tone: Tone3000Tone, models: Tone3000Model[], createBlend: boolean) => Promise<void>;
  }) {
    this.fetchModels = deps.fetchModels;
    this.onImport = deps.onImport;
    this.els = this.resolveElements();
    this.bindEvents();
  }

  open(tone: Tone3000Tone, options: OpenTone3000DetailsOptions): void {
    if (!this.els) {
      return;
    }

    this.allowMultipleSelection = options.allowMultipleSelection;
    this.allowBlendCreation = options.allowBlendCreation;
    this.currentTone = tone;
    this.currentModels = [];

    this.els.title.textContent = tone.title ?? tone.name ?? "Tone Details";
    const imageUrl = getTone3000ImageUrl(tone);
    this.els.image.innerHTML = imageUrl
      ? `<img src="${escapeHtml(imageUrl)}" alt="${escapeHtml(tone.gear ?? "Equipment")}" />`
      : `<div class="tone3000-details-image-placeholder">No image</div>`;

    const metaParts = [
      tone.gear ? `Gear: ${tone.gear}` : null,
      tone.platform ? `Platform: ${tone.platform}` : null,
      typeof tone.models_count === "number" ? `Models: ${tone.models_count}` : null,
      tone.user?.username ? `By: ${tone.user.username}` : null,
    ].filter(Boolean);

    this.els.meta.textContent = metaParts.length ? metaParts.join(" · ") : "No metadata available.";
    this.els.description.textContent = tone.description?.trim() || "No description provided.";

    const tags = Array.isArray(tone.tags)
      ? tone.tags.map((tag) => tag?.name?.trim()).filter((tagName): tagName is string => Boolean(tagName))
      : [];
    this.els.tags.innerHTML = tags.length
      ? tags.map((tag) => `<span class="tone3000-details-tag">${escapeHtml(tag)}</span>`).join("")
      : `<span class="tone3000-details-tag tone3000-details-tag-empty">No tags available.</span>`;

    this.els.models.innerHTML = "";
    this.els.modelsStatus.textContent = "Loading models...";
    this.setImportState(false);
    this.syncModeUI();
    this.els.modal.style.display = "flex";

    void this.loadModelsForTone(tone);
  }

  close(): void {
    if (!this.els) {
      return;
    }
    this.els.modal.style.display = "none";
  }

  private resolveElements(): Tone3000DetailsElements | null {
    const modal = document.getElementById("tone3000-details-modal");
    const close = document.getElementById("tone3000-details-close");
    const title = document.getElementById("tone3000-details-title");
    const image = document.getElementById("tone3000-details-image");
    const meta = document.getElementById("tone3000-details-meta");
    const description = document.getElementById("tone3000-details-description");
    const tags = document.getElementById("tone3000-details-tags");
    const modelsStatus = document.getElementById("tone3000-details-models-status");
    const models = document.getElementById("tone3000-details-models");
    const selectAll = document.getElementById("tone3000-details-select-all") as HTMLButtonElement | null;
    const selectNone = document.getElementById("tone3000-details-select-none") as HTMLButtonElement | null;
    const importSelected = document.getElementById("tone3000-details-import-selected") as HTMLButtonElement | null;
    const importBlend = document.getElementById("tone3000-details-import-blend") as HTMLButtonElement | null;
    const progress = document.getElementById("tone3000-details-progress");

    if (!modal || !close || !title || !image || !meta || !description || !tags || !modelsStatus || !models || !selectAll || !selectNone || !importSelected || !importBlend || !progress) {
      return null;
    }

    return {
      modal,
      close,
      title,
      image,
      meta,
      description,
      tags,
      modelsStatus,
      models,
      selectAll,
      selectNone,
      importSelected,
      importBlend,
      progress,
    };
  }

  private bindEvents(): void {
    if (!this.els || this.els.modal.dataset.detailsBound === "true") {
      return;
    }

    this.els.modal.dataset.detailsBound = "true";
    this.els.close.addEventListener("click", () => this.close());
    this.els.modal.addEventListener("mousedown", (event) => {
      if (event.target === this.els?.modal) {
        this.close();
      }
    });

    this.els.models.addEventListener("change", (event) => {
      const target = event.target as HTMLInputElement | null;
      if (!target || !target.classList.contains("tone3000-details-model-select")) {
        return;
      }
      if (!this.allowMultipleSelection && target.checked) {
        this.setExclusiveSelection(target.dataset.modelId ?? "");
      }
      this.updateSelectionStatus();
    });

    this.els.selectAll.addEventListener("click", () => {
      if (!this.allowMultipleSelection) {
        return;
      }
      this.setSelection(true);
    });

    this.els.selectNone.addEventListener("click", () => this.setSelection(false));
    this.els.importSelected.addEventListener("click", () => void this.importSelected(false));
    this.els.importBlend.addEventListener("click", () => void this.importSelected(true));

    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && this.els?.modal.style.display !== "none") {
        this.close();
      }
    });
  }

  private async loadModelsForTone(tone: Tone3000Tone): Promise<void> {
    if (!this.els) {
      return;
    }

    try {
      const models = await this.fetchModels(tone);

      // The list walks every page, so it can be slow, and the dialog may show another tone by now:
      // that tone's list is its own, and Import would otherwise take this one's models under its name.
      if (this.currentTone !== tone) {
        return;
      }

      this.currentModels = models;

      if (!models.length) {
        this.els.modelsStatus.textContent = "No models found for this tone.";
        this.updateSelectionStatus();
        this.setImportState(false);
        return;
      }

      this.renderModelList(models);
      this.setSelection(true);
      if (!this.allowMultipleSelection) {
        const first = models[0];
        this.setExclusiveSelection(String(first.id));
      }
      this.updateSelectionStatus();
    } catch (error) {
      if (this.currentTone !== tone) {
        return;
      }

      const message = error instanceof Error ? error.message : String(error);
      this.els.modelsStatus.textContent = `Unable to load models: ${message}`;
      this.updateSelectionStatus();
      this.setImportState(false);
    }
  }

  private renderModelList(models: Tone3000Model[]): void {
    if (!this.els) {
      return;
    }

    this.els.models.innerHTML = models
      .map((model) => {
        const label = model.name || String(model.id);
        return `
          <li class="tone3000-details-model-row">
            <label>
              <input class="tone3000-details-model-select" type="checkbox" data-model-id="${escapeHtml(model.id)}" checked />
              <span>${escapeHtml(label)}</span>
            </label>
          </li>
        `;
      })
      .join("");
  }

  private getSelectedModelIds(): Set<string> {
    const selected = new Set<string>();
    if (!this.els) {
      return selected;
    }

    this.els.models.querySelectorAll<HTMLInputElement>(".tone3000-details-model-select").forEach((input) => {
      if (input.checked && input.dataset.modelId) {
        selected.add(input.dataset.modelId);
      }
    });

    return selected;
  }

  private setSelection(checked: boolean): void {
    if (!this.els) {
      return;
    }

    this.els.models.querySelectorAll<HTMLInputElement>(".tone3000-details-model-select").forEach((input) => {
      input.checked = checked;
    });
    this.updateSelectionStatus();
  }

  private setExclusiveSelection(modelId: string): void {
    if (!this.els) {
      return;
    }

    this.els.models.querySelectorAll<HTMLInputElement>(".tone3000-details-model-select").forEach((input) => {
      input.checked = input.dataset.modelId === modelId;
    });
  }

  private updateSelectionStatus(): void {
    if (!this.els) {
      return;
    }

    const total = this.currentModels.length;
    if (!total) {
      return;
    }

    const selected = this.getSelectedModelIds().size;
    this.els.modelsStatus.textContent = `Selected ${selected} of ${total} models.`;
  }

  private setImportState(importing: boolean, progressText = ""): void {
    if (!this.els) {
      return;
    }

    this.detailsImporting = importing;
    this.els.importSelected.disabled = importing;
    this.els.importBlend.disabled = importing || !this.allowBlendCreation;
    this.els.selectAll.disabled = importing || !this.allowMultipleSelection;
    this.els.selectNone.disabled = importing;
    this.els.progress.textContent = progressText;
  }

  private syncModeUI(): void {
    if (!this.els) {
      return;
    }

    const selectContainer = this.els.selectAll.parentElement;
    if (selectContainer) {
      selectContainer.toggleAttribute("hidden", !this.allowMultipleSelection);
    }

    this.els.importBlend.toggleAttribute("hidden", !this.allowBlendCreation);
    this.els.importSelected.textContent = this.allowMultipleSelection ? "Import Selected" : "Import Model";
  }

  private async importSelected(createBlend: boolean): Promise<void> {
    if (!this.els || this.detailsImporting) {
      return;
    }

    if (createBlend && !this.allowBlendCreation) {
      return;
    }

    const tone = this.currentTone;
    if (!tone) {
      showNotification("Import failed", "Tone details unavailable");
      return;
    }

    const selectedIds = this.getSelectedModelIds();
    if (!selectedIds.size) {
      showNotification("Import failed", "Select at least one model.");
      return;
    }

    const models = this.currentModels.filter((model) => selectedIds.has(String(model.id)));
    if (!models.length) {
      showNotification("Import failed", "No matching models found.");
      return;
    }

    try {
      this.setImportState(true, "Preparing import...");
      await this.onImport(tone, models, createBlend);
      this.setImportState(false, "Import complete.");
      this.close();
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      showNotification("Import failed", message);
      this.setImportState(false, "");
    }
  }
}
