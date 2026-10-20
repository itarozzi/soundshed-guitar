/**
 * Blend effects handling for the signal path panel.
 *
 * Contains blend state derivation, indicator rendering, and the blend-editor modal wiring
 * extracted from signalPath.ts. What a blend plays is decided in blendUtils.selectBlendMix,
 * which mirrors the engine.
 */
import { uiState } from "./state.js";
import { EffectGuids } from "./effectGuids.js";
import type {
  BlendModelMapping,
  BlendDefinition,
  BlendMode,
  BlendLibrary,
  GraphNode,
  LibraryResource,
} from "./types.js";
import {
  BLEND_EDITOR_OPEN_EVENT,
  BLEND_SAVED_FROM_NODE_EVENT,
  buildBlendModelMappingsFromIds,
  buildParameterMapFromLegacy,
  denormalizeBlendValue,
  describeBlendMix,
  getBlendParamSpec,
  mappedBlendParamIds,
  type BlendEditorOpenDetail,
  type BlendKnobBinding,
  type BlendMatchSummary,
  type BlendParamSpec,
} from "./blendUtils.js";
import { BlendEditorModal } from "./blendEditor.js";
import { resourceBrowserModal } from "./resourceBrowser.js";
import { escapeHtml, findResourceById } from "./utils.js";
import { renderIcon } from "./iconAssets.js";
import { Features, isFeatureEnabled } from "./featureFlags.js";

export {
  BLEND_PARAM_SPECS,
  buildParameterMapFromLegacy,
  denormalizeBlendValue,
  normalizeBlendValue,
  type BlendMatchSummary,
  type BlendParamSpec,
} from "./blendUtils.js";

// ---------------------------------------------------------------------------
// Mapped-point helpers
// ---------------------------------------------------------------------------

export const BLEND_MAPPING_EPS = 1e-4;

type BlendMappedPoint = {
  normalized: number;
  display: number;
  isSelectable: boolean;
  isSelected: boolean;
};

function hasCloseValue(values: number[], value: number, eps = BLEND_MAPPING_EPS): boolean {
  return values.some((existing) => Math.abs(existing - value) <= eps);
}

function addUniqueValue(values: number[], value: number, eps = BLEND_MAPPING_EPS): void {
  if (!hasCloseValue(values, value, eps)) {
    values.push(value);
  }
}

function buildBlendMappedPointsForParam(
  paramId: string,
  blendState: BlendState,
  target: Record<string, number>,
): BlendMappedPoint[] {
  if (!paramId) {
    return [];
  }

  const normalizedValues: number[] = [];
  const selectableValues: number[] = [];
  const spec = getBlendParamSpec(paramId);

  blendState.mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue === "number") {
      addUniqueValue(normalizedValues, mappedValue);
    }
  });

  blendState.mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const mappedValue = params[paramId];
    if (typeof mappedValue !== "number") {
      return;
    }
    let matches = true;
    blendState.paramIds.forEach((activeParamId) => {
      if (activeParamId === paramId) {
        return;
      }
      const targetValue = target[activeParamId];
      const otherValue = params[activeParamId];
      if (typeof targetValue !== "number" || typeof otherValue !== "number") {
        matches = false;
        return;
      }
      if (Math.abs(otherValue - targetValue) > BLEND_MAPPING_EPS) {
        matches = false;
      }
    });
    if (matches) {
      addUniqueValue(selectableValues, mappedValue);
    }
  });

  const targetValue = target[paramId];
  return normalizedValues
    .slice()
    .sort((a, b) => a - b)
    .map((value) => ({
      normalized: value,
      display: denormalizeBlendValue(value, spec),
      isSelectable: hasCloseValue(selectableValues, value),
      isSelected: typeof targetValue === "number" && Math.abs(targetValue - value) <= BLEND_MAPPING_EPS,
    }));
}

function renderMappedPointElements(
  knob: HTMLElement,
  points: BlendMappedPoint[],
  min: number,
  max: number,
): void {
  let container = knob.querySelector(".knob-mapped-points") as HTMLElement | null;
  if (!container) {
    container = document.createElement("div");
    container.className = "knob-mapped-points";
    knob.prepend(container);
  }

  container.innerHTML = "";
  const range = max - min;
  const safeRange = range !== 0 ? range : 1;

  points.forEach((point) => {
    const angle = ((point.display - min) / safeRange) * 270 - 135;
    const el = document.createElement("span");
    el.className = "knob-mapped-point";
    if (point.isSelectable) {
      el.classList.add("is-selectable");
    }
    if (point.isSelected) {
      el.classList.add("is-selected");
    }
    el.style.setProperty("--mapped-angle", `${angle}deg`);
    container.appendChild(el);
  });

  knob.classList.toggle("has-mapped-points", points.length > 0);
}

// ---------------------------------------------------------------------------
// Blend state
// ---------------------------------------------------------------------------

export type BlendState = {
  blend: BlendDefinition | undefined;
  /** The node names a blend the library does not have: it plays dry. */
  missing: boolean;
  /** The mode in effect: the node's override, else the definition's. */
  blendMode: BlendMode;
  definitionBlendMode: BlendMode;
  /** The node's own choice of mode, or "" to follow the definition. */
  blendModeOverride: BlendMode | "";
  mappings: BlendModelMapping[];
  /** Parameters some model was captured at, each shown as a knob. Empty: Blend sweeps. */
  paramIds: string[];
};

export type BlendParamRange = {
  min: number;
  max: number;
  defaultValue: number;
  spec: BlendParamSpec | null;
};

export function computeBlendParamRange(
  paramId: string,
  mappings: BlendModelMapping[],
  fallbackValue: number | undefined,
): BlendParamRange {
  const spec = getBlendParamSpec(paramId);
  const values: number[] = [];

  mappings.forEach((mapping) => {
    const params = buildParameterMapFromLegacy(mapping);
    const raw = params[paramId];
    if (typeof raw === "number") {
      values.push(denormalizeBlendValue(raw, spec));
    }
  });

  const fallbackDisplay = typeof fallbackValue === "number"
    ? denormalizeBlendValue(fallbackValue, spec)
    : (spec ? spec.min : 0);

  if (!values.length) {
    return {
      min: spec ? spec.min : -1,
      max: spec ? spec.max : 1,
      defaultValue: fallbackDisplay,
      spec,
    };
  }

  const sorted = values.slice().sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  const median = sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
  const min = sorted[0];
  const max = sorted[sorted.length - 1];
  const defaultValue = typeof fallbackValue === "number" ? fallbackDisplay : median;

  return {
    min: min === max ? min - 0.5 : min,
    max: min === max ? max + 0.5 : max,
    defaultValue,
    spec,
  };
}

function asBlendMode(value: unknown): BlendMode | "" {
  return value === "snap" || value === "interpolate" ? value : "";
}

export function getBlendState(node: GraphNode): BlendState | null {
  if (node.type !== EffectGuids.kAmpNamBlend) {
    return null;
  }

  const blendId = node.config?.blendId;
  if (!blendId) {
    return null;
  }

  const blend = uiState.blendLibrary?.find((entry) => entry.id === blendId);
  const mappings = blend?.modelMappings?.length
    ? blend.modelMappings
    : buildBlendModelMappingsFromIds(blend?.models ?? [], uiState.resourceLibrary);
  const definitionBlendMode = asBlendMode(blend?.blendMode) || "interpolate";
  const blendModeOverride = asBlendMode(node.config?.blendModeOverride);

  return {
    blend,
    missing: !blend,
    blendMode: blendModeOverride || definitionBlendMode,
    definitionBlendMode,
    blendModeOverride,
    mappings,
    paramIds: mappedBlendParamIds(blend, mappings),
  };
}

/** How a mapped knob for `paramId` reads and writes its value. */
export function getBlendKnobBinding(paramId: string, blendState: BlendState): BlendKnobBinding {
  const spec = getBlendParamSpec(paramId);
  return {
    specMin: spec?.min ?? 0,
    specMax: spec?.max ?? 10,
    blendMode: blendState.blendMode,
  };
}

// ---------------------------------------------------------------------------
// Blend param indicator rendering
// ---------------------------------------------------------------------------

/** The median of the values the models were captured at: where an unset knob is drawn,
 * and where the engine starts it (BlendSupport.cpp). */
function capturedMedian(paramId: string, mappings: BlendModelMapping[]): number | undefined {
  const values = mappings
    .map((mapping) => buildParameterMapFromLegacy(mapping)[paramId])
    .filter((value): value is number => typeof value === "number")
    .sort((a, b) => a - b);
  if (!values.length) {
    return undefined;
  }
  const mid = Math.floor(values.length / 2);
  return values.length % 2 === 0 ? (values[mid - 1] + values[mid]) / 2 : values[mid];
}

function readBlendTarget(node: GraphNode, blendState: BlendState): Record<string, number> {
  const target: Record<string, number> = {};
  blendState.paramIds.forEach((paramId) => {
    const value = node.params[paramId];
    const resolved = typeof value === "number" ? value : capturedMedian(paramId, blendState.mappings);
    if (typeof resolved === "number") {
      target[paramId] = resolved;
    }
  });
  return target;
}

export function updateBlendParamIndicators(
  panel: HTMLElement | null,
  node: GraphNode,
  blendState: BlendState,
): void {
  if (!panel) {
    return;
  }

  const target = readBlendTarget(node, blendState);
  const knobs = panel.querySelectorAll('.node-param-knob[data-blend-param="true"]');
  knobs.forEach((knobElement) => {
    const knob = knobElement as HTMLElement;
    const paramId = knob.dataset.paramKey ?? "";
    const min = knob.dataset.min ? parseFloat(knob.dataset.min) : 0;
    const max = knob.dataset.max ? parseFloat(knob.dataset.max) : 1;
    const points = buildBlendMappedPointsForParam(paramId, blendState, target);
    renderMappedPointElements(knob, points, min, max);
  });
}

// ---------------------------------------------------------------------------
// Blend match summary (live view)
// ---------------------------------------------------------------------------

/**
 * Resolve a model id to a display name from the resource library.
 */
function resolveModelName(modelId: string): string {
  const resources = uiState.resourceLibrary?.nam ?? [];
  const resource = findResourceById<LibraryResource>(resources, modelId);
  return resource?.name?.trim() || modelId;
}

export function computeBlendMatchSummary(node: GraphNode, blendState: BlendState): BlendMatchSummary {
  if (blendState.missing) {
    return { name: "Blend not found", details: "This node plays its input dry until it is given a blend." };
  }
  const blend = typeof node.params.blend === "number" ? node.params.blend : 0;
  return describeBlendMix(blendState.mappings, readBlendTarget(node, blendState), blend, blendState.blendMode, resolveModelName);
}

/**
 * Update the live-view blend match summary DOM elements in place.
 * Called after a blend param changes so the matched model label tracks the knob.
 */
export function updateBlendMatchSummary(
  panel: HTMLElement | null,
  node: GraphNode,
  blendState: BlendState,
): void {
  if (!panel) {
    return;
  }
  const lineEl = panel.querySelector(".blend-info-block:not(.is-missing) .blend-match-line") as HTMLElement | null;
  if (!lineEl) {
    return;
  }
  const line = formatBlendMatchLine(computeBlendMatchSummary(node, blendState));
  lineEl.textContent = line;
  lineEl.title = line;
}

// ---------------------------------------------------------------------------
// Blend library helpers
// ---------------------------------------------------------------------------

export function getBlendEntriesForCategory(categoryId: string): Array<{ id: string; name: string; category: string; originalCategory: string }> {
  const blends = uiState.blendLibrary ?? [];
  const mapCategory = (value: string): string => {
    switch (value) {
      case "cab":
        return "cab";
      case "pedal":
        return "utility";
      case "preamp":
      case "amp":
      case "full-rig":
      default:
        return "amp";
    }
  };

  return blends
    .map((blend) => ({
      id: blend.id,
      name: blend.name,
      category: mapCategory(blend.category),
      originalCategory: blend.category,
    }))
    .filter((blend) => blend.category === categoryId);
}

// ---------------------------------------------------------------------------
// Blend editor modal
// ---------------------------------------------------------------------------

const blendEditorModal = new BlendEditorModal({
  getBlendLibrary: () => uiState.blendLibrary ?? ([] as BlendLibrary),
  getResourceLibrary: () => uiState.resourceLibrary,
  // Models from any pack: a Tone3000 pick or Import All comes back into the blend.
  canPickFromTone3000: () => isFeatureEnabled(Features.Tone3000),
  pickModelsFromTone3000: ({ onSelect, onImportMany }) => {
    resourceBrowserModal.open({
      resourceType: "nam",
      contextKey: "blend-models",
      title: "Add Models to the Blend",
      hint: "Select adds one model; Import All adds the whole tone.",
      initialTab: "tone3000",
      onSelect,
      onImportMany,
    });
  },
});

export function initializeBlendEditorModal(): void {
  blendEditorModal.initialize();
  document.addEventListener(BLEND_EDITOR_OPEN_EVENT, (event) => {
    const { blend, sourceNode } = (event as CustomEvent<Partial<BlendEditorOpenDetail> | undefined>).detail ?? {};
    if (!blend) {
      return;
    }
    // Made from a node: once saved, the signal path offers the blend in that node's place.
    blendEditorModal.openWithDefinition(blend, sourceNode
      ? (saved) => document.dispatchEvent(new CustomEvent<BlendEditorOpenDetail>(BLEND_SAVED_FROM_NODE_EVENT, {
        detail: { blend: saved, sourceNode },
      }))
      : undefined);
  });
}

export function openBlendEditorWithDefinition(blend: BlendDefinition): void {
  blendEditorModal.openWithDefinition(blend);
}

export function bindBlendEditorControls(panel: HTMLElement | null, node: GraphNode): void {
  const blendId = node.config?.blendId;
  if (!blendId) {
    return;
  }

  const openButton = panel?.querySelector(".blend-info-edit-btn, .blend-open-btn") as HTMLButtonElement | null;
  openButton?.addEventListener("click", () => {
    blendEditorModal.open(node);
  });
}

// ---------------------------------------------------------------------------
// Blend live-view info rendering (match summary + blend mode override)
// ---------------------------------------------------------------------------

const BLEND_MODE_LABELS: Record<BlendMode, string> = { interpolate: "Interpolate", snap: "Snap" };

/**
 * The one small line under the blend's name: the model playing, or the two being mixed
 * ("Mix: A 60% / B 40%", which names both).
 */
export function formatBlendMatchLine(summary: BlendMatchSummary): string {
  return summary.details || `Playing: ${summary.name}`;
}

/**
 * Render the blend info block shown in the live effect panel: the blend's name with an edit
 * pencil and the blend mode override beside it, and under them one small line naming the
 * model playing (same rule as the Test view). The line is updated in place by
 * `updateBlendMatchSummary` as the mapped param knobs are turned.
 */
export function renderBlendInfoHtml(node: GraphNode, blendState: BlendState): string {
  const summary = computeBlendMatchSummary(node, blendState);
  const modelCount = blendState.mappings.length;
  const nodeId = escapeHtml(node.id);
  // Missing, the name row says so and the line under it says what the node does meanwhile.
  const name = blendState.missing ? summary.name : (blendState.blend?.name || "Blend");
  const matchLine = blendState.missing ? summary.details : formatBlendMatchLine(summary);
  const nameTitle = blendState.missing ? name : `${name} · ${modelCount} model${modelCount === 1 ? "" : "s"}`;
  const editBlendButton = isFeatureEnabled(Features.BlendTools) && !blendState.missing
    ? `<button class="blend-info-edit-btn" data-node-id="${nodeId}" type="button" title="Edit blend" aria-label="Edit blend">${renderIcon("pencil", "blend-info-edit-icon")}</button>`
    : "";
  const option = (value: BlendMode | "", label: string): string =>
    `<option value="${value}" ${blendState.blendModeOverride === value ? "selected" : ""}>${label}</option>`;

  return `
    <div class="node-resource-selector blend-info-block${blendState.missing ? " is-missing" : ""}" data-node-id="${nodeId}">
      <label>Blend</label>
      <div class="blend-info-row">
        <span class="blend-info-name" title="${escapeHtml(nameTitle)}">${escapeHtml(name)}</span>
        ${editBlendButton}
        <select class="blend-mode-select" data-node-id="${nodeId}" title="Blend mode" aria-label="Blend mode">
          ${option("", `Default (${BLEND_MODE_LABELS[blendState.definitionBlendMode]})`)}
          ${option("interpolate", BLEND_MODE_LABELS.interpolate)}
          ${option("snap", BLEND_MODE_LABELS.snap)}
        </select>
      </div>
      <div class="blend-match-line" title="${escapeHtml(matchLine)}">${escapeHtml(matchLine)}</div>
    </div>
  `;
}
