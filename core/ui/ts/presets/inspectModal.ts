/**
 * The preset inspector: the raw JSON and the validation report behind the
 * active preset, and the repair actions offered alongside them.
 */

import { stripLegacyGlobals } from "../presets/sanitize.js";
import { clonePreset, uiState } from "../state.js";
import type { Preset } from "../types.js";
import { cleanupPresetForUi, stripGlobalSignalChainForSave, validatePresetForUi } from "./validate.js";

export function getPresetForModal(): Preset | null {
  const activePreset = uiState.presetCache.get(uiState.activePresetId ?? "") ?? null;
  return activePreset ? clonePreset(activePreset) : null;
}

export function setPresetModalActiveTab(modal: HTMLElement, tabId: string): void {
  const tabButtons = Array.from(modal.querySelectorAll<HTMLElement>(".preset-modal-tab-btn"));
  const tabPanels = Array.from(modal.querySelectorAll<HTMLElement>(".preset-modal-tab-panel"));
  tabButtons.forEach((button) => {
    const active = button.dataset.presetModalTab === tabId;
    button.classList.toggle("active", active);
    button.setAttribute("aria-selected", active ? "true" : "false");
  });
  tabPanels.forEach((panel) => {
    const active = panel.dataset.presetModalTabPanel === tabId;
    panel.classList.toggle("active", active);
  });
}

export function initPresetModalTabs(modal: HTMLElement): void {
  if (modal.dataset.tabsBound === "true") {
    return;
  }
  modal.dataset.tabsBound = "true";
  modal.addEventListener("click", (event) => {
    const target = (event.target as HTMLElement | null)?.closest(".preset-modal-tab-btn") as HTMLElement | null;
    if (!target) {
      return;
    }
    const tabId = target.dataset.presetModalTab;
    if (tabId) {
      setPresetModalActiveTab(modal, tabId);
    }
  });
}

export function updatePresetModalJson(preset: Preset | null): void {
  const pre = document.getElementById("preset-json-view") as HTMLPreElement | null;
  if (!pre) {
    return;
  }
  const withGlobalChain = preset
    ? (() => {
        const cleaned = stripGlobalSignalChainForSave(stripLegacyGlobals(preset));
        const chain = (preset as Preset & { globalSignalChain?: unknown }).globalSignalChain;
        return chain ? { ...cleaned, globalSignalChain: chain } : cleaned;
      })()
    : null;
  pre.textContent = withGlobalChain ? JSON.stringify(withGlobalChain, null, 2) : "";
}

export function updatePresetModalReport(lines: string[]): void {
  const report = document.getElementById("preset-json-report") as HTMLPreElement | null;
  if (!report) {
    return;
  }
  report.textContent = lines.length ? lines.join("\n") : "No issues found.";
}

export function initPresetModalAdvancedActions(modal: HTMLElement): void {
  if (modal.dataset.advancedBound === "true") {
    return;
  }
  modal.dataset.advancedBound = "true";

  const validateBtn = document.getElementById("preset-json-validate") as HTMLButtonElement | null;
  const cleanupBtn = document.getElementById("preset-json-cleanup") as HTMLButtonElement | null;

  validateBtn?.addEventListener("click", () => {
    const preset = getPresetForModal();
    const issues = validatePresetForUi(preset);
    updatePresetModalReport(issues);
  });

  cleanupBtn?.addEventListener("click", () => {
    const preset = getPresetForModal();
    if (!preset) {
      updatePresetModalReport(["No preset loaded."]);
      return;
    }
    const result = cleanupPresetForUi(preset);
    modal.dataset.cleanedPreset = JSON.stringify(result.cleaned);
    updatePresetModalJson(result.cleaned);
    const reportLines: string[] = [];
    if (result.removedKeys.length) {
      reportLines.push(`Removed fields: ${result.removedKeys.sort().join(", ")}`);
    }
    if (result.normalizedAliases > 0) {
      reportLines.push(`Normalized resource ref aliases: ${result.normalizedAliases}`);
    }
    if (result.removedGlobalEq) {
      reportLines.push("Removed default global EQ node.");
    }
    updatePresetModalReport(reportLines.length ? reportLines : ["No unused fields removed."]);
  });
}
