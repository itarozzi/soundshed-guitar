/**
 * The blend editor's model table: one row per model, a column per parameter on the
 * Parameters axis, a level trim, and the row's actions. Pure markup from the edit state;
 * the editor binds it by delegation.
 */

import { BLEND_PARAM_SPECS, denormalizeBlendValue, getBlendParamSpec } from "../blendUtils.js";
import type { ResourceLibrary } from "../types.js";
import { escapeHtml } from "../utils.js";
import { libraryModel, modelArchitecture, MAX_LEVEL_TRIM_DB, MIN_LEVEL_TRIM_DB, type BlendEditState, type BlendWarning } from "./editState.js";

/** A captured value as the table shows it: the parameter's own scale, to one decimal. */
export function formatParamValue(value: number | undefined, paramId: string): string {
  if (typeof value !== "number") {
    return "";
  }
  return String(Number(denormalizeBlendValue(value, getBlendParamSpec(paramId)).toFixed(1)));
}

function renderHeader(state: BlendEditState): string {
  const params = state.axis === "params" ? state.params : [];
  const unused = BLEND_PARAM_SPECS.filter((spec) => !state.params.includes(spec.id));
  const paramHeaders = params
    .map((paramId) => {
      const label = getBlendParamSpec(paramId)?.label ?? paramId;
      return `
        <th class="blend-col-param" scope="col">
          <span>${escapeHtml(label)}</span>
          <button class="blend-param-remove" type="button" data-param-id="${escapeHtml(paramId)}"
                  title="Remove ${escapeHtml(label)}" aria-label="Remove ${escapeHtml(label)}">×</button>
        </th>`;
    })
    .join("");
  const addParam = state.axis === "params" && unused.length
    ? `<th class="blend-col-add" scope="col">
         <select class="blend-param-add" aria-label="Add a parameter" title="Add a parameter">
           <option value="">+</option>
           ${unused.map((spec) => `<option value="${escapeHtml(spec.id)}">${escapeHtml(spec.label)}</option>`).join("")}
         </select>
       </th>`
    : "";

  return `
    <thead>
      <tr>
        <th class="blend-col-model" scope="col">Model</th>
        ${paramHeaders}
        ${addParam}
        <th class="blend-col-level" scope="col" title="Level trim in dB">Level</th>
        <th class="blend-col-actions" scope="col" aria-label="Actions"></th>
      </tr>
    </thead>`;
}

export function renderBlendTable(state: BlendEditState, library: ResourceLibrary): string {
  if (!state.rows.length) {
    return `<div class="blend-table-empty">No models yet. Add models to start the blend.</div>`;
  }

  const params = state.axis === "params" ? state.params : [];
  const hasAddColumn = state.axis === "params" && BLEND_PARAM_SPECS.some((spec) => !state.params.includes(spec.id));
  const sweep = state.axis === "sweep";

  const body = state.rows
    .map((row, index) => {
      const model = libraryModel(library, row.id);
      const name = model?.name?.trim() || `Missing: ${row.id}`;
      const cells = params
        .map((paramId) => {
          const spec = getBlendParamSpec(paramId);
          const label = spec?.label ?? paramId;
          return `
            <td class="blend-cell-param">
              <input class="blend-cell-value" type="number" inputmode="decimal" step="0.1"
                     min="${spec?.min ?? 0}" max="${spec?.max ?? 10}" data-index="${index}" data-param-id="${escapeHtml(paramId)}"
                     value="${escapeHtml(formatParamValue(row.values[paramId], paramId))}" aria-label="${escapeHtml(label)} for ${escapeHtml(name)}" />
            </td>`;
        })
        .join("");
      const order = sweep
        ? `<button class="blend-row-move" type="button" data-index="${index}" data-move="-1" title="Move up" aria-label="Move up" ${index === 0 ? "disabled" : ""}>↑</button>
           <button class="blend-row-move" type="button" data-index="${index}" data-move="1" title="Move down" aria-label="Move down" ${index === state.rows.length - 1 ? "disabled" : ""}>↓</button>`
        : "";
      return `
        <tr data-index="${index}">
          <td class="blend-cell-model">
            <div class="blend-model-cell">
              <span class="blend-model-name${model ? "" : " is-missing"}" title="${escapeHtml(name)}">${escapeHtml(name)}</span>
              ${modelArchitecture(model) ? `<span class="blend-model-arch">${escapeHtml(modelArchitecture(model))}</span>` : ""}
            </div>
          </td>
          ${cells}
          ${hasAddColumn ? "<td></td>" : ""}
          <td class="blend-cell-level">
            <input class="blend-cell-trim" type="number" inputmode="decimal" step="0.5" min="${MIN_LEVEL_TRIM_DB}" max="${MAX_LEVEL_TRIM_DB}"
                   data-index="${index}" value="${escapeHtml(row.levelTrimDb ? String(row.levelTrimDb) : "")}" placeholder="0"
                   aria-label="Level trim in dB for ${escapeHtml(name)}" />
          </td>
          <td class="blend-cell-actions">
            ${order}
            <button class="blend-row-remove" type="button" data-index="${index}" title="Remove from blend" aria-label="Remove ${escapeHtml(name)}">×</button>
          </td>
        </tr>`;
    })
    .join("");

  return `<table class="blend-table">${renderHeader(state)}<tbody>${body}</tbody></table>`;
}

export function renderBlendWarnings(warnings: BlendWarning[]): string {
  return warnings
    .map((warning) => `
      <li class="blend-editor-warning">
        <span>${escapeHtml(warning.text)}</span>
        ${warning.action === "split-voicings"
          ? `<button class="blend-warning-action" type="button" data-action="split-voicings">Split by voicing</button>`
          : ""}
      </li>`)
    .join("");
}
