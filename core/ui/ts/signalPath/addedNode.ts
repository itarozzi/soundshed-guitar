/**
 * Which node an add produced, so the chain can select it.
 *
 * The engine names a new node itself and answers an add with nothing but the
 * rebuilt chain. So the UI notes which nodes the chain had when it asked, and the
 * first render of that chain with a node it did not have is the answer.
 *
 * Adding an effect is picking it: its controls and visualisation should come up
 * without a second click on the new tile.
 */

import { EffectGuids } from "../effectGuids.js";
import type { GraphNode, Preset } from "../types.js";

/**
 * How long an add may take to come back. Generous, because the engine rebuilds
 * the whole chain first, models included; short enough that a failed add does
 * not leave the next unrelated new node to be selected.
 */
export const ADDED_NODE_WAIT_MS = 10_000;

type ExpectedAdd = {
  presetId: string;
  knownNodeIds: ReadonlySet<string>;
  expiresAt: number;
};

let expected: ExpectedAdd | null = null;

/** Call as the add is sent, with the chain it is going into. */
export function expectAddedNode(preset: Preset | null | undefined, now = Date.now()): void {
  expected = preset
    ? {
      presetId: preset.id,
      knownNodeIds: new Set((preset.graph?.nodes ?? []).map((node) => node.id)),
      expiresAt: now + ADDED_NODE_WAIT_MS,
    }
    : null;
}

/** The user picked a node before the add came back; that choice stands. */
export function forgetExpectedAddedNode(): void {
  expected = null;
}

/**
 * The node the expected add put into `preset`, once it has arrived. A render of
 * the same chain without it keeps waiting; another chain, or running out of time,
 * ends the wait.
 */
export function takeAddedNode(preset: Preset, now = Date.now()): GraphNode | undefined {
  if (!expected) {
    return undefined;
  }
  if (now > expected.expiresAt || preset.id !== expected.presetId) {
    expected = null;
    return undefined;
  }

  const { knownNodeIds } = expected;
  const added = (preset.graph?.nodes ?? []).filter((node) => !knownNodeIds.has(node.id));
  if (!added.length) {
    return undefined;
  }

  expected = null;
  // A splitter arrives with its mixer, and neither has controls worth moving to.
  return added.find((node) => node.type !== EffectGuids.kSplitter && node.type !== EffectGuids.kMixer);
}
