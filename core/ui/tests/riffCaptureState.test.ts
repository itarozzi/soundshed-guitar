/**
 * The riff capture modal's view of a take, as the engine's messages move it along.
 *
 *   - Arming ends when the take starts: the engine never sends `armed` itself. Left set, a take the
 *     engine finished on its own (the buffer full) still read "Armed", and Stop or Close then sent
 *     a cancel that threw the finished take away.
 */
// @vitest-environment jsdom
import { beforeEach, describe, expect, it, vi } from "vitest";

const stopRiffCapture = vi.fn();
vi.mock("../ts/bridge.js", async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  stopRiffCapture: (...args: unknown[]) => stopRiffCapture(...args),
}));
vi.mock("../ts/notifications.js", () => ({ showNotification: vi.fn(), clearNotification: vi.fn() }));

const { uiState } = await import("../ts/state.js");
const { applyRiffCaptureState } = await import("../ts/riffLibrary.js");
const { onRiffCaptureStarted, onRiffCaptureStopped } = await import("../ts/messages/captureHandlers.js");

beforeEach(() => {
  stopRiffCapture.mockReset();
  uiState.riffCapture = undefined as never;
  applyRiffCaptureState({ active: false, complete: false, armed: false, hasAudio: false, waveformPeaks: [] });
});

describe("riff capture state", () => {
  it("is no longer armed once the take starts, or once the engine finishes it", () => {
    applyRiffCaptureState({ armed: true, active: false, complete: false });
    expect(uiState.riffCapture.armed).toBe(true);

    onRiffCaptureStarted({ type: "riffCaptureStarted", takeId: "t1", barAlignOffsetSamples: 0 } as never);
    expect(uiState.riffCapture.armed).toBe(false);

    applyRiffCaptureState({ armed: true, active: false, complete: false });
    onRiffCaptureStopped({ type: "riffCaptureStopped", takeId: "t1", capturedSamples: 48000, sampleRate: 48000, hasAudio: true } as never);
    expect(uiState.riffCapture.armed).toBe(false);
  });
});
