/**
 * The blend editor: one screen to name a blend, choose its models and where each sits, and
 * try it. The data lives in `blendEditor/editState.ts`, the table markup in
 * `blendEditor/table.ts`, and adding models in `blendEditor/addModels.ts`; this file owns the
 * modal, its events and the Try strip.
 *
 * Opened from a node (not a factory blend), every edit is saved shortly after it is made, so
 * it is heard on that node as it is made; Cancel puts the saved copy back.
 */

import { requestResourceData } from "./archiveUtils.js";
import { describeBlendMix, getBlendParamSpec, normalizeBlendValue, denormalizeBlendValue } from "./blendUtils.js";
import { postMessage } from "./bridge.js";
import { GenericKnob } from "./controls.js";
import { showConfirm } from "./dialogs.js";
import { showNotification } from "./notifications.js";
import { sendSignalPathNodeParamUpdate } from "./signalPath/commands.js";
import { requestNodeParamsRefresh } from "./signalPath/render.js";
import { getActivePresetForRender } from "./state.js";
import type { BlendDefinition, BlendLibrary, BlendMode, GraphNode, ResourceLibrary } from "./types.js";
import { escapeHtml } from "./utils.js";
import { BlendAddModelsDialog } from "./blendEditor/addModels.js";
import {
  addModels,
  addParam,
  blendWarnings,
  clampTrim,
  definitionFromEditState,
  editStateFromDefinition,
  fillValuesFromNames,
  groupByVoicing,
  libraryModel,
  matchLevels,
  readNamLoudness,
  removeParam,
  setAxis,
  sortRows,
  type BlendEditState,
} from "./blendEditor/editState.js";
import { renderBlendTable, renderBlendWarnings } from "./blendEditor/table.js";

/** The Try knob for the Blend sweep, shown on the Sweep axis. */
const BLEND_SWEEP_PARAM = "blend";
/** How long after the last edit it is saved to be heard on the node. */
const AUDITION_DELAY_MS = 400;

type BlendEditorDependencies = {
  getBlendLibrary: () => BlendLibrary;
  getResourceLibrary: () => ResourceLibrary;
  /** Opens the resource picker on Tone3000: a pick, or an Import All, comes back as models to add. */
  pickModelsFromTone3000?: (handlers: { onSelect: (id: string) => void; onImportMany: (ids: string[]) => void }) => void;
  canPickFromTone3000?: () => boolean;
};

function newBlendId(): string {
  return typeof crypto !== "undefined" && "randomUUID" in crypto
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function decodeBase64Text(data: string): string {
  const binary = atob(data);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return new TextDecoder().decode(bytes);
}

export class BlendEditorModal {
  private readonly deps: BlendEditorDependencies;
  private initialized = false;

  private modal = document.getElementById("blend-editor-modal") as HTMLElement | null;
  private title = document.getElementById("blend-editor-title");
  private nameInput = document.getElementById("blend-editor-name-input") as HTMLInputElement | null;
  private table = document.getElementById("blend-editor-table");
  private warnings = document.getElementById("blend-editor-warnings");
  private fillBtn = document.getElementById("blend-fill-names-btn") as HTMLButtonElement | null;
  private matchBtn = document.getElementById("blend-match-levels-btn") as HTMLButtonElement | null;
  private testControls = document.getElementById("blend-test-controls");
  private matchLine = document.getElementById("blend-match-line");
  private testNote = document.getElementById("blend-test-note");

  private readonly addDialog: BlendAddModelsDialog;
  private state: BlendEditState | null = null;
  /** The saved definition when the editor opened, put back by Cancel after a live audition. */
  private original: BlendDefinition | null = null;
  private liveAudition = false;
  private auditioned = false;
  private auditionTimer: ReturnType<typeof setTimeout> | null = null;
  private testParams: Record<string, number> = {};
  /** Given by openWithDefinition's caller; called with the blend when Save closes the editor. */
  private onSaved: ((blend: BlendDefinition) => void) | null = null;

  constructor(deps: BlendEditorDependencies) {
    this.deps = deps;
    this.addDialog = new BlendAddModelsDialog({
      getResourceLibrary: () => this.deps.getResourceLibrary(),
      openTone3000: deps.pickModelsFromTone3000 ? () => this.pickFromTone3000() : undefined,
      canOpenTone3000: deps.canPickFromTone3000,
    });
  }

  initialize(): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;

    document.getElementById("blend-editor-modal-close")?.addEventListener("click", () => this.close());
    document.getElementById("blend-editor-cancel")?.addEventListener("click", () => this.close());
    document.getElementById("blend-editor-save")?.addEventListener("click", () => this.save());
    document.getElementById("blend-add-models-btn")?.addEventListener("click", () => this.openAddModels());
    this.fillBtn?.addEventListener("click", () => this.edit(() => {
      fillValuesFromNames(this.state!, this.library());
      sortRows(this.state!);
    }));
    this.matchBtn?.addEventListener("click", () => void this.matchLevels());
    this.nameInput?.addEventListener("input", () => {
      if (this.state && this.nameInput) {
        this.state.name = this.nameInput.value;
      }
    });

    this.modal?.querySelectorAll<HTMLButtonElement>("[data-blend-mode]").forEach((button) => {
      button.addEventListener("click", () => this.edit(() => {
        this.state!.mode = button.dataset.blendMode === "snap" ? "snap" : "interpolate";
      }));
    });
    this.modal?.querySelectorAll<HTMLButtonElement>("[data-blend-axis]").forEach((button) => {
      button.addEventListener("click", () => this.edit(() => {
        setAxis(this.state!, button.dataset.blendAxis === "sweep" ? "sweep" : "params", this.library());
      }));
    });

    this.bindTable();
    this.warnings?.addEventListener("click", (event) => {
      if ((event.target as HTMLElement | null)?.closest("[data-action='split-voicings']")) {
        void this.splitByVoicing();
      }
    });
    this.modal?.addEventListener("mousedown", (event) => {
      if (event.target === this.modal) {
        this.close();
      }
    });
  }

  /** Edit the blend a node plays; its knobs are the Try knobs, so the blend is heard. */
  open(node: GraphNode): void {
    const blendId = node.config?.blendId;
    const blend = blendId ? this.deps.getBlendLibrary().find((entry) => entry.id === blendId) : undefined;
    if (!blend) {
      return;
    }
    this.begin(blend, node.id);
  }

  /** Edit a definition: a saved blend, or a new one that is saved only by Save. */
  openWithDefinition(blend: BlendDefinition, onSaved?: (blend: BlendDefinition) => void): void {
    this.begin(blend, null, onSaved);
  }

  private begin(blend: BlendDefinition, nodeId: string | null, onSaved?: (blend: BlendDefinition) => void): void {
    if (!this.modal) {
      return;
    }
    this.initialize();

    const saved = this.deps.getBlendLibrary().find((entry) => entry.id === blend.id);
    const copy = JSON.parse(JSON.stringify(blend)) as BlendDefinition;
    this.original = saved ? (JSON.parse(JSON.stringify(saved)) as BlendDefinition) : null;
    this.state = editStateFromDefinition(copy, this.library());
    if (!copy.modelMappings?.length && copy.models?.length) {
      // A blend made from a list of models: the editor places them, as Add Models does.
      this.state.rows = [];
      addModels(this.state, copy.models, this.library());
    }
    // Knob order: the engine sorts the models the same way, so this changes nothing heard.
    sortRows(this.state);
    this.onSaved = onSaved ?? null;
    this.auditioned = false;
    this.testParams = {};
    // A factory blend saved is a copy of the user's own, so it is heard only on Save.
    this.liveAudition = Boolean(nodeId) && Boolean(saved) && !saved?.factory;

    if (nodeId) {
      this.modal.dataset.nodeId = nodeId;
    } else {
      delete this.modal.dataset.nodeId;
    }
    if (this.title) {
      this.title.textContent = saved ? "Edit Blend" : "New Blend";
    }
    if (this.nameInput) {
      this.nameInput.value = this.state.name;
    }
    if (this.testNote) {
      this.testNote.textContent = this.liveAudition
        ? "Changes play on the node as you make them. Cancel puts the saved blend back."
        : nodeId
          ? "Save to hear changes on the node."
          : "The Try knobs show which models play. Open the blend from a node to hear it.";
    }

    this.render();
    this.modal.style.display = "flex";
  }

  private library(): ResourceLibrary {
    return this.deps.getResourceLibrary();
  }

  private close(): void {
    if (!this.modal) {
      return;
    }
    if (this.auditionTimer) {
      clearTimeout(this.auditionTimer);
      this.auditionTimer = null;
    }
    // Live edits were saved as they were made; leaving without Save puts the saved copy back.
    if (this.auditioned && this.original) {
      postMessage({ type: "saveBlendDefinition", blend: this.original });
    }
    if (this.modal.dataset.nodeId) {
      // The Try knobs move the node's knobs; the params panel shows them where they are now.
      requestNodeParamsRefresh();
    }
    this.addDialog.close();
    this.modal.style.display = "none";
    delete this.modal.dataset.nodeId;
    this.state = null;
    this.original = null;
    this.auditioned = false;
    this.onSaved = null;
  }

  private getActiveNode(): GraphNode | null {
    const nodeId = this.modal?.dataset.nodeId ?? "";
    if (!nodeId) {
      return null;
    }
    // The draft the params panel edits, not the library's saved copy: a Try knob writes this node.
    const preset = getActivePresetForRender();
    return preset?.graph?.nodes.find((entry) => entry.id === nodeId) ?? null;
  }

  /** Applies a change to the blend, redraws, and saves it for the node to play if live. */
  private edit(change: () => void, redraw = true): void {
    if (!this.state) {
      return;
    }
    change();
    if (redraw) {
      this.render();
    } else {
      this.renderStatus();
    }
    this.scheduleAudition();
  }

  private scheduleAudition(): void {
    if (!this.liveAudition || !this.state) {
      return;
    }
    if (this.auditionTimer) {
      clearTimeout(this.auditionTimer);
    }
    this.auditionTimer = setTimeout(() => {
      this.auditionTimer = null;
      if (this.state?.rows.length) {
        postMessage({ type: "saveBlendDefinition", blend: this.currentDefinition() });
        this.auditioned = true;
      }
    }, AUDITION_DELAY_MS);
  }

  private currentDefinition(): BlendDefinition {
    const definition = definitionFromEditState(this.state!, this.library());
    if (!definition.name) {
      definition.name = this.original?.name || "Blend";
    }
    return definition;
  }

  private render(): void {
    if (!this.state) {
      return;
    }
    this.modal?.querySelectorAll<HTMLButtonElement>("[data-blend-mode]").forEach((button) => {
      button.classList.toggle("is-active", button.dataset.blendMode === this.state!.mode);
      button.setAttribute("aria-pressed", String(button.dataset.blendMode === this.state!.mode));
    });
    this.modal?.querySelectorAll<HTMLButtonElement>("[data-blend-axis]").forEach((button) => {
      button.classList.toggle("is-active", button.dataset.blendAxis === this.state!.axis);
      button.setAttribute("aria-pressed", String(button.dataset.blendAxis === this.state!.axis));
    });
    if (this.table) {
      this.table.innerHTML = renderBlendTable(this.state, this.library());
    }
    this.renderTryControls();
    this.renderStatus();
  }

  /** What changes with every value typed: warnings, the tools that apply, and the playing line. */
  private renderStatus(): void {
    if (!this.state) {
      return;
    }
    if (this.warnings) {
      this.warnings.innerHTML = renderBlendWarnings(blendWarnings(this.state, this.library()));
    }
    if (this.fillBtn) {
      this.fillBtn.disabled = this.state.axis !== "params" || !this.state.rows.length;
    }
    if (this.matchBtn) {
      this.matchBtn.disabled = this.state.rows.length < 2;
    }
    this.updatePlaying();
  }

  private bindTable(): void {
    this.table?.addEventListener("input", (event) => {
      const input = event.target as HTMLInputElement | null;
      const row = this.state?.rows[Number(input?.dataset.index)];
      if (!input || !row) {
        return;
      }
      if (input.classList.contains("blend-cell-value")) {
        const paramId = input.dataset.paramId ?? "";
        const value = Number.parseFloat(input.value);
        if (Number.isFinite(value)) {
          row.values[paramId] = normalizeBlendValue(value, getBlendParamSpec(paramId));
        } else {
          delete row.values[paramId];
        }
        this.renderStatus();
      } else if (input.classList.contains("blend-cell-trim")) {
        row.levelTrimDb = clampTrim(Number.parseFloat(input.value) || 0);
      }
    });
    // Heard on commit (Enter or leaving the field), not on every keystroke: each save rebuilds the node.
    this.table?.addEventListener("change", (event) => {
      const target = event.target as HTMLElement | null;
      if (target?.matches(".blend-cell-value, .blend-cell-trim")) {
        this.edit(() => undefined, false);
      } else if (target?.matches(".blend-param-add")) {
        const paramId = (target as HTMLSelectElement).value;
        this.edit(() => addParam(this.state!, paramId, this.library()));
      }
    });
    this.table?.addEventListener("click", (event) => {
      const button = (event.target as HTMLElement | null)?.closest("button");
      if (!button || !this.state) {
        return;
      }
      const index = Number(button.dataset.index);
      if (button.classList.contains("blend-row-remove")) {
        this.edit(() => this.state!.rows.splice(index, 1));
      } else if (button.classList.contains("blend-row-move")) {
        const target = index + Number(button.dataset.move);
        this.edit(() => {
          const rows = this.state!.rows;
          if (target >= 0 && target < rows.length) {
            [rows[index], rows[target]] = [rows[target], rows[index]];
          }
        });
      } else if (button.classList.contains("blend-param-remove")) {
        this.edit(() => removeParam(this.state!, button.dataset.paramId ?? ""));
      }
    });
  }

  private openAddModels(): void {
    if (!this.state) {
      return;
    }
    this.addDialog.open(this.state.rows.map((row) => row.id), (ids) => this.addToBlend(ids));
  }

  private addToBlend(ids: string[]): void {
    if (!this.state) {
      return;
    }
    let added = 0;
    this.edit(() => {
      added = addModels(this.state!, ids, this.library());
    });
    if (added) {
      showNotification(`Added ${added} model${added === 1 ? "" : "s"}`, this.state.name);
    }
  }

  private pickFromTone3000(): void {
    this.deps.pickModelsFromTone3000?.({
      onSelect: (id) => this.addToBlend([id]),
      onImportMany: (ids) => this.addToBlend(ids),
    });
  }

  /** Trims from the loudness each model file states, to their median. */
  private async matchLevels(): Promise<void> {
    const state = this.state;
    if (!state || state.rows.length < 2) {
      return;
    }
    if (this.matchBtn) {
      this.matchBtn.disabled = true;
      this.matchBtn.textContent = "Matching…";
    }
    const loudness = new Map<string, number>();
    for (const row of state.rows) {
      const data = libraryModel(this.library(), row.id) ? await requestResourceData("nam", row.id) : undefined;
      const value = data ? readNamLoudness(decodeBase64Text(data)) : null;
      if (value !== null) {
        loudness.set(row.id, value);
      }
    }
    if (this.matchBtn) {
      this.matchBtn.textContent = "Match Levels";
    }
    if (this.state !== state) {
      return;
    }
    let matched = 0;
    this.edit(() => {
      matched = matchLevels(state, loudness);
    });
    const unknown = state.rows.length - loudness.size;
    showNotification(
      matched ? `Matched ${matched} model levels` : "Levels not matched",
      matched
        ? unknown ? `${unknown} model${unknown === 1 ? " has" : "s have"} no loudness in its file and kept its trim.` : state.name
        : "Fewer than two of these model files state their loudness. Set the Level trims by ear.",
    );
  }

  /** Saves one blend per voicing; the editor stays on the first, which keeps this blend's id. */
  private async splitByVoicing(): Promise<void> {
    const state = this.state;
    if (!state) {
      return;
    }
    const voicings = groupByVoicing(state, this.library());
    if (voicings.length < 2) {
      return;
    }
    const name = state.name.trim() || "Blend";
    const confirmed = await showConfirm(
      `Split "${name}" into ${voicings.length} blends, one for each voicing (${voicings.map((voicing) => voicing.label).join(", ")})? Each is saved now.`,
      "Split by Voicing",
      { ok: "Split", cancel: "Keep One Blend" },
    );
    if (!confirmed || this.state !== state) {
      return;
    }
    const definitions = voicings.map((voicing, index) => definitionFromEditState({
      ...state,
      id: index === 0 ? state.id : newBlendId(),
      name: `${name} – ${voicing.label}`,
      rows: voicing.rows,
    }, this.library()));
    definitions.forEach((blend) => postMessage({ type: "saveBlendDefinition", blend }));
    showNotification(`Saved ${definitions.length} blends`, definitions.map((blend) => blend.name).join(", "));

    const [first] = definitions;
    this.original = JSON.parse(JSON.stringify(first)) as BlendDefinition;
    this.auditioned = false;
    this.state = editStateFromDefinition(first, this.library());
    if (this.nameInput) {
      this.nameInput.value = this.state.name;
    }
    if (this.title) {
      this.title.textContent = "Edit Blend";
    }
    this.render();
  }

  private save(): void {
    if (!this.state || !this.modal) {
      return;
    }
    if (!this.state.name.trim()) {
      this.nameInput?.focus();
      showNotification("Name the blend", "A blend needs a name to be saved.");
      return;
    }
    if (!this.state.rows.length) {
      showNotification("Add models", "A blend needs models to be saved.");
      return;
    }
    if (this.auditionTimer) {
      clearTimeout(this.auditionTimer);
      this.auditionTimer = null;
    }
    const blend = this.currentDefinition();
    postMessage({ type: "saveBlendDefinition", blend });
    const onSaved = this.onSaved;
    // Saved: nothing for Cancel to put back.
    this.auditioned = false;
    this.close();
    onSaved?.(blend);
  }

  // ── Try strip ─────────────────────────────────────────────────────────────

  private tryParamIds(): string[] {
    const state = this.state;
    if (!state || state.axis === "sweep") {
      return [BLEND_SWEEP_PARAM];
    }
    const captured = state.params.filter((paramId) => state.rows.some((row) => typeof row.values[paramId] === "number"));
    return captured.length ? captured : [BLEND_SWEEP_PARAM];
  }

  /** Where a Try knob starts: the node's own value, else the middle of the captured values. */
  private startingValue(paramId: string): number {
    const nodeValue = this.getActiveNode()?.params?.[paramId];
    if (typeof nodeValue === "number") {
      return nodeValue;
    }
    if (paramId === BLEND_SWEEP_PARAM) {
      return 0;
    }
    const values = (this.state?.rows ?? [])
      .map((row) => row.values[paramId])
      .filter((value): value is number => typeof value === "number")
      .sort((a, b) => a - b);
    if (!values.length) {
      return 0.5;
    }
    const mid = Math.floor(values.length / 2);
    return values.length % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
  }

  private renderTryControls(): void {
    if (!this.testControls || !this.state) {
      return;
    }
    const paramIds = this.tryParamIds();
    const next: Record<string, number> = {};
    paramIds.forEach((paramId) => {
      next[paramId] = typeof this.testParams[paramId] === "number" ? this.testParams[paramId] : this.startingValue(paramId);
    });
    this.testParams = next;

    this.testControls.innerHTML = paramIds
      .map((paramId) => {
        const spec = paramId === BLEND_SWEEP_PARAM ? null : getBlendParamSpec(paramId);
        const display = spec ? denormalizeBlendValue(this.testParams[paramId], spec) : this.testParams[paramId];
        return `
          <div class="knob-control">
            <span class="knob-label">${escapeHtml(spec?.label ?? "Blend")}</span>
            <div class="knob blend-test-knob" data-param-id="${escapeHtml(paramId)}" data-value="${display.toFixed(2)}"
                 data-min="${spec?.min ?? 0}" data-max="${spec?.max ?? 1}">
              <div class="knob-indicator"></div>
            </div>
            <span class="knob-value">${display.toFixed(1)}</span>
          </div>`;
      })
      .join("");

    this.testControls.querySelectorAll<HTMLElement>(".blend-test-knob").forEach((knob) => {
      const paramId = knob.dataset.paramId ?? "";
      const spec = paramId === BLEND_SWEEP_PARAM ? null : getBlendParamSpec(paramId);
      const min = spec?.min ?? 0;
      const max = spec?.max ?? 1;
      new GenericKnob({
        knobElement: knob,
        paramId: `blend_test_${paramId}`,
        minValue: min,
        maxValue: max,
        defaultValue: spec ? denormalizeBlendValue(this.testParams[paramId], spec) : this.testParams[paramId],
        displayFormat: (value) => value.toFixed(1),
        valueDisplay: knob.parentElement?.querySelector(".knob-value") as HTMLElement | null,
        labelElement: knob.parentElement?.querySelector(".knob-label") as HTMLElement | null,
        stepValue: spec ? 0.1 : 0.01,
        sensitivity: (max - min) / 200,
        sendParameter: false,
        onValueChange: (value) => {
          const normalized = spec ? normalizeBlendValue(value, spec) : value;
          this.testParams[paramId] = normalized;
          this.updatePlaying();
          // Opened from a node, the Try knobs are its knobs, so the blend is heard.
          const node = this.getActiveNode();
          if (node && node.params[paramId] !== normalized) {
            node.params[paramId] = normalized;
            sendSignalPathNodeParamUpdate(node.id, paramId, normalized);
          }
        },
      });
    });
  }

  /** The model playing at the Try knobs, by the engine's own rule (blendUtils.selectBlendMix). */
  private updatePlaying(): void {
    if (!this.matchLine || !this.state) {
      return;
    }
    const definition = definitionFromEditState(this.state, this.library());
    const target: Record<string, number> = {};
    Object.entries(this.testParams).forEach(([paramId, value]) => {
      if (paramId !== BLEND_SWEEP_PARAM) {
        target[paramId] = value;
      }
    });
    const override = this.getActiveNode()?.config?.blendModeOverride;
    const mode: BlendMode = override === "snap" || override === "interpolate" ? override : this.state.mode;
    const summary = describeBlendMix(
      definition.modelMappings ?? [],
      target,
      this.testParams[BLEND_SWEEP_PARAM] ?? 0,
      mode,
      (modelId) => libraryModel(this.library(), modelId)?.name ?? modelId,
    );
    const line = definition.modelMappings?.length ? summary.details || `Playing: ${summary.name}` : "";
    this.matchLine.textContent = line;
    this.matchLine.title = line;
  }
}
