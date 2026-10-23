/**
 * Controls shown only in some modes. A parameter the engine declares with `visibleWhen` (say,
 * one reverb machine's knobs under Verbal's Machine) is drawn with its condition as data
 * attributes and hidden while the condition fails, and a group left with nothing to show hides
 * with it. Display only: the parameter keeps its value, and presets and automation still set it.
 */

import { getNodeEffectInfo } from "../../presetV2.js";
import type { ParameterDef, ParamVisibleWhen } from "../../presetV2.js";
import type { GraphNode } from "../../types.js";
import { escapeHtml } from "../../utils.js";
import { nodeParamsPanelElement } from "../state.js";

/** The engine's `visibleWhen`, or undefined unless it names a key and at least one value. */
export function parseParamVisibleWhen(raw: unknown): ParamVisibleWhen | undefined {
  if (!raw || typeof raw !== "object") {
    return undefined;
  }
  const source = raw as Record<string, unknown>;
  const values = Array.isArray(source.values)
    ? source.values.filter((value): value is number => typeof value === "number" && Number.isFinite(value))
    : [];
  if (typeof source.key !== "string" || source.key === "" || values.length === 0) {
    return undefined;
  }
  return { key: source.key, values };
}

/** Whether `def` shows for these settings; an unset controlling value reads as its default. */
export function isParamVisible(def: ParameterDef, params: Record<string, number>, defs: ParameterDef[]): boolean {
  const when = def.visibleWhen;
  if (!when) {
    return true;
  }
  const raw = params[when.key] ?? defs.find((candidate) => candidate.key === when.key)?.default ?? 0;
  return when.values.includes(Math.round(raw));
}

/**
 * `html` (a control whose root is a `.node-param-group`) carrying `def`'s condition, and
 * hidden from the start when it fails, so the panel never flashes the other modes' controls.
 */
export function withParamVisibility(html: string, def: ParameterDef, visible: boolean): string {
  const when = def.visibleWhen;
  if (!when) {
    return html;
  }
  const attributes = `data-visible-key="${escapeHtml(when.key)}" data-visible-values="${escapeHtml(when.values.join(","))}"${visible ? "" : " hidden"}`;
  return html.replace(`<div class="node-param-group`, `<div ${attributes} class="node-param-group`);
}

/** Shows and hides `root`'s conditional controls for `params`, then the groups they leave empty. */
export function applyParamVisibility(root: ParentNode | null | undefined, params: Record<string, number>, defs: ParameterDef[]): void {
  if (!root) {
    return;
  }
  const conditional = root.querySelectorAll<HTMLElement>("[data-visible-key]");
  if (conditional.length === 0) {
    return;
  }
  conditional.forEach((element) => {
    const key = element.dataset.visibleKey ?? "";
    const values = (element.dataset.visibleValues ?? "").split(",").map(Number);
    const raw = params[key] ?? defs.find((candidate) => candidate.key === key)?.default ?? 0;
    element.hidden = !values.includes(Math.round(raw));
  });
  root.querySelectorAll<HTMLElement>(".node-param-group-block").forEach((block) => {
    const items = Array.from(block.querySelectorAll<HTMLElement>(".node-param-group"));
    block.hidden = items.length > 0 && items.every((item) => item.hidden);
  });
}

/** After a change to `node`'s settings: re-applies the open panel's conditional controls. */
export function refreshNodeParamVisibility(node: GraphNode): void {
  const defs = getNodeEffectInfo(node)?.parameters ?? [];
  if (defs.some((def) => def.visibleWhen)) {
    applyParamVisibility(nodeParamsPanelElement, node.params, defs);
  }
}
