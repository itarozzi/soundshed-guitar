/**
 * A slot's next/prev list as rows, for a list drawn outside the browser (the IR Cabinet's list
 * between its two slots), and what that list can do with them: hand the list to another slot,
 * fetch the IRs of a Tone3000 tone, and import a Tone3000 model before it is loaded.
 *
 * Which list a slot walks follows the browser's own rule (getAdjacentResourceSelection): the
 * Tone3000 set it was last picked from, else the folder, else the library.
 */

import { uiState } from "../state.js";
import { findResourceById } from "../utils.js";
import { copyFolderLocation } from "./folderRoots.js";
import { DEFAULT_RESOURCE_CONTEXT_KEY } from "./settings.js";
import type { Tone3000Navigator } from "./tone3000Navigation.js";
import type { NavigationCacheOptions, ResourceNavigationResult, ResourceNavigationState, ResourceType, Tone3000NavigationState } from "./types.js";

export type NavigationView = "library" | "folder" | "tone3000";

export interface NavigationListItem {
  /** Unique within the listing. */
  key: string;
  label: string;
  detail?: string;
  resourceId?: string;
  filePath?: string;
  /** A Tone3000 row: a model, or a tone whose models have not been fetched yet (no modelId). */
  tone3000?: { toneId: string; modelId?: string };
}

export interface NavigationListing {
  view: NavigationView;
  title: string;
  items: NavigationListItem[];
}

/** The browser's own records, which these lists read and (for adopt) write. */
export interface NavigationListingHost {
  /** The list each context last picked from. */
  views: Map<string, NavigationView>;
  /** The folder each context last picked from, per context. */
  folderStates: Map<string, ResourceNavigationState>;
  navigator: Tone3000Navigator;
  getLibraryState(resourceType: ResourceType, options: NavigationCacheOptions): ResourceNavigationState | null;
}

function contextOf(options: NavigationCacheOptions): string {
  return options.contextKey || DEFAULT_RESOURCE_CONTEXT_KEY;
}

function fileLabel(filePath: string): string {
  return (filePath.split(/[\\/]/).pop() ?? filePath).replace(/\.[^.]+$/, "");
}

export class NavigationLists {
  constructor(private readonly host: NavigationListingHost) {}

  /** The list a slot's next/prev walks. */
  effectiveView(resourceType: ResourceType, options: NavigationCacheOptions): NavigationView {
    if (this.host.navigator.isTone3000NavigationActive(resourceType, options)) {
      return "tone3000";
    }
    const folderState = this.host.folderStates.get(contextOf(options));
    return this.host.views.get(contextOf(options)) === "folder" && folderState?.resourceType === resourceType && folderState.items.length > 0
      ? "folder"
      : "library";
  }

  listing(resourceType: ResourceType, options: NavigationCacheOptions): NavigationListing {
    const view = this.effectiveView(resourceType, options);
    const contextKey = contextOf(options);
    if (view === "tone3000") {
      return this.tone3000Listing(resourceType, this.tone3000State(contextKey));
    }
    if (view === "folder") {
      const items = this.host.folderStates.get(contextKey)?.items ?? [];
      const folderPath = (items[0]?.filePath ?? "").replace(/[\\/][^\\/]*$/, "");
      return {
        view,
        title: `Folder · ${fileLabel(folderPath) || folderPath}`,
        items: items.map((item, index) => ({
          key: `f:${index}:${item.filePath ?? item.resourceId ?? ""}`,
          label: fileLabel(item.filePath ?? "") || item.resourceId || "",
          resourceId: item.resourceId || undefined,
          filePath: item.filePath,
        })),
      };
    }
    const resources = uiState.resourceLibrary[resourceType] ?? [];
    const items = this.host.getLibraryState(resourceType, options)?.items ?? [];
    return {
      view,
      title: "Library",
      items: items.map((item, index) => {
        const resource = item.resourceId ? findResourceById(resources, item.resourceId) : undefined;
        return {
          key: `l:${index}:${item.resourceId ?? ""}`,
          label: resource?.name || item.resourceId || fileLabel(item.filePath ?? ""),
          resourceId: item.resourceId,
        };
      }),
    };
  }

  private tone3000Listing(resourceType: ResourceType, state: Tone3000NavigationState | undefined): NavigationListing {
    const items: NavigationListItem[] = [];
    for (const tone of state?.tones ?? []) {
      const toneId = String(tone.id);
      const toneTitle = tone.title?.trim() || tone.name?.trim() || toneId;
      const models = state?.modelsByToneId.get(toneId);
      if (!models) {
        items.push({ key: `t:${toneId}`, label: toneTitle, detail: "Show IRs", tone3000: { toneId } });
        continue;
      }
      for (const model of models) {
        const modelId = String(model.id);
        items.push({
          key: `t:${toneId}:${modelId}`,
          label: model.name?.trim() || toneTitle,
          detail: toneTitle,
          resourceId: this.host.navigator.findImportedTone3000ResourceId(resourceType, modelId) || undefined,
          tone3000: { toneId, modelId },
        });
      }
    }
    return { view: "tone3000", title: `Tone3000 · ${state?.tones.length ?? 0} tones`, items };
  }

  /**
   * Hands the list one slot walks to another, so a row dropped on IR B from IR A's list leaves
   * IR B stepping through that list next: its view, its folder or Tone3000 set, and the folder
   * its browser reopens in. A library list keeps the target's own filters.
   */
  adopt(resourceType: ResourceType, from: NavigationCacheOptions, to: NavigationCacheOptions): void {
    const fromKey = contextOf(from);
    const toKey = contextOf(to);
    if (fromKey === toKey) {
      return;
    }
    const view = this.effectiveView(resourceType, from);
    if (view === "folder") {
      const folderState = this.host.folderStates.get(fromKey);
      if (folderState) {
        this.host.folderStates.set(toKey, folderState);
      }
      copyFolderLocation(fromKey, toKey);
    } else if (view === "tone3000") {
      const toneState = this.tone3000State(fromKey);
      if (toneState) {
        this.host.navigator.tone3000NavigationStates.set(toKey, toneState);
      }
    }
    this.host.views.set(toKey, view);
  }

  /** Fetches a listed tone's models, so its rows can be drawn. */
  async loadTone(options: NavigationCacheOptions, toneId: string): Promise<void> {
    const state = this.tone3000State(contextOf(options));
    if (state) {
      await this.host.navigator.ensureListedToneModels(state, toneId);
    }
  }

  /** The library resource for a listed Tone3000 model, downloaded and imported if need be. */
  async resolveTone3000(options: NavigationCacheOptions, toneId: string, modelId: string): Promise<ResourceNavigationResult | null> {
    const state = this.tone3000State(contextOf(options));
    return state ? this.host.navigator.resolveListedModel(state, toneId, modelId) : null;
  }

  private tone3000State(contextKey: string): Tone3000NavigationState | undefined {
    return this.host.navigator.tone3000NavigationStates.get(contextKey);
  }
}
