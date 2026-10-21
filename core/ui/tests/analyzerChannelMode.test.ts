import { describe, expect, it } from "vitest";
import { formatAnalyzerChannelMode } from "../ts/signalPath/telemetry.js";
import type { InputAnalyzerLevelTelemetry } from "../ts/types.js";

function levels(fields: Partial<InputAnalyzerLevelTelemetry>): InputAnalyzerLevelTelemetry {
  return fields as InputAnalyzerLevelTelemetry;
}

describe("formatAnalyzerChannelMode", () => {
  it("keeps the configured layout as the mode, and what was measured as the hint", () => {
    expect(formatAnalyzerChannelMode(levels({ channelMode: "stereo", activeChannelCount: 2 }), 2)).toEqual({ mode: "Stereo", hint: "" });
    expect(formatAnalyzerChannelMode(levels({ channelMode: "mono", activeChannelCount: 2 }), 2)).toEqual({ mode: "Stereo", hint: "sides identical" });
    expect(formatAnalyzerChannelMode(levels({ channelMode: "mono", activeChannelCount: 1 }), 2)).toEqual({ mode: "Stereo", hint: "one side silent" });
  });

  it("never hints on a mono layout", () => {
    expect(formatAnalyzerChannelMode(levels({ channelMode: "stereo", activeChannelCount: 2 }), 1)).toEqual({ mode: "Mono", hint: "" });
  });

  it("falls back to the measurement when the layout is unknown", () => {
    expect(formatAnalyzerChannelMode(levels({ stereo: true, activeChannelCount: 2 }))).toEqual({ mode: "Stereo", hint: "2 ch" });
    expect(formatAnalyzerChannelMode(levels({ stereo: false }))).toEqual({ mode: "Mono", hint: "" });
  });
});
