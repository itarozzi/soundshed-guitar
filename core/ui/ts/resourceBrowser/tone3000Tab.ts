/**
 * The resource browser's Tone3000 tab: searching the remote catalog, auditioning
 * a model before committing to it, and importing the one that is chosen.
 *
 * It also owns the navigation state a Tone3000 selection leaves behind, so the
 * node's prev/next controls keep walking that result set after the modal has
 * closed — including fetching and importing models that were never downloaded.
 *
 * It reaches back into the modal only through `Tone3000TabHost`.
 */

import { ensureTone3000Session, isTone3000AuthReady, isTone3000ByokEnabled, tone3000AuthenticatedFetch } from "../tone3000.js";
import { buildTone3000FavoritesUrl, buildTone3000SearchUrl, extractTone3000Tones, parseTone3000Pagination } from "../tone3000Api.js";
import type { Tone3000Architecture, Tone3000Model, Tone3000Tone } from "../tone3000ApiTypes.js";
import { backfillTone3000ResourceImages, fetchTone3000Models, getTone3000ImageUrl } from "../tone3000Shared.js";
import { escapeHtml } from "../utils.js";
import { isDoubleClickExempt, isSecondClickOfDouble } from "./helpers.js";
import { TONE3000_IR_GEAR_OPTIONS } from "./slot.js";
import { Tone3000BulkImporter } from "./tone3000BulkImport.js";
import { renderToneDetails, renderToneMetaParts, renderToneModelRow } from "./tone3000Rows.js";
import { Tone3000Navigator } from "./tone3000Navigation.js";
import type { PersistedResourceBrowserState, PreviewLoadingState, PreviewState, ResourceBrowserOptions, ResourceType } from "./types.js";

/**
 * What the Tone3000 tab needs from the modal around it.
 *
 * As with the Folder tab, the modal builds this as an object of closures, so
 * nothing in `ResourceBrowserModal` has to become public for the tab to move.
 */
export interface Tone3000TabHost {
  /** What the modal was opened for: the node, the resource type, the callback. */
  getOptions(): ResourceBrowserOptions | null;
  /** The architecture filter, which the Library tab shares. */
  getSelectedArchitecture(): Tone3000Architecture | null;
  /** The effect-role key that navigation state is filed under. */
  getContextKey(): string;
  rememberNavigationView(contextKey: string, view: "library" | "folder" | "tone3000"): void;
  getNavigationView(contextKey: string): "library" | "folder" | "tone3000" | undefined;
  /** The preview currently playing through the node, if any. */
  getPreviewState(): PreviewState | null;
  /** The model whose download is still in flight, if any. */
  getPreviewLoading(): PreviewLoadingState | null;
  startPreview(toneId: string, modelId: string, modelUrl: string): Promise<void>;
  cancelPreview(restoreOriginal?: boolean): void;
  selectAndImportModel(toneId: string, modelId: string, modelUrl: string, modelName: string): Promise<void>;
  /** Close the picker, reverting any preview; Create Blend leaves it for the blend editor. */
  closeModal(): void;
  /** The "sign in to see favourites" placeholder, shared with the Library tab. */
  renderFavoritesPrompt(): string;
  /** Persist the per-resource-type state after a filter change. */
  saveState(): void;
}

export class Tone3000Tab {
  /**
   * Where a Tone3000 selection leaves the node pointing, so its prev/next
   * controls keep walking this result set once the modal has closed.
   */
  readonly navigation = new Tone3000Navigator({
    getOptions: () => this.host.getOptions(),
    getSelectedArchitecture: () => this.host.getSelectedArchitecture(),
    getContextKey: () => this.host.getContextKey(),
    rememberNavigationView: (contextKey, view) => this.host.rememberNavigationView(contextKey, view),
    getNavigationView: (contextKey) => this.host.getNavigationView(contextKey),
    getTones: () => this.tone3000Tones,
    getModelsCache: () => this.toneModelsCache,
  });

  /** A tone's Import All and Create Blend, and which of its models are in the library. */
  private readonly bulkImport = new Tone3000BulkImporter({
    navigation: this.navigation,
    getOptions: () => this.host.getOptions(),
    closeModal: () => this.host.closeModal(),
    render: () => this.renderTone3000List(),
  });

  constructor(private readonly host: Tone3000TabHost) {}
  /**
   * Look up this tab's DOM and bind it. Called once from the modal's own
   * initialize(), so every tab is wired at the same point.
   */
  initialize(): void {
    this.tone3000ModeSearchTab = document.getElementById("resource-browser-tone3000-mode-search-tab") as HTMLButtonElement | null;
    this.tone3000ModeFavoritesTab = document.getElementById("resource-browser-tone3000-mode-favorites-tab") as HTMLButtonElement | null;
    this.tone3000SearchControls = document.getElementById("resource-browser-tone3000-search-controls");
    this.tone3000Search = document.getElementById("resource-browser-tone3000-search") as HTMLInputElement | null;
    this.tone3000SearchBtn = document.getElementById("resource-browser-tone3000-search-btn") as HTMLButtonElement | null;
    this.tone3000Category = document.getElementById("resource-browser-tone3000-category") as HTMLSelectElement | null;
    this.tone3000Sort = document.getElementById("resource-browser-tone3000-sort") as HTMLSelectElement | null;
    this.tone3000Architecture = document.getElementById("resource-browser-tone3000-architecture") as HTMLSelectElement | null;
    this.tone3000List = document.getElementById("resource-browser-tone3000-list");
    this.tone3000Pagination = document.getElementById("resource-browser-tone3000-pagination");
    this.tone3000PrevBtn = document.getElementById("resource-browser-tone3000-prev") as HTMLButtonElement | null;
    this.tone3000NextBtn = document.getElementById("resource-browser-tone3000-next") as HTMLButtonElement | null;
    this.tone3000PageLabel = document.getElementById("resource-browser-tone3000-page-label");
    this.tone3000Status = document.getElementById("resource-browser-tone3000-status");

    this.tone3000Search?.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        void this.runTone3000Search();
      }
    });
    this.tone3000SearchBtn?.addEventListener("click", () => void this.runTone3000Search());
    this.tone3000ModeSearchTab?.addEventListener("click", () => {
      if (this.tone3000FavoritesOnly) {
        this.tone3000FavoritesOnly = false;
        this.syncTone3000ModeUi();
        this.host.saveState();
        void this.runTone3000Search(1);
      }
    });
    this.tone3000ModeFavoritesTab?.addEventListener("click", () => {
      if (!this.tone3000FavoritesOnly) {
        this.tone3000FavoritesOnly = true;
        this.syncTone3000ModeUi();
        this.host.saveState();
        void this.runTone3000Search(1);
      }
    });
    this.tone3000Category?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Sort?.addEventListener("change", () => void this.runTone3000Search());
    this.tone3000Architecture?.addEventListener("change", () => {
      this.expandedToneId = null;
      this.toneModelsCache.clear();
      this.toneModelErrors.clear();
      ++this.toneModelsGeneration;
      this.host.saveState();
      void this.runTone3000Search();
    });

    this.tone3000PrevBtn?.addEventListener("click", () => {
      if (this.tone3000Page > 1) {
        void this.runTone3000Search(this.tone3000Page - 1);
      }
    });
    this.tone3000NextBtn?.addEventListener("click", () => {
      if (this.tone3000Page < this.tone3000TotalPages) {
        void this.runTone3000Search(this.tone3000Page + 1);
      }
    });

    this.tone3000List?.addEventListener("click", (event) => this.handleTone3000Click(event));
    this.tone3000List?.addEventListener("dblclick", (event) => void this.handleTone3000DoubleClick(event));
  }

  /** The model row the last click landed on, for a double click whose second click lands on a repainted list. */
  private lastClickedModelSelect: { data: DOMStringMap; at: number } | null = null;

  // Tone3000 tab elements
  private tone3000ModeSearchTab: HTMLButtonElement | null = null;

  private tone3000ModeFavoritesTab: HTMLButtonElement | null = null;

  private tone3000SearchControls: HTMLElement | null = null;

  private tone3000Search: HTMLInputElement | null = null;

  private tone3000SearchBtn: HTMLButtonElement | null = null;

  tone3000Category: HTMLSelectElement | null = null;

  private tone3000Sort: HTMLSelectElement | null = null;

  tone3000Architecture: HTMLSelectElement | null = null;

  tone3000List: HTMLElement | null = null;

  private tone3000Pagination: HTMLElement | null = null;

  private tone3000PrevBtn: HTMLButtonElement | null = null;

  private tone3000NextBtn: HTMLButtonElement | null = null;

  tone3000PageLabel: HTMLElement | null = null;

  tone3000Status: HTMLElement | null = null;

  // Tone3000 state
  private tone3000Query = "";

  tone3000Tones: Tone3000Tone[] = [];

  tone3000Page = 1;

  tone3000TotalPages = 1;

  tone3000FavoritesOnly = false;

  private expandedToneId: string | null = null;

  private expandedToneSection: "models" | "details" = "models";

  toneModelsCache: Map<string, Tone3000Model[]> = new Map();
  /** A tone whose models failed to load, with why: shown, and retried on the next expand, never
   *  cached (the cache is persisted, so a passing network error read "No models" for good). */
  private toneModelErrors: Map<string, string> = new Map();
  /** Bumped when the cache is cleared for an architecture change, so a fetch for the old one is dropped. */
  private toneModelsGeneration = 0;

  /** The Tone3000 half of the per-resource-type state the modal remembers. */
  captureTone3000State(persisted: PersistedResourceBrowserState, resourceType: ResourceType): void {
    persisted.tone3000Search = this.tone3000Search?.value ?? "";
    persisted.tone3000Category = this.tone3000Category?.value ?? (resourceType === "ir" ? "cab" : "amp");
    persisted.tone3000Sort = this.tone3000Sort?.value ?? "popular";
    persisted.tone3000Architecture = this.tone3000Architecture?.value ?? (resourceType === "ir" ? "all" : "2");
    persisted.tone3000FavoritesOnly = this.tone3000FavoritesOnly;
    persisted.tone3000Page = this.tone3000Page;
    persisted.tone3000TotalPages = this.tone3000TotalPages;
    persisted.tone3000Tones = [...this.tone3000Tones];
    persisted.expandedToneId = this.expandedToneId;
    persisted.toneModelsCache = Array.from(this.toneModelsCache.entries());
  }

  /** The Tone3000 controls: search box, filters, and the favourites toggle. */
  restoreTone3000Controls(persisted: PersistedResourceBrowserState, resourceType: ResourceType): void {
    if (this.tone3000Search) {
      this.tone3000Search.value = persisted.tone3000Search;
      this.tone3000Search.placeholder = resourceType === "ir"
        ? "Search IRs..."
        : "Search amps and pedals...";
    }

    if (this.tone3000Category) {
      this.tone3000Category.value = persisted.tone3000Category;
    }

    if (this.tone3000Sort) {
      this.tone3000Sort.value = persisted.tone3000Sort;
    }

    if (this.tone3000Architecture) {
      this.tone3000Architecture.value = persisted.tone3000Architecture;
    }

    this.tone3000FavoritesOnly = Boolean(persisted.tone3000FavoritesOnly);
    this.syncTone3000ModeUi();
  }

  /** The result set itself, so reopening the modal shows the same page again. */
  restoreTone3000Results(persisted: PersistedResourceBrowserState): void {
    this.tone3000Query = persisted.tone3000Search;
    this.tone3000Page = persisted.tone3000Page;
    this.tone3000TotalPages = persisted.tone3000TotalPages;
    this.tone3000Tones = [...persisted.tone3000Tones];
    this.expandedToneId = persisted.expandedToneId;
    this.toneModelsCache = new Map(persisted.toneModelsCache);
  }

  private canUseTone3000FavoritesMode(): boolean {
    return isTone3000ByokEnabled();
  }

  private syncTone3000ModeUi(): void {
    const enabled = this.canUseTone3000FavoritesMode();
    if (this.tone3000ModeFavoritesTab) {
      this.tone3000ModeFavoritesTab.classList.toggle("active", this.tone3000FavoritesOnly);
      this.tone3000ModeFavoritesTab.setAttribute("aria-pressed", this.tone3000FavoritesOnly ? "true" : "false");
      this.tone3000ModeFavoritesTab.title = enabled
        ? "Show your Tone3000 favourites"
        : "Favourites require your own Tone3000 API key in Settings";
    }

    if (this.tone3000ModeSearchTab) {
      this.tone3000ModeSearchTab.classList.toggle("active", !this.tone3000FavoritesOnly);
      this.tone3000ModeSearchTab.setAttribute("aria-pressed", !this.tone3000FavoritesOnly ? "true" : "false");
    }

    if (this.tone3000SearchControls) {
      this.tone3000SearchControls.style.display = this.tone3000FavoritesOnly ? "none" : "";
    }
  }

  /** Bumped by every search, so a reply that is not the latest search's is dropped. */
  private tone3000SearchSequence = 0;

  invalidateRequests(): void {
    ++this.tone3000SearchSequence;
    ++this.toneModelsGeneration;
    if (this.expandedToneId && !this.toneModelsCache.has(this.expandedToneId)) {
      this.expandedToneId = null;
    }
  }

  /**
   * Shows the one tone a model came from, with its models. Resolves to why it could not be
   * shown (no session, the model list refused or failed, no models left), so the picker can
   * fall back to the model in the Library; null once shown, or once the picker has moved on.
   */
  async openSourceTone(tone: Tone3000Tone, architectureVersion?: string): Promise<string | null> {
    const options = this.host.getOptions();
    if (!this.tone3000List || !options) {
      return null;
    }

    this.invalidateRequests();
    const sequence = this.tone3000SearchSequence;
    const isStale = (): boolean => sequence !== this.tone3000SearchSequence || this.host.getOptions() !== options;
    const architecture = options.resourceType === "nam"
      && (architectureVersion === "1" || architectureVersion === "2" || architectureVersion === "custom")
      ? architectureVersion : undefined;
    if (this.tone3000Architecture) {
      this.tone3000Architecture.value = architecture ?? "all";
    }
    if (this.tone3000Search) {
      this.tone3000Search.value = "";
    }
    this.tone3000Query = "";
    this.tone3000FavoritesOnly = false;
    this.syncTone3000ModeUi();
    this.tone3000Tones = [];
    this.toneModelsCache.clear();
    this.toneModelErrors.clear();
    this.expandedToneId = null;
    this.expandedToneSection = "models";
    this.tone3000Page = 1;
    this.tone3000TotalPages = 1;
    this.tone3000List.innerHTML = `<div class="resource-browser-empty">Loading source pack...</div>`;
    if (this.tone3000Status) {
      this.tone3000Status.textContent = "";
    }
    this.updateTone3000Pagination(true);

    try {
      await ensureTone3000Session();
      if (isStale()) {
        return null;
      }
      if (!isTone3000AuthReady()) {
        throw new Error("Add a Tone3000 API key in Settings to browse.");
      }
      this.tone3000Tones = [tone];
      await this.toggleToneExpanded(String(tone.id));
      if (isStale()) {
        return null;
      }
      this.updateTone3000PaginationFromData({ page: 1, total: 1, total_pages: 1 }, 1);
      this.host.saveState();
      // A refused or failed model list (a 403 most often) is shown inside the tone, not thrown.
      const failure = this.toneModelErrors.get(String(tone.id));
      if (failure || !this.toneModelsCache.get(String(tone.id))?.length) {
        return failure ? `Its models could not be loaded (${failure}).` : "It has no models to show.";
      }
      return null;
    } catch (error) {
      if (isStale()) {
        return null;
      }
      const message = error instanceof Error ? error.message : String(error);
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">Could not open source pack: ${escapeHtml(message)} Use Search to browse other tones.</div>`;
      this.updateTone3000PaginationFromData({ page: 1, total: 1, total_pages: 1 }, 1);
      return message;
    }
  }

  async runTone3000Search(page = 1): Promise<void> {
    if (!this.tone3000List || !this.host.getOptions()) {
      return;
    }

    // The picker is one modal shared by every slot: a model search still in flight when it is
    // opened for an IR slot (or the category, sort or page changes) could land last and fill it.
    this.invalidateRequests();
    const sequence = this.tone3000SearchSequence;
    const isStale = (): boolean => sequence !== this.tone3000SearchSequence;

    this.syncTone3000ModeUi();
    
    const useFavoritesMode = this.canUseTone3000FavoritesMode() && this.tone3000FavoritesOnly;
    if (this.tone3000FavoritesOnly && !this.canUseTone3000FavoritesMode()) {
      this.tone3000List.innerHTML = this.host.renderFavoritesPrompt();
      this.updateTone3000Pagination(false);
      return;
    }

    await ensureTone3000Session();
    if (isStale()) {
      return;
    }
    if (!isTone3000AuthReady()) {
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">Add a Tone3000 API key in Settings to browse.</div>`;
      this.updateTone3000Pagination(false);
      return;
    }
    
    this.tone3000Query = this.tone3000Search?.value.trim() ?? "";
    this.tone3000Page = page;
    
    this.tone3000List.innerHTML = `<div class="resource-browser-empty">Loading...</div>`;
    this.updateTone3000Pagination(true);
    
    try {
      const params = new URLSearchParams({
        page: String(page),
        page_size: "20",
      });
      
      if (!useFavoritesMode) {
        if (this.tone3000Query) {
          params.set("query", this.tone3000Query);
        }
        
        // Set gear filter based on category. IRs search the select's own gear: cab IRs,
        // rooms ("space"), or every IR.
        const isIr = this.host.getOptions()?.resourceType === "ir";
        const categoryValue = this.tone3000Category?.value
          ?? (isIr ? "cab" : (this.host.getOptions()?.tone3000CategoryFilter ?? "amp"));
        if (isIr) {
          params.set("gear", TONE3000_IR_GEAR_OPTIONS.some((gear) => gear.value === categoryValue) ? categoryValue : "ir");
        } else if (categoryValue === "pedal") {
          params.set("gear", "pedal");
        } else if (categoryValue === "preamp") {
          params.set("gear", "outboard");
        } else if (categoryValue === "full-rig") {
          params.set("gear", "full-rig");
        } else {
          params.set("gear", "amp");
        }
        
        // Set sort
        const sortValue = this.tone3000Sort?.value ?? "popular";
        if (sortValue === "popular") {
          params.set("sort", "downloads-all-time");
        } else if (sortValue === "recent") {
          params.set("sort", "newest");
        } else if (sortValue === "trending") {
          params.set("sort", "trending");
        }

        const architecture = this.host.getSelectedArchitecture();
        if (architecture) {
          params.set("architecture", architecture);
        }
      }
      
      const endpoint = useFavoritesMode ? buildTone3000FavoritesUrl(params) : buildTone3000SearchUrl(params);
      const response = await tone3000AuthenticatedFetch(endpoint);
      if (isStale()) {
        return;
      }
      
      if (!response.ok) {
        if (useFavoritesMode && (response.status === 401 || response.status === 403 || response.status === 404)) {
          throw new Error("Favourites are only available in BYOK direct API mode.");
        }
        throw new Error(`Search failed: ${response.status}`);
      }
      
      const data = await response.json();
      if (isStale()) {
        return;
      }
      const tones = extractTone3000Tones(data);
      backfillTone3000ResourceImages(tones);

      // No client-side filtering needed - the API gear param already filters
      this.tone3000Tones = tones;
      this.updateTone3000PaginationFromData(data, tones.length);
      this.renderTone3000List();
    } catch (error) {
      if (isStale()) {
        return;
      }
      const message = error instanceof Error ? error.message : String(error);
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">Error: ${escapeHtml(message)}</div>`;
      this.updateTone3000Pagination(false);
    }
  }

  updateTone3000Pagination(loading: boolean): void {
    if (!this.tone3000Pagination || !this.tone3000PageLabel || !this.tone3000PrevBtn || !this.tone3000NextBtn) {
      return;
    }
    
    this.tone3000Pagination.style.opacity = loading ? "0.6" : "1";
    this.tone3000PageLabel.textContent = `Page ${this.tone3000Page}`;
    this.tone3000PrevBtn.disabled = loading || this.tone3000Page <= 1;
    this.tone3000NextBtn.disabled = loading;
  }

  private updateTone3000PaginationFromData(data: Record<string, unknown>, _pageSize: number): void {
    const parsed = parseTone3000Pagination(data, this.tone3000Page, 20);
    this.tone3000Page = parsed.page;
    this.tone3000TotalPages = parsed.total ? parsed.totalPages : this.tone3000Page;
    
    if (this.tone3000PageLabel) {
      this.tone3000PageLabel.textContent = `Page ${this.tone3000Page} of ${this.tone3000TotalPages}`;
    }
    if (this.tone3000PrevBtn) {
      this.tone3000PrevBtn.disabled = this.tone3000Page <= 1;
    }
    if (this.tone3000NextBtn) {
      this.tone3000NextBtn.disabled = this.tone3000Page >= this.tone3000TotalPages;
    }
    if (this.tone3000Pagination) {
      this.tone3000Pagination.style.opacity = "1";
    }
  }

  renderTone3000List(): void {
    if (!this.tone3000List) {
      return;
    }
    
    if (!this.tone3000Tones.length) {
      const emptyLabel = this.tone3000FavoritesOnly
        ? "No favourite tones found. Favourite them on Tone3000 first, then refresh."
        : "No tones found. Try a different search.";
      this.tone3000List.innerHTML = `<div class="resource-browser-empty">${escapeHtml(emptyLabel)}</div>`;
      return;
    }
    
    this.tone3000List.innerHTML = this.tone3000Tones
      .map((tone) => {
        const isExpanded = this.expandedToneId === String(tone.id);
        const imageUrl = this.getToneImageUrl(tone);
        const cachedModels = this.toneModelsCache.get(String(tone.id));
        const modelCount = cachedModels?.length ?? tone.models_count;
        const resourceType = this.host.getOptions()?.resourceType ?? "nam";
        
        const imageMarkup = imageUrl
          ? `<img class="resource-browser-tone-image" src="${escapeHtml(imageUrl)}" alt="" loading="lazy" />`
          : `<div class="resource-browser-tone-image-placeholder"></div>`;
        
        const expandedClass = isExpanded ? "resource-browser-tone is-expanded" : "resource-browser-tone";
        const expandedContentHtml = isExpanded ? this.renderToneExpandedContent(tone) : "";
        const displayTitle = tone.title || tone.name || "Untitled Tone";
        const metaParts = renderToneMetaParts(tone, modelCount, this.tone3000FavoritesOnly);
        const toggleLabel = isExpanded ? "Hide models" : "Show models";

        return `
          <div class="${expandedClass}" data-tone-id="${escapeHtml(String(tone.id))}">
            <div class="resource-browser-tone-header">
              ${imageMarkup}
              <div class="resource-browser-tone-info">
                <div class="resource-browser-tone-title">${escapeHtml(displayTitle)}</div>
                <div class="resource-browser-tone-meta">${metaParts}${this.bulkImport.renderToneBadge(tone, cachedModels, resourceType)}</div>
              </div>
              <button class="resource-browser-tone-expand" type="button" data-tone-id="${escapeHtml(String(tone.id))}" title="${toggleLabel}" aria-label="${toggleLabel}" aria-expanded="${isExpanded ? "true" : "false"}">
                <svg aria-hidden="true" xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"></polyline></svg>
              </button>
            </div>
            ${expandedContentHtml}
          </div>
        `;
      })
      .join("");
  }

  private renderToneExpandedContent(tone: Tone3000Tone): string {
    const modelsActive = this.expandedToneSection === "models";
    return `
      <div class="resource-browser-tone-sections" data-tone-id="${escapeHtml(String(tone.id))}">
        <div class="resource-browser-tone-section-tabs" role="tablist" aria-label="Tone sections">
          <button
            class="resource-browser-tone-section-tab ${modelsActive ? "is-active" : ""}"
            type="button"
            role="tab"
            aria-selected="${modelsActive ? "true" : "false"}"
            data-tone-id="${escapeHtml(String(tone.id))}"
            data-tone-section="models"
          >Models</button>
          <button
            class="resource-browser-tone-section-tab ${!modelsActive ? "is-active" : ""}"
            type="button"
            role="tab"
            aria-selected="${!modelsActive ? "true" : "false"}"
            data-tone-id="${escapeHtml(String(tone.id))}"
            data-tone-section="details"
          >Details</button>
        </div>
        <div class="resource-browser-tone-section-panel" role="tabpanel">
          ${modelsActive ? this.renderToneModels(tone) : renderToneDetails(tone)}
        </div>
      </div>
    `;
  }

  private renderToneModels(tone: Tone3000Tone): string {
    const models = this.toneModelsCache.get(String(tone.id));
    
    if (!models) {
      const error = this.toneModelErrors.get(String(tone.id));
      return error
        ? `<div class="resource-browser-tone-models"><div class="resource-browser-empty">Could not load models: ${escapeHtml(error)}</div></div>`
        : `<div class="resource-browser-tone-models"><div class="resource-browser-empty">Loading models...</div></div>`;
    }
    
    if (!models.length) {
      return `<div class="resource-browser-tone-models"><div class="resource-browser-empty">No models available.</div></div>`;
    }
    
    const previewState = this.host.getPreviewState();
    const previewingModelId = previewState?.toneId === String(tone.id) ? previewState.modelId : null;
    const previewLoading = this.host.getPreviewLoading();
    const loadingModelId = previewLoading?.toneId === String(tone.id) ? previewLoading.modelId : null;
    // A preview plays through the node, so a browser opened without one can only select.
    const canPreview = Boolean(this.host.getOptions()?.nodeId);
    const resourceType = this.host.getOptions()?.resourceType ?? "nam";

    return `
      <div class="resource-browser-tone-models">
        ${this.bulkImport.renderToolbar(tone, models, resourceType)}
        ${models.map((model) => renderToneModelRow(tone, model, {
          isPreviewing: String(model.id) === previewingModelId,
          isLoadingPreview: String(model.id) === loadingModelId,
          isImported: this.bulkImport.isModelImported(resourceType, model),
          canPreview,
        })).join("")}
      </div>
    `;
  }

  private getToneImageUrl(tone: Tone3000Tone): string | null {
    return getTone3000ImageUrl(tone);
  }

  private async handleTone3000Click(event: Event): Promise<void> {
    const target = event.target as HTMLElement | null;
    if (!target) {
      return;
    }
    
    // Handle expand button
    const expandBtn = target.closest(".resource-browser-tone-expand") as HTMLButtonElement | null;
    if (expandBtn) {
      const toneId = expandBtn.dataset.toneId;
      if (toneId) {
        await this.toggleToneExpanded(toneId);
      }
      return;
    }

    const sectionTabBtn = target.closest(".resource-browser-tone-section-tab") as HTMLButtonElement | null;
    if (sectionTabBtn) {
      const toneId = sectionTabBtn.dataset.toneId;
      const section = sectionTabBtn.dataset.toneSection;
      if (!toneId || this.expandedToneId !== toneId) {
        return;
      }
      if (section === "models" || section === "details") {
        this.expandedToneSection = section;
        this.renderTone3000List();
      }
      return;
    }

    // Expand/collapse when the user clicks anywhere on the tone row header.
    const toneHeader = target.closest(".resource-browser-tone-header") as HTMLElement | null;
    if (toneHeader) {
      const toneContainer = toneHeader.closest(".resource-browser-tone") as HTMLElement | null;
      const toneId = toneContainer?.dataset.toneId;
      if (toneId) {
        await this.toggleToneExpanded(toneId);
      }
      return;
    }
    
    const importAllBtn = target.closest(".resource-browser-tone-import-all, .resource-browser-tone-create-blend") as HTMLButtonElement | null;
    if (importAllBtn) {
      const toneId = importAllBtn.dataset.toneId ?? "";
      const tone = this.tone3000Tones.find((t) => String(t.id) === toneId);
      const models = this.toneModelsCache.get(toneId);
      if (tone && models) {
        await this.bulkImport.importAll(tone, models, importAllBtn.classList.contains("resource-browser-tone-create-blend"));
      }
      return;
    }

    // What is left is a model row: its preview or Select button, or the row itself.
    const modelRow = target.closest(".resource-browser-model") as HTMLElement | null;
    const rowSelectBtn = modelRow?.querySelector<HTMLButtonElement>(".resource-browser-model-select");
    if (!modelRow || !rowSelectBtn) {
      return;
    }
    this.lastClickedModelSelect = { data: { ...rowSelectBtn.dataset }, at: Date.now() };
    if (isSecondClickOfDouble(event)) {
      return;
    }

    if (target.closest(".resource-browser-model-select")) {
      const { toneId = "", modelId = "", modelUrl = "", modelName = "" } = rowSelectBtn.dataset;
      await this.host.selectAndImportModel(toneId, modelId, modelUrl, modelName);
      return;
    }

    // A click on the row previews it, as a click on a row does in the library and
    // folder tabs. The preview button does the same, and stops the one playing.
    const previewBtn = modelRow.querySelector<HTMLButtonElement>(".resource-browser-model-preview");
    if (!previewBtn || previewBtn.hidden) {
      return;
    }
    const toneId = previewBtn.dataset.toneId ?? "";
    const modelId = previewBtn.dataset.modelId ?? "";
    const loading = this.host.getPreviewLoading();
    if (loading?.toneId === toneId && loading.modelId === modelId) {
      return;
    }
    const previewState = this.host.getPreviewState();
    if (previewState?.toneId === toneId && previewState.modelId === modelId) {
      if (target.closest(".resource-browser-model-preview")) {
        this.host.cancelPreview();
      }
      return;
    }
    await this.host.startPreview(toneId, modelId, previewBtn.dataset.modelUrl ?? "");
  }

  /// A double click on a model row takes it, the same as Select. The first click
  /// of the pair started a preview, which repaints the list, so the second click
  /// may land on a fresh row or on the list itself: what the first click hit is
  /// the model the user meant.
  private async handleTone3000DoubleClick(event: Event): Promise<void> {
    const target = event.target as HTMLElement | null;
    if (!target || isDoubleClickExempt(target) || target.closest(".resource-browser-tone-header, .resource-browser-tone-section-tabs")) {
      return;
    }
    const rowSelectBtn = target.closest(".resource-browser-model")?.querySelector<HTMLButtonElement>(".resource-browser-model-select");
    const recent = this.lastClickedModelSelect && Date.now() - this.lastClickedModelSelect.at < 1000
      ? this.lastClickedModelSelect.data
      : null;
    // Only a row, or the bare list a repaint left under the pointer, means "this model".
    const selection = rowSelectBtn ? { ...rowSelectBtn.dataset } : target === this.tone3000List ? recent : null;
    if (!selection?.toneId || !selection.modelId) {
      return;
    }
    await this.host.selectAndImportModel(selection.toneId, selection.modelId, selection.modelUrl ?? "", selection.modelName ?? "");
  }

  private async toggleToneExpanded(toneId: string): Promise<void> {
    if (this.expandedToneId === toneId) {
      this.expandedToneId = null;
      this.expandedToneSection = "models";
      this.renderTone3000List();
      return;
    }
    
    this.expandedToneId = toneId;
    this.expandedToneSection = "models";
    this.renderTone3000List();
    
    // Load models if not cached
    if (!this.toneModelsCache.has(toneId)) {
      const tone = this.tone3000Tones.find((t) => String(t.id) === toneId);
      if (tone) {
        // Models are fetched for the architecture selected now. A change of architecture while
        // this is in flight clears the cache, and this tone's list for the old one must not
        // come back into it to show, and import, the wrong architecture's models.
        const generation = this.toneModelsGeneration;
        this.toneModelErrors.delete(toneId);
        try {
          const models = await this.fetchToneModels(tone);
          if (generation !== this.toneModelsGeneration) {
            return;
          }
          this.toneModelsCache.set(toneId, models);
          this.renderTone3000List();
        } catch (error) {
          console.error("Failed to fetch models:", error);
          if (generation !== this.toneModelsGeneration) {
            return;
          }
          this.toneModelErrors.set(toneId, error instanceof Error ? error.message : String(error));
          this.renderTone3000List();
        }
      }
    }
  }

  private async fetchToneModels(tone: Tone3000Tone): Promise<Tone3000Model[]> {
    if (!isTone3000AuthReady()) {
      throw new Error("No session");
    }

    return fetchTone3000Models(tone, this.host.getSelectedArchitecture() ?? undefined);
  }
}
