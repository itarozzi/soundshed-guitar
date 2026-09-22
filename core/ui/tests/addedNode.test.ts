import { beforeEach, describe, expect, it } from "vitest";
import { EffectGuids } from "../ts/effectGuids";
import {
  ADDED_NODE_WAIT_MS,
  expectAddedNode,
  forgetExpectedAddedNode,
  takeAddedNode,
} from "../ts/signalPath/addedNode";
import type { GraphNode, Preset } from "../ts/types";

function node(id: string, type: string = EffectGuids.kDelayDigital): GraphNode {
  return { id, type, displayName: id, category: "delay", bypassed: false, params: {}, config: {} };
}

function preset(nodes: GraphNode[], id = "rig"): Preset {
  return { id, name: id, graph: { nodes, edges: [] } };
}

const before = preset([node("amp"), node("cab")]);

describe("takeAddedNode", () => {
  beforeEach(() => forgetExpectedAddedNode());

  it("returns the node the chain did not have when the add was sent", () => {
    expectAddedNode(before, 0);
    const added = node("delay_1");
    expect(takeAddedNode(preset([node("amp"), added, node("cab")]), 5)).toBe(added);
  });

  it("answers once", () => {
    expectAddedNode(before, 0);
    const after = preset([node("amp"), node("delay_1"), node("cab")]);
    takeAddedNode(after, 5);
    expect(takeAddedNode(after, 6)).toBeUndefined();
  });

  it("keeps waiting through renders of the chain before the add", () => {
    expectAddedNode(before, 0);
    expect(takeAddedNode(preset([node("amp"), node("cab")]), 5)).toBeUndefined();
    expect(takeAddedNode(preset([node("amp"), node("cab"), node("delay_1")]), 10)?.id).toBe("delay_1");
  });

  it("stops waiting when another chain renders", () => {
    expectAddedNode(before, 0);
    expect(takeAddedNode(preset([node("fuzz")], "other"), 5)).toBeUndefined();
    expect(takeAddedNode(preset([node("amp"), node("cab"), node("delay_1")]), 10)).toBeUndefined();
  });

  it("stops waiting once an add has had its time", () => {
    expectAddedNode(before, 0);
    expect(takeAddedNode(preset([node("amp"), node("cab"), node("delay_1")]), ADDED_NODE_WAIT_MS + 1)).toBeUndefined();
  });

  it("does not move to a splitter and its mixer", () => {
    expectAddedNode(before, 0);
    const split = preset([node("amp"), node("split_1", EffectGuids.kSplitter), node("mix_1", EffectGuids.kMixer), node("cab")]);
    expect(takeAddedNode(split, 5)).toBeUndefined();
    expect(takeAddedNode(preset([...split.graph!.nodes, node("delay_1")]), 10)).toBeUndefined();
  });

  it("handles an empty chain", () => {
    expectAddedNode(preset([]), 0);
    expect(takeAddedNode(preset([node("delay_1")]), 5)?.id).toBe("delay_1");
  });

  it("yields to a node the user picked meanwhile", () => {
    expectAddedNode(before, 0);
    forgetExpectedAddedNode();
    expect(takeAddedNode(preset([node("amp"), node("cab"), node("delay_1")]), 5)).toBeUndefined();
  });
});
