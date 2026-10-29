/**
 * The splitter's Branches control: how many parallel branches its split has.
 *
 * The count is the splitter's outgoing edges; no param stores it. The engine
 * (`setSplitBranchCount`) adds empty branches and removes only empty ones, so a
 * count below the branches that hold effects is offered disabled rather than
 * refused after the click.
 */

import { EffectGuids } from "../../effectGuids.js";
import type { GraphNode, Preset } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { sendSetSplitBranchCount } from "../commands.js";
import { nodeParamsPanelElement } from "../state.js";

export const MIN_SPLIT_BRANCHES = 2;
/** One branch per mixer input (MixerEffect::kMaxInputs). */
export const MAX_SPLIT_BRANCHES = 4;

export interface SplitBranchState {
  count: number;
  /** The fewest branches the split can have without deleting an effect. */
  minCount: number;
}

/** Null for anything but a splitter. */
export function getSplitBranchState(node: GraphNode, preset: Preset): SplitBranchState | null {
  if (node.type !== EffectGuids.kSplitter) {
    return null;
  }
  const nodes = preset.graph?.nodes ?? [];
  const branches = (preset.graph?.edges ?? []).filter((edge) => edge.from === node.id);
  // An empty branch runs from the splitter straight into the mixer.
  const occupied = branches.filter((edge) => nodes.find((n) => n.id === edge.to)?.type !== EffectGuids.kMixer).length;
  return { count: branches.length, minCount: Math.max(MIN_SPLIT_BRANCHES, occupied) };
}

/**
 * The tooltip on a split's remove control. Removing a split keeps the effects of the one
 * branch that has any, back in the main path; with more than one in use the engine refuses.
 */
export function collapseSplitTitle(usedBranches: number): string {
  if (usedBranches === 0) return "Remove split";
  if (usedBranches === 1) return "Remove split: its effects move back into the main signal path";
  return "Remove split: first take the effects off all but one branch";
}

/** The split notes a split of `branches` branches has no branch for: Split Note 2 needs three, 3 four. */
export function isSplitterParamInUse(key: string, branches: number): boolean {
  if (key === "splitNote2") return branches >= 3;
  if (key === "splitNote3") return branches >= 4;
  return true;
}

/**
 * What the branches get in the splitter's mode, from its mode and branch count alone (the knobs
 * show the values); empty in Copy, where every branch gets everything. As uiclient's
 * DescribeSplitRouting words it for Nano Q.
 */
export function describeSplitRouting(node: GraphNode, branches: number): string {
  const count = Math.min(Math.max(branches, 1), MAX_SPLIT_BRANCHES);
  const rest = (what: string): string =>
    count <= 2 ? "" : count === 3 ? `; branch 3 gets ${what}` : `; branches 3 and 4 get ${what}`;
  switch (Math.round(node.params?.mode ?? 0)) {
    case 1:
      return `Position moves the signal from branch 1 to branch ${count}`;
    case 2:
      return "Only the chosen branch plays; the others ring out";
    case 3:
      return `Soft playing to branch 1, hard to branch 2${rest("everything")}`;
    case 4:
      return count === 2
        ? "Notes below the split note to branch 1, the rest to branch 2"
        : `Notes to branches 1 to ${count} by range, at the split notes`;
    case 5:
      return `Left to branch 1, right to branch 2${rest("both")}`;
    default:
      return "";
  }
}

export function buildSplitBranchControlsHtml(node: GraphNode, preset: Preset): string {
  const state = getSplitBranchState(node, preset);
  if (!state) {
    return "";
  }
  const routing = describeSplitRouting(node, state.count);
  const options = [];
  for (let count = MIN_SPLIT_BRANCHES; count <= MAX_SPLIT_BRANCHES; count++) {
    const selected = count === state.count;
    const blocked = count < state.minCount;
    const title = blocked
      ? "Take the effects off a branch first: only empty branches are removed"
      : `${count} parallel branches`;
    options.push(
      `<button type="button" class="split-branch-option${selected ? " is-selected" : ""}" data-branch-count="${count}" aria-pressed="${selected}"${blocked ? " disabled" : ""} title="${escapeHtml(title)}">${count}</button>`,
    );
  }
  return `
    <div class="mixer-inputs-section split-branches-section" data-node-id="${escapeHtml(node.id)}">
      <div class="mixer-inputs-header">Branches</div>
      <div class="split-branch-options" role="group" aria-label="Parallel branches">${options.join("")}</div>
      <div class="split-branch-hint split-branch-routing" data-split-branches="${state.count}"${routing ? "" : " hidden"}>${escapeHtml(routing)}</div>
      <div class="split-branch-hint">New branches start empty. Only an empty branch can be removed.</div>
    </div>
  `;
}

/** After a change to the open splitter's settings: its routing line follows the mode. */
export function refreshSplitRouting(node: GraphNode): void {
  const line = nodeParamsPanelElement?.querySelector<HTMLElement>(".split-branch-routing");
  if (node.type !== EffectGuids.kSplitter || !line) {
    return;
  }
  const routing = describeSplitRouting(node, Number(line.dataset.splitBranches) || MIN_SPLIT_BRANCHES);
  line.textContent = routing;
  line.hidden = routing === "";
}

export function bindSplitBranchControls(node: GraphNode, preset: Preset): void {
  const state = getSplitBranchState(node, preset);
  if (!state || !nodeParamsPanelElement) {
    return;
  }
  nodeParamsPanelElement.querySelectorAll<HTMLButtonElement>(".split-branch-option").forEach((button) => {
    button.addEventListener("click", () => {
      const count = Number(button.dataset.branchCount);
      if (button.disabled || count === state.count) {
        return;
      }
      sendSetSplitBranchCount(node.id, count);
    });
  });
}
