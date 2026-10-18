import { beforeEach, describe, expect, it, vi } from "vitest";

const { postMessage } = vi.hoisted(() => ({ postMessage: vi.fn() }));
vi.mock("../ts/bridge.js", () => ({ postMessage }));
vi.mock("../ts/logging.js", () => ({ appendLog: vi.fn() }));
vi.mock("../ts/iconAssets.js", () => ({ getCheckmarkSvg: () => "" }));

// The controls and the readout the tests care about, plus the footer button the module expects
// to find; the rest of the modal is optional to it.
function renderTunerModal(): void {
  document.body.innerHTML = `
    <button id="footer-tuner-btn"></button>
    <div id="tuner-modal" style="display: none;">
      <button id="tuner-mute-btn" aria-pressed="false"></button>
      <input type="checkbox" id="tuner-live-toggle" checked />
      <div class="tuner-display">
        <span id="tuner-note-left"></span><span id="tuner-current-note"></span><span id="tuner-note-right"></span>
        <div id="tuner-pitch-indicator"></div>
        <span id="tuner-cents-value"></span><span id="tuner-cents-indicator"></span>
      </div>
    </div>`;
}

// The module keeps the live mode in module state, so each test starts from a fresh copy.
async function loadTuner() {
  vi.resetModules();
  const tuner = await import("../ts/tuner.js");
  tuner.initializeTuner();
  return tuner;
}

function muteButton(): HTMLButtonElement {
  return document.getElementById("tuner-mute-btn") as HTMLButtonElement;
}

function liveToggle(): HTMLInputElement {
  return document.getElementById("tuner-live-toggle") as HTMLInputElement;
}

function setLiveModeSends(): unknown[] {
  return postMessage.mock.calls
    .map(([message]) => message as { type?: string; action?: string; liveMode?: boolean })
    .filter((message) => message.type === "tuner" && message.action === "setLiveMode")
    .map((message) => message.liveMode);
}

describe("tuner mute button", () => {
  beforeEach(() => {
    postMessage.mockClear();
    renderTunerModal();
  });

  it("silences the output through the engine's live mode", async () => {
    await loadTuner();

    muteButton().click();

    expect(setLiveModeSends()).toEqual([false]);
    expect(muteButton().classList.contains("muted")).toBe(true);
    expect(muteButton().getAttribute("aria-pressed")).toBe("true");
    expect(liveToggle().checked).toBe(false);

    muteButton().click();

    expect(setLiveModeSends()).toEqual([false, true]);
    expect(muteButton().classList.contains("muted")).toBe(false);
    expect(liveToggle().checked).toBe(true);
  });

  it("follows the Live toggle", async () => {
    await loadTuner();

    liveToggle().checked = false;
    liveToggle().dispatchEvent(new Event("change"));

    expect(setLiveModeSends()).toEqual([false]);
    expect(muteButton().classList.contains("muted")).toBe(true);
    expect(muteButton().getAttribute("aria-pressed")).toBe("true");
  });

  it("follows the engine's reported live mode", async () => {
    const tuner = await loadTuner();

    tuner.handleTunerLiveModeChanged(false);

    expect(muteButton().classList.contains("muted")).toBe(true);
    expect(liveToggle().checked).toBe(false);
    // Reflecting the engine's state must not echo it back.
    expect(setLiveModeSends()).toEqual([]);
  });
});

describe("tuner readout", () => {
  beforeEach(() => {
    postMessage.mockClear();
    renderTunerModal();
  });

  it("starts a fresh average when the note changes", async () => {
    // Cents are relative to the note named: E at +45 averaged with F at -45 showed F in tune.
    const tuner = await loadTuner();
    tuner.openTuner();
    for (let i = 0; i < 5; i += 1) {
      tuner.handleTunerUpdate({ detected: true, noteName: "E", frequency: 338, centOffset: 45, confidence: 1 });
    }
    tuner.handleTunerUpdate({ detected: true, noteName: "F", frequency: 340, centOffset: -45, confidence: 1 });

    expect(document.getElementById("tuner-current-note")!.textContent).toBe("F");
    expect(document.getElementById("tuner-cents-value")!.textContent).toBe("-45.0");
  });
});
