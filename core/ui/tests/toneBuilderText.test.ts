// @vitest-environment node
import { describe, expect, it } from "vitest";
import type { ToneProposal } from "../ts/toneBuilder/jobTypes.js";
import { capturesLine, packText, presetDescription, sourcesLine } from "../ts/toneBuilder/text.js";

function proposal(extra: Partial<ToneProposal> = {}): ToneProposal {
  return {
    schema: "soundshed.tone-proposal/2", effectCatalogHash: "h", dossierId: null, notes: "internal",
    validation: { ok: true, problems: [], presets: [] }, builderPresets: [],
    credits: [
      { ref: "p:1", provider: "tone3000", title: " Showman ", creator: "ann", creatorUrl: null, sourceUrl: null, licence: "t3k", licenceUrl: null, licencePolicy: "reference-only", attribution: "x" },
      { ref: "p:2", provider: "tone3000", title: "Unused", creator: "bob", creatorUrl: null, sourceUrl: null, licence: "cc-by", licenceUrl: null, licencePolicy: "redistributable", attribution: "y" },
    ],
    presets: [{
      key: "a", name: "A", description: "A loud clean Showman sound.", tags: [], tuning: null, gearIds: [],
      scenes: [{ key: "m", title: "M", chain: [{ split: [[{ fx: "Neural Amp (NAM)", preset: null, set: {}, label: null, off: false, resource: { ref: "p:1", provider: "tone3000", key: "1", type: "nam", sha256: null, gearId: null, captureScope: "amp", licence: { id: "t3k", url: null, policy: "reference-only", attribution: "" }, creator: { name: "ann", url: null }, sourceUrl: null, title: "Showman", providerData: {}, fallback: null } }]], gains: null, masterLevel: null, label: null }] }],
    }],
    sources: [
      { title: "Fender Showman history", url: "https://www.fender.com/showman" },
      { title: "Guitar Player interview", url: "https://guitarplayer.com/a" },
      { title: "", url: "https://example.org/x" },
      { title: "Fourth", url: "https://four.example/y" },
    ],
    ...extra,
  };
}

describe("Tone Builder published text", () => {
  it("lists at most three sources with their sites", () => {
    expect(sourcesLine(proposal())).toBe("Sources: Fender Showman history (fender.com); Guitar Player interview (guitarplayer.com); example.org.");
    expect(sourcesLine(proposal({ sources: [] }))).toBe("");
  });

  it("credits only the captures a preset plays, by name, with no licence", () => {
    const line = capturesLine(proposal().credits, proposal().presets[0]);
    expect(line).toBe("Captures: Showman by ann.");
    expect(line).not.toMatch(/t3k|tone3000|licen/i);
  });

  it("writes the preset description as the designer's text, captures and sources", () => {
    const text = presetDescription(proposal(), "a", "fallback");
    expect(text.split("\n\n")).toEqual([
      "A loud clean Showman sound.",
      "Captures: Showman by ann.",
      expect.stringMatching(/^Sources: /),
    ]);
    expect(text).not.toMatch(/internal|dossier/i);
  });

  it("titles the pack from the proposal, falling back to the subject and research summary", () => {
    expect(packText(proposal({ packTitle: "Dick Dale – Misirlou", packDescription: "Surf at full volume." }), "x", "summary"))
      .toEqual({ title: "Dick Dale – Misirlou", description: expect.stringMatching(/^Surf at full volume\.\n\nSources: /) });
    expect(packText(proposal({ sources: [] }), "Fallback", "The research summary.")).toEqual({ title: "Fallback", description: "The research summary." });
  });
});
