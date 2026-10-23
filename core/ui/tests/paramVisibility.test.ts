import { describe, expect, it } from "vitest";
import type { ParameterDef } from "../ts/presetV2.js";
import {
  applyParamVisibility,
  isParamVisible,
  parseParamVisibleWhen,
  withParamVisibility,
} from "../ts/signalPath/paramsPanel/paramVisibility.js";

const machine: ParameterDef = { key: "machine", name: "Machine", default: 1, min: 0, max: 11, unit: "enum", step: 1 };
const roomSize: ParameterDef = {
  key: "roomSize", name: "Size", default: 0, min: 0, max: 1, unit: "enum", step: 1, group: "Machine",
  visibleWhen: { key: "machine", values: [0] },
};
const diffusion: ParameterDef = {
  key: "diffusion", name: "Diffusion", default: 0.6, min: 0, max: 1, unit: "amount", group: "Machine",
  visibleWhen: { key: "machine", values: [0, 6] },
};
const tone: ParameterDef = { key: "tone", name: "Tone", default: 0.6, min: 0, max: 1, unit: "amount", group: "Reverb" };
const defs = [machine, roomSize, diffusion, tone];

describe("parseParamVisibleWhen", () => {
  it("keeps a key with numeric values", () => {
    expect(parseParamVisibleWhen({ key: "machine", values: [0, 6, "x"] })).toEqual({ key: "machine", values: [0, 6] });
  });

  it("drops anything without a key or a value", () => {
    expect(parseParamVisibleWhen(undefined)).toBeUndefined();
    expect(parseParamVisibleWhen({ key: "", values: [1] })).toBeUndefined();
    expect(parseParamVisibleWhen({ key: "machine", values: [] })).toBeUndefined();
  });
});

describe("isParamVisible", () => {
  it("shows an unconditional control always", () => {
    expect(isParamVisible(tone, {}, defs)).toBe(true);
  });

  it("follows the controlling value, and its default when unset", () => {
    expect(isParamVisible(roomSize, { machine: 0 }, defs)).toBe(true);
    expect(isParamVisible(roomSize, { machine: 1 }, defs)).toBe(false);
    // Unset, Machine reads as its default, Hall.
    expect(isParamVisible(roomSize, {}, defs)).toBe(false);
    expect(isParamVisible(diffusion, { machine: 6 }, defs)).toBe(true);
  });
});

describe("applyParamVisibility", () => {
  const render = (params: Record<string, number>): HTMLElement => {
    const control = (def: ParameterDef): string =>
      withParamVisibility(`<div class="node-param-group"><span>${def.name}</span></div>`, def, isParamVisible(def, params, defs));
    const root = document.createElement("div");
    root.innerHTML = `
      <div class="node-param-group-block" data-group="Reverb">${control(machine)}${control(tone)}</div>
      <div class="node-param-group-block" data-group="Machine">${control(roomSize)}${control(diffusion)}</div>`;
    return root;
  };
  const hidden = (root: HTMLElement, selector: string): boolean => root.querySelector<HTMLElement>(selector)!.hidden;

  it("starts hidden what the settings rule out", () => {
    const root = render({ machine: 1 });
    expect(root.querySelectorAll("[data-visible-key][hidden]").length).toBe(2);
  });

  it("shows a machine's controls when it is chosen, and hides a group left empty", () => {
    const root = render({ machine: 6 });
    applyParamVisibility(root, { machine: 6 }, defs);
    expect(hidden(root, '[data-group="Machine"]')).toBe(false);

    applyParamVisibility(root, { machine: 2 }, defs);
    expect(hidden(root, '[data-group="Machine"]')).toBe(true);
    expect(hidden(root, '[data-group="Reverb"]')).toBe(false);

    applyParamVisibility(root, { machine: 0 }, defs);
    expect(root.querySelectorAll("[data-visible-key][hidden]").length).toBe(0);
  });
});
