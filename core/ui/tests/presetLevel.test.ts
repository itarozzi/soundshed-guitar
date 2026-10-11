import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as PresetLevelModule from "../ts/presetLevel.js";

type LevelModule = typeof PresetLevelModule;
type SentMessage = Record<string, unknown> & { type: string; requestId: string };

let presetLevel: LevelModule;
let sent: SentMessage[];

function measured(requestId: string, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    type: "presetLevelMeasured",
    requestId,
    presetId: "p1",
    sceneId: "scene-1",
    clipId: "di-riff-03",
    integratedLufs: -17.2,
    peakDbfs: -2.5,
    rmsDbfs: -20.1,
    durationSeconds: 110.4,
    sampleRate: 48000,
    latencySamples: 0,
    slotGainDb: -3.0103,
    tailSeconds: 0.3,
    ...extra,
  };
}

beforeEach(async () => {
  vi.useFakeTimers();
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  // The pending requests live in the module, so every test gets a fresh copy.
  vi.resetModules();
  presetLevel = await import("../ts/presetLevel.js");
});

afterEach(() => {
  vi.useRealTimers();
});

describe("requestPresetLevel", () => {
  it("asks by id with the options given, and resolves with the matching answer", async () => {
    const promise = presetLevel.requestPresetLevel("p1", { sceneId: "scene-2", clipId: "riff", maxSeconds: 20 });
    expect(sent).toHaveLength(1);
    expect(sent[0]).toMatchObject({ type: "measurePresetLevel", presetId: "p1", sceneId: "scene-2", clipId: "riff", maxSeconds: 20 });
    expect(sent[0].preset).toBeUndefined();

    presetLevel.applyPresetLevelMeasured(measured(sent[0].requestId, { sceneId: "scene-2", clipId: "riff" }));
    const level = await promise;
    expect(level.integratedLufs).toBe(-17.2);
    expect(level.peakDbfs).toBe(-2.5);
    expect(level.sceneId).toBe("scene-2");
    expect(level.slotGainDb).toBeCloseTo(-3.01, 2);
  });

  it("sends a whole preset with its id, and leaves out options not given", () => {
    void presetLevel.requestPresetLevel({ id: "built-1", name: "Built", graph: { nodes: [], edges: [] } });
    expect(sent[0]).toMatchObject({ type: "measurePresetLevel", presetId: "built-1", preset: { id: "built-1", name: "Built" } });
    expect(sent[0]).not.toHaveProperty("sceneId");
    expect(sent[0]).not.toHaveProperty("clipId");
    expect(sent[0]).not.toHaveProperty("maxSeconds");
  });

  it("matches answers to requests when several are out", async () => {
    const first = presetLevel.requestPresetLevel("a");
    const second = presetLevel.requestPresetLevel("b");
    expect(sent[0].requestId).not.toBe(sent[1].requestId);

    presetLevel.applyPresetLevelMeasured(measured(sent[1].requestId, { presetId: "b", integratedLufs: -12 }));
    presetLevel.applyPresetLevelMeasured(measured(sent[0].requestId, { presetId: "a", integratedLufs: -24 }));
    await expect(first).resolves.toMatchObject({ presetId: "a", integratedLufs: -24 });
    await expect(second).resolves.toMatchObject({ presetId: "b", integratedLufs: -12 });
  });

  it("reads a silent render's missing levels as null", async () => {
    const promise = presetLevel.requestPresetLevel("quiet");
    presetLevel.applyPresetLevelMeasured(measured(sent[0].requestId, { integratedLufs: null, peakDbfs: null, rmsDbfs: null }));
    await expect(promise).resolves.toMatchObject({ integratedLufs: null, peakDbfs: null, rmsDbfs: null });
  });

  it("rejects with the engine's reason and the missing resources", async () => {
    const promise = presetLevel.requestPresetLevel("p1");
    presetLevel.applyPresetLevelMeasureFailed({
      type: "presetLevelMeasureFailed",
      requestId: sent[0].requestId,
      presetId: "p1",
      message: "1 resource is missing, so the preset would not sound as it should",
      missingResources: [{ nodeId: "amp", nodeType: "amp_nam", resourceType: "nam", resourceId: "x", name: "Plexi", filePath: "" }],
    });
    const error = await promise.catch((e: unknown) => e);
    expect(error).toBeInstanceOf(presetLevel.PresetLevelError);
    const levelError = error as PresetLevelModule.PresetLevelError;
    expect(levelError.message).toContain("missing");
    expect(levelError.missingResources).toEqual([
      { nodeId: "amp", nodeType: "amp_nam", resourceType: "nam", resourceId: "x", name: "Plexi", filePath: "" },
    ]);
  });

  it("rejects when the engine does not answer in time, and ignores a late answer", async () => {
    const promise = presetLevel.requestPresetLevel("slow", { timeoutMs: 1000 });
    const outcome = promise.catch((e: unknown) => e);
    vi.advanceTimersByTime(1001);
    const error = await outcome;
    expect(error).toBeInstanceOf(presetLevel.PresetLevelError);
    expect((error as Error).message).toContain("did not answer");

    expect(() => presetLevel.applyPresetLevelMeasured(measured(sent[0].requestId))).not.toThrow();
  });

  it("ignores answers to requests it never made", () => {
    expect(() => presetLevel.applyPresetLevelMeasured(measured("someone-else"))).not.toThrow();
    expect(() => presetLevel.applyPresetLevelMeasureFailed({ type: "presetLevelMeasureFailed", requestId: "x", message: "no" })).not.toThrow();
  });
});
