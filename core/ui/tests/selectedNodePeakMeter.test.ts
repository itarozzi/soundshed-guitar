import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import type { uiState as UiState } from "../ts/state.js";
import type { SignalLevelMetrics } from "../ts/types.js";

// The panel root is looked up when signalPath/state.ts loads, so the DOM must exist first.
document.body.innerHTML = `<div id="node-params-panel"></div>`;

let updateSelectedNodePeakMeter: () => void;
let uiState: typeof UiState;

beforeAll(async () => {
  ({ updateSelectedNodePeakMeter } = await import("../ts/signalPath/telemetry.js"));
  ({ uiState } = await import("../ts/state.js"));
  const { setSelectedNodeId } = await import("../ts/signalPath/state.js");
  setSelectedNodeId("amp");
});

function levels(peakDbfs: number, clipped = false): SignalLevelMetrics {
  return { peakDbfs, rmsDbfs: peakDbfs - 6, headroomDb: -peakDbfs, clipped, clipCount: clipped ? 1 : 0 };
}

function setPeak(peakDbfs: number | null, clipped = false): void {
  uiState.signalDiagnostics = peakDbfs === null
    ? null
    : { input: levels(-20), output: levels(-20), nodes: [{ scope: "preset", nodeId: "amp", nodeType: "nam", levels: levels(peakDbfs, clipped) }] };
}

function rail(): HTMLElement {
  return document.querySelector<HTMLElement>(".default-effect-shell-rail")!;
}

function meterScale(): string {
  return document.querySelector<HTMLElement>(".default-effect-shell-meter")!.style.getPropertyValue("--meter-fill-scale");
}

/** Runs one frame and returns how many DOM mutations it made. */
function frame(): number {
  const observer = new MutationObserver(() => {});
  observer.observe(document.getElementById("node-params-panel")!, { subtree: true, attributes: true, childList: true });
  updateSelectedNodePeakMeter();
  const count = observer.takeRecords().length;
  observer.disconnect();
  return count;
}

describe("updateSelectedNodePeakMeter", () => {
  beforeEach(() => {
    // A fresh rail each time, as a re-rendered params panel would have.
    document.getElementById("node-params-panel")!.innerHTML = `
      <div class="default-effect-shell-rail"><span class="default-effect-shell-meter" style="--meter-fill-scale: 0"></span></div>`;
  });

  it("shows the selected node's peak on a fresh rail", () => {
    setPeak(-12);
    frame();

    expect(meterScale()).toBe("0.75");
    expect(rail().classList.contains("is-inactive")).toBe(false);
    expect(rail().classList.contains("is-clipped")).toBe(false);
    expect(rail().title).toContain("-12.0 dBFS");
  });

  it("writes nothing when a frame changes nothing", () => {
    setPeak(-12);
    frame();

    expect(frame()).toBe(0);
  });

  it("writes nothing on repeated frames with no diagnostics", () => {
    setPeak(null);
    frame();

    expect(rail().classList.contains("is-inactive")).toBe(true);
    expect(rail().title).toBe("No diagnostics data for this node");
    expect(frame()).toBe(0);
  });

  it("moves the fill and flags clipping as the level changes", () => {
    setPeak(-24);
    frame();
    setPeak(-0.1, true);
    frame();

    expect(meterScale()).toBe("1.00");
    expect(rail().classList.contains("is-clipped")).toBe(true);

    setPeak(-24);
    frame();

    expect(meterScale()).toBe("0.50");
    expect(rail().classList.contains("is-clipped")).toBe(false);
  });

  it("leaves the title alone while the pointer is elsewhere", () => {
    setPeak(-12);
    frame();
    setPeak(-6);
    frame();

    expect(rail().title).toContain("-12.0 dBFS");
  });
});
