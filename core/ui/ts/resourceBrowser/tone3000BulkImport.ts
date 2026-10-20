/**
 * The whole-tone actions in the resource browser's Tone3000 tab: which of a
 * tone's models are already in the library, Import All for the rest, and
 * Create Blend, which imports what is missing, then leaves the picker for the
 * blend editor.
 *
 * Models are imported the way the tab's own Select imports one (an IR filed
 * under the slot it was picked for), through `Tone3000Navigator`, so next/prev
 * and this share one record of what has been downloaded this session.
 */

import { BLEND_EDITOR_OPEN_EVENT, findBlendForToneGroup, type BlendEditorOpenDetail } from "../blendUtils.js";
import { postMessage } from "../bridge.js";
import { EffectGuids } from "../effectGuids.js";
import { Features, isFeatureEnabled } from "../featureFlags.js";
import { showNotification } from "../notifications.js";
import { getSignalPathPreset, uiState } from "../state.js";
import { isTone3000AuthReady } from "../tone3000.js";
import type { Tone3000Model, Tone3000Tone } from "../tone3000ApiTypes.js";
import { createTone3000BlendDefinition } from "../tone3000Shared.js";
import type { BlendDefinition } from "../types.js";
import { escapeHtml } from "../utils.js";
import { slotImportCategory } from "./slot.js";
import type { Tone3000Navigator } from "./tone3000Navigation.js";
import type { ResourceBrowserOptions, ResourceType } from "./types.js";

export interface Tone3000BulkImportHost {
  navigation: Tone3000Navigator;
  getOptions(): ResourceBrowserOptions | null;
  closeModal(): void;
  /** Redraw the tone list, so progress and imported badges show. */
  render(): void;
}

interface BulkImportProgress {
  toneId: string;
  completed: number;
  total: number;
}

export class Tone3000BulkImporter {
  /** The import running now; one at a time, as the downloads go one after another. */
  private progress: BulkImportProgress | null = null;

  constructor(private readonly host: Tone3000BulkImportHost) {}

  isModelImported(resourceType: ResourceType, model: Tone3000Model): boolean {
    return Boolean(this.host.navigation.findImportedTone3000ResourceId(resourceType, String(model.id)));
  }

  /**
   * The tone header's import badge. With the models fetched it counts them; before that it
   * counts what the library holds from this tone, as the list's own model count is not
   * narrowed by the architecture filter.
   */
  renderToneBadge(tone: Tone3000Tone, models: Tone3000Model[] | undefined, resourceType: ResourceType): string {
    if (models?.length) {
      const imported = models.filter((model) => this.isModelImported(resourceType, model)).length;
      if (!imported) {
        return "";
      }
      const label = imported === models.length ? "Imported" : `${imported}/${models.length} imported`;
      return `<span class="resource-browser-tone-imported">${escapeHtml(label)}</span>`;
    }

    const toneId = String(tone.id);
    const modelIds = new Set((uiState.resourceLibrary[resourceType] ?? [])
      .filter((resource) => !resource.fileMissing && resource.metadata?.toneId === toneId)
      .map((resource) => resource.metadata?.modelId ?? resource.id));
    return modelIds.size
      ? `<span class="resource-browser-tone-imported">${modelIds.size} imported</span>`
      : "";
  }

  /** The bar over a tone's model list: how many are imported, Import All and Create Blend. */
  renderToolbar(tone: Tone3000Tone, models: Tone3000Model[], resourceType: ResourceType): string {
    if (models.length < 2) {
      return "";
    }

    const toneId = escapeHtml(String(tone.id));
    const imported = models.filter((model) => this.isModelImported(resourceType, model)).length;
    const remaining = models.length - imported;
    const busy = this.progress !== null;
    const isThisTone = this.progress?.toneId === String(tone.id);

    const status = isThisTone && this.progress
      ? `Importing ${Math.min(this.progress.completed + 1, this.progress.total)} of ${this.progress.total}...`
      : remaining === 0
        ? `All ${models.length} models imported`
        : `${imported} of ${models.length} models imported`;

    const importButton = remaining > 0
      ? `<button class="resource-browser-tone-import-all" type="button" data-tone-id="${toneId}"
                 title="Import the ${remaining} model${remaining === 1 ? "" : "s"} not in your library yet" ${busy ? "disabled" : ""}>
           ${imported ? "Import Remaining" : "Import All"}
         </button>`
      : "";
    const blendButton = resourceType === "nam" && isFeatureEnabled(Features.BlendTools)
      ? `<button class="resource-browser-tone-create-blend" type="button" data-tone-id="${toneId}"
                 title="${remaining ? "Import every model, then open" : "Open"} a blend of them in the blend editor" ${busy ? "disabled" : ""}>
           Create Blend
         </button>`
      : "";

    return `
      <div class="resource-browser-tone-models-toolbar">
        <span class="resource-browser-tone-models-status">${escapeHtml(status)}</span>
        <div class="resource-browser-tone-models-toolbar-actions">${importButton}${blendButton}</div>
      </div>
    `;
  }

  /** Imports every model of the tone not in the library yet; then, if asked, opens a blend of them all. */
  async importAll(tone: Tone3000Tone, models: Tone3000Model[], createBlend: boolean): Promise<void> {
    const options = this.host.getOptions();
    if (!options || this.progress || !models.length) {
      return;
    }
    if (!isTone3000AuthReady()) {
      showNotification("Import failed", "No Tone3000 session");
      return;
    }

    // The picker this was asked from: closed, or opened for another slot, and the rest stops.
    const isStale = (): boolean => this.host.getOptions() !== options;
    const resourceType = options.resourceType;
    const importCategory = resourceType === "ir" ? slotImportCategory(options, "") : undefined;
    const toneTitle = tone.title ?? tone.name ?? "Tone3000";

    const resourceIds = new Map<string, string>();
    const missing: Tone3000Model[] = [];
    for (const model of models) {
      const existing = this.host.navigation.findImportedTone3000ResourceId(resourceType, String(model.id));
      if (existing) {
        resourceIds.set(String(model.id), existing);
      } else {
        missing.push(model);
      }
    }

    let failed = 0;
    let lastError = "";
    if (missing.length) {
      this.progress = { toneId: String(tone.id), completed: 0, total: missing.length };
      this.host.render();
      try {
        for (const model of missing) {
          if (isStale()) {
            return;
          }
          try {
            const resourceId = await this.host.navigation.downloadAndImportTone3000Model(tone, model, resourceType, importCategory);
            resourceIds.set(String(model.id), resourceId);
          } catch (error) {
            failed += 1;
            lastError = error instanceof Error ? error.message : String(error);
          }
          this.progress.completed += 1;
          if (!isStale()) {
            this.host.render();
          }
        }
      } finally {
        this.progress = null;
        if (!isStale()) {
          this.host.render();
        }
      }

      if (isStale()) {
        return;
      }
      if (failed) {
        showNotification(`Imported ${missing.length - failed} of ${missing.length}`, `${toneTitle}: ${lastError}`);
      } else {
        showNotification(`Imported ${missing.length} model${missing.length === 1 ? "" : "s"}`, toneTitle);
      }
    }

    if (createBlend) {
      this.openBlend(tone, models, resourceIds, options);
    }
  }

  /**
   * Saves a blend of the tone's models, or finds the one made before, and opens it in the blend
   * editor. Started from a NAM node's picker, the editor is told which node, so its Save can offer
   * the blend in that node's place.
   */
  private openBlend(
    tone: Tone3000Tone,
    models: Tone3000Model[],
    resourceIds: Map<string, string>,
    options: ResourceBrowserOptions,
  ): void {
    const modelNames: Record<string, string> = {};
    const blendIds: string[] = [];
    for (const model of models) {
      const resourceId = resourceIds.get(String(model.id));
      if (resourceId && !blendIds.includes(resourceId)) {
        blendIds.push(resourceId);
        modelNames[resourceId] = model.name ?? "";
      }
    }
    if (blendIds.length < 2) {
      showNotification("Blend creation failed", "A blend needs at least two imported models.");
      return;
    }

    const toneGroupId = String(tone.id);
    let blend: BlendDefinition | undefined = findBlendForToneGroup(uiState.blendLibrary ?? [], toneGroupId, blendIds);
    if (!blend) {
      blend = {
        ...createTone3000BlendDefinition(tone, blendIds, modelNames),
        toneGroupId,
        toneGroupTitle: tone.title ?? tone.name ?? "",
      };
      postMessage({ type: "saveBlendDefinition", blend });
    }

    const preset = getSignalPathPreset();
    const node = options.nodeId ? preset?.graph?.nodes.find((candidate) => candidate.id === options.nodeId) : undefined;
    const detail: BlendEditorOpenDetail = {
      blend,
      sourceNode: preset && node && node.type !== EffectGuids.kAmpNamBlend
        ? { presetId: preset.id, nodeId: node.id }
        : undefined,
    };

    this.host.closeModal();
    document.dispatchEvent(new CustomEvent(BLEND_EDITOR_OPEN_EVENT, { detail }));
  }
}
