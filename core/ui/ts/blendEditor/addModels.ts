/**
 * The blend editor's Add Models dialog: every NAM model in the library, ticked to add several
 * at once, narrowed by search or by pack. Models already in the blend are shown, ticked and
 * fixed, so none goes in twice. Tone3000… hands over to the resource picker's Tone3000 tab,
 * whose picks and imports come back into the blend.
 */

import { deduplicateResourcesByHashAndPath } from "../resourceDedup.js";
import type { LibraryResource, ResourceLibrary } from "../types.js";
import { escapeHtml } from "../utils.js";
import { modelArchitecture } from "./editState.js";

const ALL_PACKS = "__all__";
const NO_PACK = "__none__";

function packOf(resource: LibraryResource): { id: string; title: string } | null {
  const metadata = resource.metadata ?? {};
  const id = metadata.toneId || metadata.groupId || "";
  const title = (metadata.toneTitle || metadata.groupName || "").trim();
  return id ? { id, title: title || `Pack ${id}` } : null;
}

export interface BlendAddModelsDeps {
  getResourceLibrary(): ResourceLibrary;
  /** Opens Tone3000 to pick or import models. */
  openTone3000?: () => void;
  /** Whether Tone3000 can be offered now (it is a feature that can be off). */
  canOpenTone3000?: () => boolean;
}

export class BlendAddModelsDialog {
  private readonly modal = document.getElementById("blend-add-models-modal");
  private readonly search = document.getElementById("blend-add-models-search") as HTMLInputElement | null;
  private readonly pack = document.getElementById("blend-add-models-pack") as HTMLSelectElement | null;
  private readonly list = document.getElementById("blend-add-models-list");
  private readonly selectAll = document.getElementById("blend-add-models-all") as HTMLInputElement | null;
  private readonly count = document.getElementById("blend-add-models-count");
  private readonly confirm = document.getElementById("blend-add-models-confirm") as HTMLButtonElement | null;
  private readonly tone3000 = document.getElementById("blend-add-models-tone3000") as HTMLButtonElement | null;

  private inBlend = new Set<string>();
  private picked = new Set<string>();
  private shown: LibraryResource[] = [];
  private onAdd: ((ids: string[]) => void) | null = null;
  private bound = false;

  constructor(private readonly deps: BlendAddModelsDeps) {}

  open(existingIds: string[], onAdd: (ids: string[]) => void): void {
    if (!this.modal) {
      return;
    }
    this.bind();
    this.inBlend = new Set(existingIds);
    this.picked.clear();
    this.onAdd = onAdd;
    if (this.search) {
      this.search.value = "";
    }
    this.tone3000?.toggleAttribute("hidden", !this.deps.openTone3000 || !(this.deps.canOpenTone3000?.() ?? true));
    this.renderPacks();
    this.render();
    this.modal.style.display = "flex";
    requestAnimationFrame(() => this.search?.focus());
  }

  close(): void {
    if (this.modal) {
      this.modal.style.display = "none";
    }
    this.onAdd = null;
  }

  isOpen(): boolean {
    return this.modal?.style.display === "flex";
  }

  private bind(): void {
    if (this.bound) {
      return;
    }
    this.bound = true;
    document.getElementById("blend-add-models-close")?.addEventListener("click", () => this.close());
    document.getElementById("blend-add-models-cancel")?.addEventListener("click", () => this.close());
    this.modal?.addEventListener("mousedown", (event) => {
      if (event.target === this.modal) {
        this.close();
      }
    });
    this.search?.addEventListener("input", () => this.render());
    this.pack?.addEventListener("change", () => this.render());
    this.selectAll?.addEventListener("change", () => {
      this.shown.forEach((resource) => {
        if (this.inBlend.has(resource.id)) {
          return;
        }
        if (this.selectAll?.checked) {
          this.picked.add(resource.id);
        } else {
          this.picked.delete(resource.id);
        }
      });
      this.render();
    });
    this.list?.addEventListener("change", (event) => {
      const input = event.target as HTMLInputElement | null;
      const id = input?.dataset.modelId;
      if (!input || !id) {
        return;
      }
      if (input.checked) {
        this.picked.add(id);
      } else {
        this.picked.delete(id);
      }
      this.updateCount();
    });
    this.confirm?.addEventListener("click", () => {
      const ids = [...this.picked];
      const onAdd = this.onAdd;
      this.close();
      if (ids.length) {
        onAdd?.(ids);
      }
    });
    this.tone3000?.addEventListener("click", () => {
      this.close();
      this.deps.openTone3000?.();
    });
  }

  private models(): LibraryResource[] {
    const resources = this.deps.getResourceLibrary().nam ?? [];
    return deduplicateResourcesByHashAndPath(resources, { preferredResourceIds: [...this.inBlend] }).deduped
      .filter((resource) => !resource.fileMissing);
  }

  /** "All packs", then the packs the blend already uses, then every other, by name. */
  private renderPacks(): void {
    if (!this.pack) {
      return;
    }
    const packs = new Map<string, string>();
    let unpacked = false;
    this.models().forEach((resource) => {
      const pack = packOf(resource);
      if (pack) {
        packs.set(pack.id, pack.title);
      } else {
        unpacked = true;
      }
    });
    const library = this.deps.getResourceLibrary().nam ?? [];
    const used = new Set(library.filter((resource) => this.inBlend.has(resource.id)).map((resource) => packOf(resource)?.id ?? ""));
    const ordered = [...packs.entries()].sort(([aId, a], [bId, b]) =>
      Number(used.has(bId)) - Number(used.has(aId)) || a.localeCompare(b));
    this.pack.innerHTML = [
      `<option value="${ALL_PACKS}">All packs</option>`,
      ...ordered.map(([id, title]) => `<option value="${escapeHtml(id)}">${escapeHtml(title)}${used.has(id) ? " (in blend)" : ""}</option>`),
      unpacked ? `<option value="${NO_PACK}">Not from a pack</option>` : "",
    ].join("");
    this.pack.value = ALL_PACKS;
  }

  private render(): void {
    if (!this.list) {
      return;
    }
    const query = (this.search?.value ?? "").trim().toLowerCase();
    const pack = this.pack?.value ?? ALL_PACKS;
    this.shown = this.models()
      .filter((resource) => {
        const resourcePack = packOf(resource);
        if (pack === NO_PACK ? resourcePack : pack !== ALL_PACKS && resourcePack?.id !== pack) {
          return false;
        }
        return !query || `${resource.name} ${resourcePack?.title ?? ""} ${resource.category ?? ""}`.toLowerCase().includes(query);
      })
      .sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));

    this.list.innerHTML = this.shown.length
      ? this.shown
        .map((resource) => {
          const inBlend = this.inBlend.has(resource.id);
          const checked = inBlend || this.picked.has(resource.id);
          const packTitle = packOf(resource)?.title ?? "";
          return `
            <label class="blend-add-models-item${inBlend ? " is-in-blend" : ""}">
              <input type="checkbox" data-model-id="${escapeHtml(resource.id)}" ${checked ? "checked" : ""} ${inBlend ? "disabled" : ""} />
              <span class="blend-add-models-name">${escapeHtml(resource.name || resource.id)}</span>
              <span class="blend-add-models-meta">${inBlend ? "In blend" : escapeHtml([packTitle, modelArchitecture(resource)].filter(Boolean).join(" · "))}</span>
            </label>`;
        })
        .join("")
      : `<div class="blend-add-models-empty">No models match.</div>`;
    this.updateCount();
  }

  private updateCount(): void {
    const addable = this.shown.filter((resource) => !this.inBlend.has(resource.id));
    if (this.selectAll) {
      this.selectAll.checked = addable.length > 0 && addable.every((resource) => this.picked.has(resource.id));
      this.selectAll.disabled = !addable.length;
    }
    if (this.count) {
      this.count.textContent = `${this.shown.length} shown`;
    }
    if (this.confirm) {
      this.confirm.disabled = !this.picked.size;
      this.confirm.textContent = this.picked.size ? `Add ${this.picked.size}` : "Add";
    }
  }
}
