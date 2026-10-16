import { renderIcon } from "./iconAssets.js";
import { escapeHtml } from "./utils.js";

export type ResourcePickerLabelLike = {
  dataset: {
    nodeId?: string;
    resourceType?: string;
    resourceIndex?: string;
    exposedResourceId?: string;
  };
};

export function findMatchingResourcePickerLabel<T extends ResourcePickerLabelLike>(
  candidates: ArrayLike<T> | null | undefined,
  nodeId: string,
  resourceType: "nam" | "ir",
  resourceIndex: number,
  exposedResourceId?: string,
): T | null {
  if (!candidates) {
    return null;
  }

  for (let labelIndex = 0; labelIndex < candidates.length; labelIndex += 1) {
    const candidate = candidates[labelIndex];
    if (!candidate) {
      continue;
    }

    if ((candidate.dataset.nodeId ?? "") !== nodeId) {
      continue;
    }

    const candidateType = candidate.dataset.resourceType as "nam" | "ir" | undefined;
    if (candidateType !== resourceType) {
      continue;
    }

    if (exposedResourceId && (candidate.dataset.exposedResourceId ?? "") !== exposedResourceId) {
      continue;
    }

    const candidateResourceIndexRaw = candidate.dataset.resourceIndex;
    const candidateResourceIndex = candidateResourceIndexRaw !== undefined
      ? parseInt(candidateResourceIndexRaw, 10)
      : 0;
    if (candidateResourceIndex !== resourceIndex) {
      continue;
    }

    return candidate;
  }

  return null;
}

/**
 * What a picker label shows. An empty `displayName` means nothing is loaded, and
 * the label becomes the slot's call to action instead of a readout.
 */
export interface ResourcePickerLabelState {
  resourceType: string;
  displayName: string;
  isMissing?: boolean;
  /** A slot that is fine left empty, such as the cabinet's second IR: asked for quietly. */
  isOptional?: boolean;
}

export function getResourcePickerPrompt(resourceType: string, isOptional = false): string {
  if (resourceType === "ir") {
    return isOptional ? "Add a second IR" : "Choose an IR";
  }
  return resourceType === "nam" ? "Choose a model" : "Choose a resource";
}

/** Matches what a drop onto the slot accepts (`inferResourceTypeFromFile`). */
function getResourcePickerDropHint(resourceType: string): string {
  if (resourceType === "ir") {
    return "or drop a .wav file here";
  }
  return resourceType === "nam" ? "or drop a .nam file here" : "";
}

export function getResourcePickerLabelClass(state: ResourcePickerLabelState): string {
  if (!state.displayName) {
    return state.isOptional ? "resource-picker-label is-empty is-optional" : "resource-picker-label is-empty";
  }
  return state.isMissing ? "resource-picker-label is-missing" : "resource-picker-label";
}

export function getResourcePickerLabelTitle(state: ResourcePickerLabelState): string {
  if (state.displayName) {
    return state.displayName;
  }
  const hint = getResourcePickerDropHint(state.resourceType);
  const prompt = getResourcePickerPrompt(state.resourceType, state.isOptional);
  return hint ? `${prompt}, ${hint}` : prompt;
}

/**
 * The label's content. Loaded, it is the resource's name with a caret, so it reads
 * as something that opens a list. Empty, it says what to do: browse, or drop a file.
 */
export function renderResourcePickerLabelContent(state: ResourcePickerLabelState): string {
  if (state.displayName) {
    return `<span class="resource-picker-label-text">${escapeHtml(state.displayName)}</span>${renderIcon("chevron-down", "resource-picker-label-caret")}`;
  }
  const hint = getResourcePickerDropHint(state.resourceType);
  return `${renderIcon("plus", "resource-picker-label-icon")}`
    + `<span class="resource-picker-label-body">`
    + `<span class="resource-picker-label-text">${escapeHtml(getResourcePickerPrompt(state.resourceType, state.isOptional))}</span>`
    + `${hint ? `<span class="resource-picker-label-hint">${escapeHtml(hint)}</span>` : ""}`
    + `</span>`
    + `<span class="resource-picker-label-action" aria-hidden="true">Browse</span>`;
}

/** Redraws a rendered label in place after a pick, a step or a clear. */
export function applyResourcePickerLabelState(labelEl: HTMLElement, displayName: string, isMissing = false): void {
  const state: ResourcePickerLabelState = {
    resourceType: labelEl.dataset.resourceType ?? "",
    displayName,
    isMissing,
    isOptional: labelEl.dataset.optional === "true",
  };
  labelEl.className = getResourcePickerLabelClass(state);
  labelEl.title = getResourcePickerLabelTitle(state);
  labelEl.innerHTML = renderResourcePickerLabelContent(state);
}