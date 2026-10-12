/**
 * A dropdown flyout for an enum with too many choices for a knob: Pickup Match's 257 pickups,
 * say. The engine opts a parameter in with `control: "picker"` and can describe each label
 * (`labelInfo`: a group, a subgroup, a picture and a line of detail).
 *
 * The trigger shows the current choice's picture and name. The flyout has the groups as tiles
 * with their pictures, a search box, and the choices under subgroup headings, in index order.
 * The flyout hangs from the body (body-level popovers: zoom and the compact Controls sheet, see
 * positionFlyout and data-control-bar-popover), and closes before it applies a choice, so
 * nothing redraws the row that was clicked while its click is still bubbling.
 */

import { renderIcon } from "../../iconAssets.js";
import { enumLabelIndex } from "../../paramLabels.js";
import { pickerIcon } from "../../pickerIcons.js";
import { getUiZoom } from "../../pointerDrag.js";
import type { ParamLabelInfo, ParameterDef } from "../../presetV2.js";
import type { GraphNode } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendSignalPathNodeParamUpdate } from "../commands.js";
import { refreshNodeVisualizations } from "./visualizations.js";

const VIEWPORT_MARGIN = 8;
const FLYOUT_GAP = 6;
const ALL_GROUPS = "";
/** Below this much room under the trigger, the flyout opens above it when there is more room there. */
const PREFERRED_HEIGHT = 480;

export interface PickerChoice {
  index: number;
  value: number;
  label: string;
  info: ParamLabelInfo;
}

export function isPickerParam(def: ParameterDef | undefined): def is ParameterDef {
  return Boolean(def && def.control === "picker" && def.unit === "enum" && def.labels && def.labels.length > 0);
}

function stepOf(def: ParameterDef): number {
  return typeof def.step === "number" && def.step > 0 ? def.step : 1;
}

export function pickerChoices(def: ParameterDef): PickerChoice[] {
  const step = stepOf(def);
  return (def.labels ?? []).map((label, index) => ({
    index,
    value: def.min + index * step,
    label,
    info: def.labelInfo?.[index] ?? {},
  }));
}

/** The choice a value lands on, as an enum knob would read it. */
export function pickerChoiceIndex(def: ParameterDef, value: number): number {
  return enumLabelIndex(value, def.labels?.length ?? 0, def.min, stepOf(def));
}

/** The groups in first-appearance order, with how many choices each has. */
export function pickerGroups(choices: PickerChoice[]): { name: string; icon?: string; count: number }[] {
  const groups = new Map<string, { name: string; icon?: string; count: number }>();
  for (const choice of choices) {
    const name = choice.info.group;
    if (!name) continue;
    const group = groups.get(name) ?? { name, icon: choice.info.icon, count: 0 };
    group.count += 1;
    groups.set(name, group);
  }
  return [...groups.values()];
}

/** The choices in `group` (ALL_GROUPS for every one) whose text holds every word of `query`. */
export function filterPickerChoices(choices: PickerChoice[], group: string, query: string): PickerChoice[] {
  const words = query.toLowerCase().split(/\s+/).filter(Boolean);
  return choices.filter((choice) => {
    if (group !== ALL_GROUPS && choice.info.group !== group) return false;
    if (words.length === 0) return true;
    const text = `${choice.label} ${choice.info.subgroup ?? ""} ${choice.info.group ?? ""}`.toLowerCase();
    return words.every((word) => text.includes(word));
  });
}

function triggerInnerHtml(choice: PickerChoice | undefined): string {
  const subtitle = choice?.info.subgroup && choice.info.subgroup !== choice.label ? choice.info.subgroup : choice?.info.group ?? "";
  return `
    <span class="enum-picker-trigger-icon">${pickerIcon(choice?.info.icon)}</span>
    <span class="enum-picker-trigger-text">
      <span class="enum-picker-trigger-label">${escapeHtml(choice?.label ?? "")}</span>
      ${subtitle ? `<span class="enum-picker-trigger-sub">${escapeHtml(subtitle)}</span>` : ""}
    </span>
    ${renderIcon("chevron-down", "enum-picker-trigger-chevron")}
  `;
}

/** The trigger alone, showing the current choice: for a row laid out by its owner. */
export function buildEnumPickerButtonHtml(nodeId: string, def: ParameterDef, value: number): string {
  const choice = pickerChoices(def)[pickerChoiceIndex(def, value)];
  return `<button type="button" class="enum-picker-trigger" data-node-id="${escapeHtml(nodeId)}"
        data-param-key="${escapeHtml(def.key)}" aria-haspopup="dialog" aria-expanded="false"
        title="${escapeHtml(choice?.info.detail ?? choice?.label ?? "")}">${triggerInnerHtml(choice)}</button>`;
}

/** The control the params panel draws in place of a knob. */
export function buildEnumPickerTriggerHtml(nodeId: string, def: ParameterDef, value: number, label: string): string {
  return `
    <div class="node-param-group enum-picker-group">
      <span class="node-param-label">${escapeHtml(label)}</span>
      ${buildEnumPickerButtonHtml(nodeId, def, value)}
    </div>
  `;
}

/**
 * What choosing a value means. By default it is a node parameter of the active preset, sent
 * the way any params-panel control sends one; a picker for something else (the global chain's
 * Pickup Match) passes its own.
 */
export type EnumPickerCommit = (def: ParameterDef, value: number) => void;

function commitNodeParam(node: GraphNode): EnumPickerCommit {
  return (def, value) => {
    node.params[def.key] = value;
    sendSignalPathNodeParamUpdate(node.id, def.key, value);
    refreshNodeVisualizations(node);
  };
}

interface OpenPicker {
  popover: HTMLElement;
  close: (returnFocus?: boolean) => void;
  /** Follows a redrawn trigger, or closes when the panel no longer has one. */
  reposition: () => void;
  nodeId: string;
  key: string;
}

let openPicker: OpenPicker | null = null;

export function closeEnumPicker(): void {
  openPicker?.close();
}

export function isEnumPickerOpen(): boolean {
  return openPicker !== null;
}

function findTrigger(nodeId: string, key: string): HTMLElement | null {
  return (
    Array.from(document.querySelectorAll<HTMLElement>(".enum-picker-trigger")).find(
      (trigger) => trigger.dataset.nodeId === nodeId && trigger.dataset.paramKey === key,
    ) ?? null
  );
}

/** Wires every picker trigger the panel just drew to `node`. */
export function bindEnumPickerTriggers(
  root: ParentNode | null | undefined,
  node: GraphNode,
  defs: readonly ParameterDef[],
  commit: EnumPickerCommit = commitNodeParam(node),
): void {
  root?.querySelectorAll<HTMLButtonElement>(".enum-picker-trigger").forEach((trigger) => {
    const def = defs.find((candidate) => candidate.key === trigger.dataset.paramKey);
    if (!isPickerParam(def)) return;
    trigger.addEventListener("click", () => openEnumPicker(trigger, node, def, commit));
    // A panel redraw while the flyout is open: the flyout follows the new trigger.
    if (openPicker && openPicker.nodeId === node.id && openPicker.key === def.key) {
      trigger.setAttribute("aria-expanded", "true");
    }
  });
  openPicker?.reposition();
}

export function openEnumPicker(
  initialAnchor: HTMLElement,
  node: GraphNode,
  def: ParameterDef,
  commit: EnumPickerCommit = commitNodeParam(node),
): void {
  // Re-clicking the trigger that opened it closes it.
  if (openPicker) {
    const same = openPicker.nodeId === node.id && openPicker.key === def.key;
    openPicker.close();
    if (same) return;
  }

  const choices = pickerChoices(def);
  const groups = pickerGroups(choices);
  let anchor = initialAnchor;
  let selectedIndex = pickerChoiceIndex(def, node.params[def.key] ?? def.default);
  let group = choices[selectedIndex]?.info.group ?? ALL_GROUPS;
  let query = "";

  const popover = document.createElement("div");
  popover.className = "enum-picker-popover";
  popover.setAttribute("role", "dialog");
  popover.setAttribute("aria-label", def.name);
  // The compact Controls sheet must not take a click in here for a click outside it.
  popover.setAttribute("data-control-bar-popover", "");
  popover.innerHTML = `
    <div class="enum-picker-header">
      <span class="enum-picker-title">${escapeHtml(def.name)}</span>
      <input class="enum-picker-search" type="search" placeholder="Search ${choices.length} choices"
        aria-label="Search ${escapeHtml(def.name)}" autocomplete="off" spellcheck="false">
    </div>
    ${groups.length > 1 ? `<div class="enum-picker-groups" role="radiogroup" aria-label="Kind"></div>` : ""}
    <div class="enum-picker-list" role="listbox" aria-label="${escapeHtml(def.name)}"></div>
  `;
  document.body.appendChild(popover);

  const search = popover.querySelector<HTMLInputElement>(".enum-picker-search");
  const groupBar = popover.querySelector<HTMLElement>(".enum-picker-groups");
  const list = popover.querySelector<HTMLElement>(".enum-picker-list");

  const renderGroups = (): void => {
    if (!groupBar) return;
    const visible = filterPickerChoices(choices, ALL_GROUPS, query);
    const tile = (name: string, label: string, icon: string, count: number): string => `
      <button type="button" class="enum-picker-group-tile" role="radio" data-group="${escapeHtml(name)}"
        aria-checked="${group === name}">
        <span class="enum-picker-group-icon">${icon}</span>
        <span class="enum-picker-group-name">${escapeHtml(label)}</span>
        <span class="enum-picker-group-count">${count}</span>
      </button>`;
    groupBar.innerHTML =
      tile(ALL_GROUPS, "All", renderIcon("sliders", "enum-picker-all-icon"), visible.length) +
      groups
        .map((entry) =>
          tile(entry.name, entry.name, pickerIcon(entry.icon), visible.filter((c) => c.info.group === entry.name).length))
        .join("");
  };

  const renderList = (): void => {
    if (!list) return;
    const visible = filterPickerChoices(choices, group, query);
    if (visible.length === 0) {
      list.innerHTML = `<div class="enum-picker-empty">Nothing matches</div>`;
      return;
    }
    let lastSubgroup: string | undefined;
    list.innerHTML = visible
      .map((choice) => {
        const heading = choice.info.subgroup !== lastSubgroup && choice.info.subgroup
          ? `<div class="enum-picker-subgroup" role="presentation">${escapeHtml(choice.info.subgroup)}</div>`
          : "";
        lastSubgroup = choice.info.subgroup;
        const selected = choice.index === selectedIndex;
        return `${heading}
          <button type="button" class="enum-picker-option${selected ? " is-selected" : ""}" role="option"
            aria-selected="${selected}" data-index="${choice.index}">
            <span class="enum-picker-option-icon">${pickerIcon(choice.info.icon)}</span>
            <span class="enum-picker-option-text">
              <span class="enum-picker-option-label">${escapeHtml(choice.label)}</span>
              ${choice.info.detail ? `<span class="enum-picker-option-detail">${escapeHtml(choice.info.detail)}</span>` : ""}
            </span>
            ${selected ? renderIcon("check", "enum-picker-option-check") : ""}
          </button>`;
      })
      .join("");
  };

  const position = (): void => {
    if (!anchor.isConnected) {
      // The panel redrew under the flyout; follow its new trigger, or close with the panel.
      const replacement = findTrigger(node.id, def.key);
      if (!replacement) {
        close();
        return;
      }
      anchor = replacement;
      anchor.setAttribute("aria-expanded", "true");
    }
    // The app's zoom is CSS zoom on the body, which scales a fixed element's left and top.
    const zoom = getUiZoom();
    const client = anchor.getBoundingClientRect();
    const rect = { left: client.left / zoom, top: client.top / zoom, bottom: client.bottom / zoom };
    const viewportWidth = window.innerWidth / zoom;
    const viewportHeight = window.innerHeight / zoom;
    const width = popover.offsetWidth;
    const left = Math.min(Math.max(rect.left, VIEWPORT_MARGIN), Math.max(VIEWPORT_MARGIN, viewportWidth - VIEWPORT_MARGIN - width));
    const spaceBelow = viewportHeight - rect.bottom - FLYOUT_GAP - VIEWPORT_MARGIN;
    const spaceAbove = rect.top - FLYOUT_GAP - VIEWPORT_MARGIN;
    const above = spaceBelow < PREFERRED_HEIGHT && spaceAbove > spaceBelow;
    const maxHeight = Math.max(160, Math.floor(above ? spaceAbove : spaceBelow));
    popover.style.maxHeight = `${maxHeight}px`;
    const height = Math.min(popover.offsetHeight, maxHeight);
    const top = above ? Math.max(VIEWPORT_MARGIN, rect.top - FLYOUT_GAP - height) : rect.bottom + FLYOUT_GAP;
    popover.style.left = `${Math.round(left)}px`;
    popover.style.top = `${Math.round(top)}px`;
    popover.classList.toggle("opens-above", above);
  };

  const options = (): HTMLButtonElement[] => Array.from(popover.querySelectorAll<HTMLButtonElement>(".enum-picker-option"));

  const choose = (index: number): void => {
    const choice = choices[index];
    if (!choice) return;
    const trigger = anchor;
    // Close first: nothing may redraw the clicked row while its click is still bubbling.
    close(true);
    selectedIndex = index;
    if (trigger.isConnected) {
      trigger.innerHTML = triggerInnerHtml(choice);
      trigger.title = choice.info.detail ?? choice.label;
    }
    commit(def, choice.value);
  };

  const onDocumentPointerDown = (event: PointerEvent): void => {
    const target = event.target as Node | null;
    if (target && !popover.contains(target) && !anchor.contains(target)) {
      close();
    }
  };
  const onKeyDown = (event: KeyboardEvent): void => {
    if (event.key === "Escape") {
      event.stopPropagation();
      close(true);
    }
  };
  const onReposition = (event: Event): void => {
    // The list scrolling inside the flyout moves nothing the flyout hangs from.
    if (event.target instanceof Node && popover.contains(event.target)) return;
    position();
  };

  function close(returnFocus = false): void {
    document.removeEventListener("pointerdown", onDocumentPointerDown, true);
    document.removeEventListener("keydown", onKeyDown, true);
    window.removeEventListener("resize", onReposition);
    window.removeEventListener("scroll", onReposition, true);
    popover.remove();
    if (openPicker?.popover === popover) {
      openPicker = null;
    }
    anchor.setAttribute("aria-expanded", "false");
    if (returnFocus && anchor.isConnected) {
      anchor.focus();
    }
  }

  groupBar?.addEventListener("click", (event) => {
    const tile = (event.target as HTMLElement).closest<HTMLElement>(".enum-picker-group-tile");
    if (!tile) return;
    group = tile.dataset.group ?? ALL_GROUPS;
    renderGroups();
    renderList();
    position();
  });
  list?.addEventListener("click", (event) => {
    const option = (event.target as HTMLElement).closest<HTMLElement>(".enum-picker-option");
    if (option) choose(Number(option.dataset.index));
  });
  list?.addEventListener("keydown", (event) => {
    const all = options();
    const current = all.indexOf(document.activeElement as HTMLButtonElement);
    const move = (to: number): void => {
      event.preventDefault();
      all[Math.min(Math.max(to, 0), all.length - 1)]?.focus();
    };
    if (event.key === "ArrowDown") {
      move(current + 1);
    } else if (event.key === "ArrowUp" && current <= 0) {
      // Up from the first choice goes back to the search box.
      event.preventDefault();
      search?.focus();
    } else if (event.key === "ArrowUp") {
      move(current - 1);
    } else if (event.key === "Home") {
      move(0);
    } else if (event.key === "End") {
      move(all.length - 1);
    }
  });
  search?.addEventListener("input", () => {
    query = search.value;
    renderGroups();
    renderList();
    position();
  });
  search?.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      options()[0]?.focus();
    } else if (event.key === "Enter") {
      event.preventDefault();
      const first = options()[0];
      if (first) choose(Number(first.dataset.index));
    }
  });

  openPicker = { popover, close, reposition: position, nodeId: node.id, key: def.key };
  anchor.setAttribute("aria-expanded", "true");
  renderGroups();
  renderList();
  position();
  // The current choice in view, by scrolling the list alone: scrollIntoView would move the page too.
  const current = popover.querySelector<HTMLElement>(".enum-picker-option.is-selected");
  if (current && list) {
    list.scrollTop = Math.max(0, current.offsetTop - list.offsetTop - list.clientHeight / 2);
  }
  search?.focus();

  document.addEventListener("pointerdown", onDocumentPointerDown, true);
  document.addEventListener("keydown", onKeyDown, true);
  window.addEventListener("resize", onReposition);
  window.addEventListener("scroll", onReposition, true);
}
