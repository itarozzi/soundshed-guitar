import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type * as IrAlignmentModule from "../ts/irAlignment.js";

type AlignmentModule = typeof IrAlignmentModule;
type SentMessage = { type: string; requestId?: string; irA?: unknown; irB?: unknown };

let irAlignment: AlignmentModule;
let sent: SentMessage[];

const SAMPLE_RATE = 48000;

/** Float32 samples as the engine sends them: little-endian, base64'd. */
function encode(samples: number[]): string {
  const bytes = new Uint8Array(new Float32Array(samples).buffer);
  let binary = "";
  bytes.forEach((byte) => {
    binary += String.fromCharCode(byte);
  });
  return btoa(binary);
}

/** A short decaying cabinet-ish IR starting at `onset` samples. */
function cabLike(onset: number, length = 2400): number[] {
  return Array.from({ length }, (_, n) => {
    const t = (n - onset) / SAMPLE_RATE;
    return t < 0 ? 0 : Math.exp(-t / 0.003) * (Math.sin(2 * Math.PI * 900 * t) + 0.5 * Math.sin(2 * Math.PI * 2700 * t));
  });
}

function analysisReply(requestId: string | undefined, extra: Record<string, unknown> = {}): Record<string, unknown> {
  const maxOffsetSamples = 4;
  return {
    type: "irAlignment",
    requestId,
    sampleRate: SAMPLE_RATE,
    waveformA: encode(cabLike(0)),
    waveformB: encode(cabLike(0)),
    normalizeGainA: 2,
    normalizeGainB: 0.5,
    alignedOffsetMs: -0.05,
    invertB: false,
    match: 0.9,
    onsetAMs: 0,
    onsetBMs: 0,
    maxOffsetSamples,
    matchByOffset: [-0.2, 0, 0.4, 0.8, 0.9, 0.6, 0.2, -0.1, -0.3],
    ...extra,
  };
}

beforeEach(async () => {
  vi.useFakeTimers();
  sent = [];
  window.IPlugSendMsg = (payload: string) => {
    sent.push(JSON.parse(payload) as SentMessage);
  };
  // The module keeps the one request in flight for the whole UI, so every test gets a fresh copy.
  vi.resetModules();
  irAlignment = await import("../ts/irAlignment.js");
});

afterEach(() => {
  delete window.IPlugSendMsg;
  vi.useRealTimers();
});

describe("alignment requests", () => {
  it("sends both refs and delivers the decoded analysis", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignment({ resourceId: "a" }, { filePath: "C:/irs/b.wav" }, listener);
    const [request] = sent.filter((message) => message.type === "analyzeIrAlignment");
    expect(request).toMatchObject({ irA: { resourceId: "a" }, irB: { filePath: "C:/irs/b.wav" } });

    expect(irAlignment.applyIrAlignment(analysisReply(request.requestId))).toBe(true);
    const result = listener.mock.calls[0][0] as IrAlignmentModule.IrAlignmentResult;
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.analysis.alignedOffsetMs).toBe(-0.05);
      expect(result.analysis.waveformA).toHaveLength(2400);
      expect(result.analysis.waveformA[10]).toBeCloseTo(cabLike(0)[10], 6);
    }
  });

  it("reports the engine's error, and drops a reply nobody is waiting for", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignment({ resourceId: "a" }, { resourceId: "b" }, listener);
    const [request] = sent;
    expect(irAlignment.applyIrAlignment({ requestId: "someone-else" })).toBe(false);
    irAlignment.applyIrAlignment({ requestId: request.requestId, error: "IR B could not be read" });
    expect(listener).toHaveBeenCalledWith({ ok: false, error: "IR B could not be read" });
  });

  it("gives up on a request the engine never answers", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignment({ resourceId: "a" }, { resourceId: "b" }, listener);
    vi.advanceTimersByTime(irAlignment.IR_ALIGNMENT_TIMEOUT_MS);
    expect(listener).toHaveBeenCalledWith({ ok: false, error: "The engine did not answer" });
  });

  it("rejects a match curve that does not fit its reach", () => {
    expect(irAlignment.parseIrAlignment(analysisReply("x", { matchByOffset: [0, 1] }))).toBeNull();
  });

  it("asks about one IR alone, leaving the empty slot out", () => {
    irAlignment.requestIrAlignment({ resourceId: "a" }, null, vi.fn());
    const [request] = sent;
    expect(request.irA).toEqual({ resourceId: "a" });
    expect("irB" in request).toBe(false);
  });

  it("reads a one-IR reply as that slot alone, with nothing to align", () => {
    const single = irAlignment.parseIrAlignment({
      sampleRate: SAMPLE_RATE,
      waveformB: encode(cabLike(0)),
      normalizeGainB: 0.5,
    });
    expect(single).toMatchObject({ slots: "b", normalizeGainB: 0.5, maxOffsetSamples: 0, matchByOffset: [0] });
    expect(single?.waveformA).toHaveLength(0);
    expect(single?.waveformB).toHaveLength(2400);
  });
});

describe("the match at an offset", () => {
  it("reads the curve at whole samples and interpolates between them", () => {
    const analysis = irAlignment.parseIrAlignment(analysisReply("x"));
    expect(analysis).not.toBeNull();
    if (!analysis) return;
    const msPerSample = 1000 / SAMPLE_RATE;
    expect(irAlignment.matchAtOffset(analysis, 0)).toBeCloseTo(0.9);
    expect(irAlignment.matchAtOffset(analysis, -msPerSample)).toBeCloseTo(0.8);
    expect(irAlignment.matchAtOffset(analysis, 0.5 * msPerSample)).toBeCloseTo(0.75);
    // Beyond the analysis's reach the ends hold.
    expect(irAlignment.matchAtOffset(analysis, 10)).toBeCloseTo(-0.3);
  });
});

describe("the summed response, from the engine", () => {
  type ResponseMessage = SentMessage & { params?: Record<string, number>; offsetMs?: number; points?: number };
  const responseRequests = (): ResponseMessage[] =>
    sent.filter((message) => message.type === "getIrAlignmentResponse") as ResponseMessage[];
  const query = (offsetMs: number, params: Record<string, number> = { irBlend: 0.5 }): IrAlignmentModule.AlignmentResponseQuery => ({
    irA: { resourceId: "a" },
    irB: { filePath: "C:/irs/b.wav" },
    params,
    offsetMs,
  });
  const reply = (requestId: string | undefined, extra: Record<string, unknown> = {}): Record<string, unknown> => ({
    type: "irAlignmentResponse",
    requestId,
    slots: "both",
    frequencies: [20, 200, 2000, 20000],
    summedDb: [-3, 0, 2, -12],
    unrelatedDb: [-4, -1, 1, -13],
    ...extra,
  });

  it("asks with the IRs, the node's params and the offset, and delivers the curves", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignmentResponse(query(0.25, { irBlend: 0.4, lowCutHz: 120 }), listener);
    const [request] = responseRequests();
    expect(request).toMatchObject({
      irA: { resourceId: "a" },
      irB: { filePath: "C:/irs/b.wav" },
      params: { irBlend: 0.4, lowCutHz: 120 },
      offsetMs: 0.25,
      points: irAlignment.IR_ALIGNMENT_RESPONSE_POINTS,
    });

    expect(irAlignment.applyIrAlignmentResponse(reply(request.requestId))).toBe(true);
    expect(listener).toHaveBeenCalledWith({
      ok: true,
      response: {
        slots: "both",
        frequencies: [20, 200, 2000, 20000],
        summedDb: [-3, 0, 2, -12],
        unrelatedDb: [-4, -1, 1, -13],
      },
    });
  });

  it("keeps one request out and folds a drag's changes into the newest", () => {
    const first = vi.fn();
    const skipped = vi.fn();
    const newest = vi.fn();
    irAlignment.requestIrAlignmentResponse(query(0.1), first);
    irAlignment.requestIrAlignmentResponse(query(0.2), skipped);
    irAlignment.requestIrAlignmentResponse(query(0.3), newest);
    expect(responseRequests()).toHaveLength(1);

    irAlignment.applyIrAlignmentResponse(reply(responseRequests()[0].requestId));
    expect(first).toHaveBeenCalledTimes(1);
    const requests = responseRequests();
    expect(requests).toHaveLength(2);
    expect(requests[1].offsetMs).toBe(0.3);

    irAlignment.applyIrAlignmentResponse(reply(requests[1].requestId));
    expect(newest).toHaveBeenCalledTimes(1);
    expect(skipped).not.toHaveBeenCalled();
  });

  it("reports the engine's error, drops a reply nobody is waiting for, and gives up on silence", () => {
    const listener = vi.fn();
    irAlignment.requestIrAlignmentResponse(query(0), listener);
    expect(irAlignment.applyIrAlignmentResponse({ requestId: "someone-else" })).toBe(false);
    irAlignment.applyIrAlignmentResponse({ requestId: responseRequests()[0].requestId, error: "IR B could not be read" });
    expect(listener).toHaveBeenCalledWith({ ok: false, error: "IR B could not be read" });

    const unanswered = vi.fn();
    const next = vi.fn();
    irAlignment.requestIrAlignmentResponse(query(1), unanswered);
    irAlignment.requestIrAlignmentResponse(query(2), next);
    vi.advanceTimersByTime(irAlignment.IR_ALIGNMENT_RESPONSE_TIMEOUT_MS);
    expect(unanswered).toHaveBeenCalledWith({ ok: false, error: "The engine did not answer" });
    // The one waiting behind it goes out then.
    expect(responseRequests().at(-1)?.offsetMs).toBe(2);
  });

  it("reads one IR alone without a curve to compare, and rejects a curve that does not fit", () => {
    expect(irAlignment.parseIrAlignmentResponse(reply("x", { slots: "a", unrelatedDb: undefined }))).toEqual({
      slots: "a",
      frequencies: [20, 200, 2000, 20000],
      summedDb: [-3, 0, 2, -12],
      unrelatedDb: null,
    });
    expect(irAlignment.parseIrAlignmentResponse(reply("x", { summedDb: [0, 1] }))).toBeNull();
    expect(irAlignment.parseIrAlignmentResponse(reply("x", { unrelatedDb: undefined }))).toBeNull();
  });

  it("asks a tracked plot again only when its query changes, and redraws with the answer", () => {
    const owner = {};
    const redraw = vi.fn();
    expect(irAlignment.trackedIrAlignmentResponse(owner, query(0.5), redraw)).toBeNull();
    expect(irAlignment.trackedIrAlignmentResponse(owner, query(0.5), redraw)).toBeNull();
    expect(responseRequests()).toHaveLength(1);

    irAlignment.applyIrAlignmentResponse(reply(responseRequests()[0].requestId));
    expect(redraw).toHaveBeenCalledTimes(1);
    expect(irAlignment.trackedIrAlignmentResponse(owner, query(0.5), redraw)).toMatchObject({ ok: true });

    // A failure keeps the last curve on screen.
    irAlignment.trackedIrAlignmentResponse(owner, query(0.6), redraw);
    irAlignment.applyIrAlignmentResponse({ requestId: responseRequests()[1].requestId, error: "IR B could not be read" });
    expect(irAlignment.trackedIrAlignmentResponse(owner, query(0.6), redraw)).toMatchObject({ ok: true });
  });
});
