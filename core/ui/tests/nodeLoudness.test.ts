import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as NodeLoudnessModule from "../ts/nodeLoudness.js";

type LoudnessModule = typeof NodeLoudnessModule;
type SentMessage = { type: string; scope?: string; nodeId?: string; presetId?: string };

let loudness: LoudnessModule;
let sent: SentMessage[];

/** A frame as the backend sends it. */
function frame(id: string, scope = "preset", l: unknown[] = [-14.2, null, -15.1, 1.3], extra: Record<string, unknown> = {}): Record<string, unknown> {
  return { type: "sldL", scope, id, l, ...extra };
}

function watches(): SentMessage[] {
  return sent.filter((message) => message.type === "setLoudnessWatch");
}

beforeEach(async () => {
  vi.useFakeTimers();
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  // The module holds the one backend watch for the whole UI, so every test gets a fresh copy.
  vi.resetModules();
  loudness = await import("../ts/nodeLoudness.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.useRealTimers();
});

describe("the loudness watch", () => {
  it("asks the backend to tap the watched node, once", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1", presetId: "p7" });
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1", presetId: "p7" });
    expect(watches()).toEqual([{ type: "setLoudnessWatch", scope: "preset", nodeId: "amp1", presetId: "p7" }]);
  });

  it("moves to another node and lets go with null", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    loudness.setNodeLoudnessWatch({ scope: "post", nodeId: "__output__" });
    loudness.setNodeLoudnessWatch(null);
    loudness.setNodeLoudnessWatch(null);
    expect(watches()).toEqual([
      { type: "setLoudnessWatch", scope: "preset", nodeId: "amp1" },
      { type: "setLoudnessWatch", scope: "post", nodeId: "__output__" },
      { type: "setLoudnessWatch" },
    ]);
  });

  it("renews the lease while held and stops renewing once let go", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    vi.advanceTimersByTime(loudness.NODE_LOUDNESS_RENEW_MS * 2 + 10);
    expect(watches()).toHaveLength(3);

    loudness.setNodeLoudnessWatch(null);
    const afterStop = watches().length;
    vi.advanceTimersByTime(loudness.NODE_LOUDNESS_RENEW_MS * 3);
    expect(watches()).toHaveLength(afterStop);
  });
});

describe("loudness frames", () => {
  it("keeps the watched node's reading and ignores others", () => {
    const listener = vi.fn();
    loudness.setNodeLoudnessListener(listener);
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    listener.mockClear();

    expect(loudness.applyNodeLoudnessFrame(frame("cab1"))).toBe(false);
    expect(loudness.applyNodeLoudnessFrame(frame("amp1", "post"))).toBe(false);
    expect(listener).not.toHaveBeenCalled();
    expect(loudness.getNodeLoudness()).toBeNull();

    expect(loudness.applyNodeLoudnessFrame(frame("amp1"))).toBe(true);
    expect(listener).toHaveBeenCalledTimes(1);
    expect(loudness.getNodeLoudness()).toEqual({
      momentaryLufs: -14.2,
      shortTermLufs: null,
      integratedLufs: -15.1,
      integratedSeconds: 1.3,
    });
  });

  it("accepts the preset id the backend filled in when the UI named none", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    expect(loudness.applyNodeLoudnessFrame(frame("amp1", "preset", undefined, { presetId: "active" }))).toBe(true);
  });

  it("refuses another preset's node when both name one", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1", presetId: "p7" });
    expect(loudness.applyNodeLoudnessFrame(frame("amp1", "preset", undefined, { presetId: "p8" }))).toBe(false);
  });

  it("refuses a malformed frame", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    expect(loudness.applyNodeLoudnessFrame(frame("amp1", "preset", [-14]))).toBe(false);
    expect(loudness.applyNodeLoudnessFrame({ type: "sldL", scope: "preset", id: "amp1" })).toBe(false);
  });

  it("reads anything that is not a finite number as no reading", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    loudness.applyNodeLoudnessFrame(frame("amp1", "preset", [null, "loud", -Infinity, "x"]));
    expect(loudness.getNodeLoudness()).toEqual({
      momentaryLufs: null,
      shortTermLufs: null,
      integratedLufs: null,
      integratedSeconds: 0,
    });
  });

  it("forgets the reading when the watch moves", () => {
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    loudness.applyNodeLoudnessFrame(frame("amp1"));
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "cab1" });
    expect(loudness.getNodeLoudness()).toBeNull();
    expect(loudness.applyNodeLoudnessFrame(frame("amp1"))).toBe(false);
  });

  it("clears a reading that stops arriving, and says so", () => {
    const listener = vi.fn();
    loudness.setNodeLoudnessListener(listener);
    loudness.setNodeLoudnessWatch({ scope: "preset", nodeId: "amp1" });
    loudness.applyNodeLoudnessFrame(frame("amp1"));
    listener.mockClear();

    vi.advanceTimersByTime(loudness.NODE_LOUDNESS_STALE_MS - 10);
    expect(loudness.getNodeLoudness()).not.toBeNull();
    loudness.applyNodeLoudnessFrame(frame("amp1"));
    vi.advanceTimersByTime(loudness.NODE_LOUDNESS_STALE_MS - 10);
    expect(loudness.getNodeLoudness()).not.toBeNull();

    vi.advanceTimersByTime(20);
    expect(loudness.getNodeLoudness()).toBeNull();
    expect(listener).toHaveBeenCalledTimes(2);
  });
});
