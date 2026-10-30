/**
 * The multi-band effects' section of the params panel: which effects get it, and what the
 * Compressor's and the Distortion's plots show differently (the hanging bar's legend and sign,
 * where each band's strip mark comes from, and the Compressor's knees).
 */
import { describe, expect, it } from "vitest";
import type { EqSpectrumBand } from "../ts/eqSpectrum";
import type { GraphNode } from "../ts/types";

// Module-level DOM roots are looked up at import, so the page exists first.
document.body.innerHTML = `
  <div id="signal-path-bar"><div class="signal-path-scroll"><div id="signal-path-nodes"></div></div></div>
  <div id="node-params-panel"></div>
`;

const { EffectGuids } = await import("../ts/effectGuids.js");
const { MULTIBAND_KINDS, buildMultibandSectionHtml, multibandKind } = await import(
  "../ts/signalPath/paramsPanel/multibandPanel.js"
);

function node(type: string, params: Record<string, number> = {}): GraphNode {
  return { id: "n1", type, params } as unknown as GraphNode;
}

describe("the multi-band panel", () => {
  it("is built for the Compressor and the Distortion, by GUID or alias, and nothing else", () => {
    expect(multibandKind(node(EffectGuids.kMultibandCompressor))?.sectionClass).toBe("mb-compressor");
    expect(multibandKind(node("multiband_distortion"))?.sectionClass).toBe("mb-distortion");
    expect(multibandKind(node(EffectGuids.kDistortion))).toBeNull();
    expect(buildMultibandSectionHtml(node(EffectGuids.kEqParametric))).toBe("");
  });

  it("names what hangs from the top of each band", () => {
    const compressor = buildMultibandSectionHtml(node(EffectGuids.kMultibandCompressor));
    const distortion = buildMultibandSectionHtml(node(EffectGuids.kMultibandDistortion));
    expect(compressor).toContain(">Reduction<");
    expect(distortion).toContain(">Drive<");
    expect(distortion).toContain("mb-section mb-distortion");
  });

  it("marks the Compressor's strip at its thresholds, from the knobs", () => {
    const kind = MULTIBAND_KINDS[EffectGuids.kMultibandCompressor];
    expect(kind.depthSign).toBe("−");
    expect(kind.thresholds({ threshold1: -30, threshold3: -12 }, undefined)).toEqual([-30, -24, -12, -24]);
  });

  it("shades the Compressor's knees, from the knobs, and no knee for the Distortion", () => {
    expect(MULTIBAND_KINDS[EffectGuids.kMultibandCompressor].knees?.({ knee2: 0, knee4: 24 })).toEqual([6, 0, 6, 24]);
    expect(MULTIBAND_KINDS[EffectGuids.kMultibandDistortion].knees).toBeUndefined();
  });

  it("marks the Distortion's strip where the engine says its knee is, else from Drive", () => {
    const kind = MULTIBAND_KINDS[EffectGuids.kMultibandDistortion];
    expect(kind.depthSign).toBe("+");
    // Before the first frame: the nominal -18 dBFS, 12 dB of headroom, less each Drive.
    expect(kind.thresholds({ drive1: 30 }, undefined)).toEqual([-36, -34, -42, -36]);
    const band = (thresholdDb?: number): EqSpectrumBand => ({
      lowHz: null,
      highHz: null,
      gainReductionDb: 0,
      levelDb: -40,
      ...(thresholdDb === undefined ? {} : { thresholdDb }),
    });
    // A nominal level of -14 dBFS moves every knee up 4 dB; the engine knows, the UI does not.
    expect(kind.thresholds({ drive1: 30 }, [band(-32), band(-20), band(), band(-14)])).toEqual([-32, -20, -42, -14]);
  });
});
