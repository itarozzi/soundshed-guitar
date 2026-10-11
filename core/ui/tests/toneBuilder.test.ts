// @vitest-environment node
import { describe, expect, it } from "vitest";
import { BuildRegistry, ProposalError, buildPreset, graphHash, validateGraph } from "../ts/toneBuilder/build.js";
import { checkProposal, normaliseCaptureScope } from "../ts/toneBuilder/check.js";
import { sha1Hex, uuidv5 } from "../ts/toneBuilder/ids.js";
import type { BuildInventory, PresetSpec } from "../ts/toneBuilder/types.js";

const AMP_NAM = "49ea214c-91e6-41f9-bd27-ad6eec0ae90a";
const FX_NAM = "c3263344-65e4-4b7e-b102-ea625700e12f";
const IR_CAB = "94fa2577-e904-43b8-968b-9c569c511160";

function inventory(): BuildInventory {
  return {
    effects: [
      {
        type: AMP_NAM, displayName: "Neural Amp (NAM)", category: "amp", requiresResource: true, resourceType: "nam",
        parameters: [{ key: "outputGain", min: -24, max: 24, default: 0 }], presets: [],
      },
      {
        type: FX_NAM, displayName: "Neural FX (NAM)", category: "amp", requiresResource: true, resourceType: "nam",
        parameters: [{ key: "level", min: 0, max: 1, default: 1 }], presets: [],
      },
      {
        type: IR_CAB, displayName: "IR Cabinet", category: "cab", requiresResource: true, resourceType: "ir",
        parameters: [{ key: "mix", min: 0, max: 1, default: 1 }], presets: [],
      },
      {
        type: "delay-type", displayName: "Delay", category: "delay",
        parameters: [
          { key: "time", min: 1, max: 2000, default: 350 },
          { key: "mode", min: 0, max: 2, default: 0, labels: ["Digital", "Tape", "Analog"] },
          { key: "sync", min: 0, max: 1, default: 0 },
        ],
        presets: [{ name: "Slapback", parameters: { time: 110, mode: "Tape" } }],
      },
      { type: "drive-a", displayName: "Twin Name", category: "drive", parameters: [], presets: [] },
      { type: "drive-b", displayName: "Twin Name", category: "drive", parameters: [], presets: [] },
    ],
    resources: [
      { id: "remote:1", type: "nam", name: "Plexi", provider: "remote", creator: "Ann", licence: "cc-by", captureScope: "amp", hash: "h-plexi" },
      { id: "remote:2", type: "nam", name: "Plexi Full", provider: "remote", captureScope: "amp+cab" },
      { id: "remote:3", type: "nam", name: "Fuzz", provider: "remote", captureScope: "pedal" },
      { id: "local:a", type: "ir", name: "V30", provider: "local" },
      { id: "remote:4", type: "ir", name: "V30", provider: "remote" },
      { id: "gone", type: "nam", name: "Gone", fileMissing: true },
    ],
  };
}

const collection = { idScope: "tests/pack", slug: "pack", category: "Tests", tags: ["t"] };

describe("ids", () => {
  it("matches published SHA-1 and UUIDv5 vectors", () => {
    expect(sha1Hex("abc")).toBe("a9993e364706816aba3e25717850c26c9cd0d89d");
    expect(sha1Hex("")).toBe("da39a3ee5e6b4b0d3255bfef95601890afd80709");
    // RFC 4122 appendix example, DNS namespace.
    expect(uuidv5("www.example.com", "6ba7b810-9dad-11d1-80b4-00c04fd430c8")).toBe("2ed6657d-e927-568b-95e1-2665a8aea6a2");
  });
});

describe("buildPreset", () => {
  const spec: PresetSpec = {
    key: "lead", name: "Lead", description: "A lead", tags: ["rock"],
    scenes: [
      {
        key: "main", title: "Main",
        chain: [
          { fx: "Neural Amp (NAM)", resource: { id: "remote:1" }, set: { outputGain: -3 } },
          { fx: "IR Cabinet", resource: { name: "V30" } },
          { split: [[{ fx: "Delay", preset: "Slapback", set: { sync: true } }], []], gains: [1, 0.5], label: "Wet" },
        ],
      },
    ],
  };

  it("builds a valid, deterministic graph with factory presets, enum labels and splits", () => {
    const registry = new BuildRegistry(inventory());
    const first = buildPreset(collection, spec, registry);
    const second = buildPreset(collection, spec, new BuildRegistry(inventory()));
    expect(JSON.stringify(first)).toBe(JSON.stringify(second));

    const { preset, used } = first;
    expect(preset.id).toBe(uuidv5("tests/pack/lead"));
    expect(preset.tags).toEqual(["pack:pack", "t", "rock", "fxpreset:Delay/Slapback"]);
    const graph = preset.scenes[0].graph;
    const amp = graph.nodes.find((n) => n.type === AMP_NAM);
    expect(amp?.params.outputGain).toBe(-3);
    expect(amp?.label).toBe("Neural Amp (NAM) · Plexi (Ann)");
    expect(amp?.resources).toEqual([{ resourceType: "nam", resourceId: "remote:1" }]);
    // A name shared by a local and a remote file picks the local one.
    expect(graph.nodes.find((n) => n.type === IR_CAB)?.resources?.[0].resourceId).toBe("local:a");
    const delay = graph.nodes.find((n) => n.type === "delay-type");
    expect(delay?.params).toEqual({ time: 110, mode: 1, sync: 1 });
    // The dry branch runs splitter -> mixer on port 1 with its gain.
    expect(graph.edges.some((e) => e.fromPort === 1 && e.toPort === 1 && e.gain === 0.5)).toBe(true);
    expect(used.resources[0]).toBe("Plexi by Ann (remote, cc-by)");
    expect(() => validateGraph(graph, "x")).not.toThrow();
    expect(graphHash(graph)).toMatch(/^[0-9a-f]{16}$/);
  });

  it("matches a resource by content hash", () => {
    const built = buildPreset(collection, { key: "h", name: "H", description: "", chain: [{ fx: AMP_NAM, resource: { hash: "h-plexi" } }] }, new BuildRegistry(inventory()));
    expect(built.preset.scenes[0].graph.nodes[1].resources?.[0].resourceId).toBe("remote:1");
  });

  it.each([
    [{ fx: "Nope" }, /Unknown or ambiguous effect/],
    [{ fx: "Twin Name" }, /ambiguous/],
    [{ fx: "Delay", set: { time: 5000 } }, /outside 1..2000/],
    [{ fx: "Delay", set: { mode: "Shimmer" } }, /not one of/],
    [{ fx: "Delay", set: { bogus: 1 } }, /no parameter "bogus"/],
    [{ fx: "Delay", preset: "Nope" }, /has no preset/],
    [{ fx: "Neural Amp (NAM)" }, /needs a resource/],
    [{ fx: "Neural Amp (NAM)", resource: { id: "gone" } }, /not found or file missing/],
    [{ fx: "Neural Amp (NAM)", resource: { id: "local:a" } }, /is ir, the effect needs nam/],
  ])("rejects %j", (item, message) => {
    expect(() => buildPreset(collection, { key: "x", name: "X", description: "", chain: [item as never] }, new BuildRegistry(inventory())))
      .toThrow(message);
  });

  it("rejects an empty chain and a graph with an orphan", () => {
    expect(() => buildPreset(collection, { key: "x", name: "X", description: "", chain: [] }, new BuildRegistry(inventory()))).toThrow(ProposalError);
    expect(() => validateGraph({
      nodes: [{ id: "__input__", type: "input", category: "", label: "", params: {} }, { id: "a", type: "t", category: "", label: "", params: {} }, { id: "__output__", type: "output", category: "", label: "", params: {} }],
      edges: [{ from: "__input__", to: "__output__" }],
    }, "x")).toThrow(/not reachable from the input: a/);
  });

  it("follows effect aliases and lets a caller resolve its own resource specs", () => {
    const registry = new BuildRegistry(inventory(), {
      effectAliases: new Map([["old amp name", "neural amp (nam)"]]),
      resolveResource: (spec, _effect, _type, reg) => (spec.pick
        ? { resource: reg.resources.find((r) => r.id === "remote:2")!, note: "picked", short: "Pick" }
        : undefined),
    });
    const built = buildPreset(collection, { key: "a", name: "A", description: "", chain: [{ fx: "Old Amp Name", resource: { pick: "x" } }] }, registry);
    expect(built.preset.scenes[0].graph.nodes[1].label).toBe("Neural Amp (NAM) · Pick");
    expect(built.used.resources).toEqual(["picked"]);
  });
});

describe("checkProposal", () => {
  const registry = new BuildRegistry(inventory());

  it("requires the shape and at most three scenes", () => {
    const scene = { key: "s", title: "S", chain: [{ fx: "Delay" }] };
    const result = checkProposal({ key: "k", name: "N", description: "", scenes: [scene, { ...scene, key: "b" }, { ...scene, key: "c" }, { ...scene, key: "d" }] }, registry);
    expect(result.ok).toBe(false);
    expect(result.problems).toContain(`no "description"`);
    expect(result.problems.some((p) => p.includes("4 scenes"))).toBe(true);
  });

  it("warns where a capture's scope does not fit the chain", () => {
    const result = checkProposal({
      key: "k", name: "N", description: "d",
      scenes: [
        { key: "full", title: "Full", chain: [{ fx: "Neural Amp (NAM)", resource: { id: "remote:2" } }, { fx: "IR Cabinet", resource: { id: "local:a" } }] },
        { key: "bare", title: "Bare", chain: [{ fx: "Neural Amp (NAM)", resource: { id: "remote:1" } }] },
        { key: "pedal", title: "Pedal", chain: [{ fx: "Neural Amp (NAM)", resource: { id: "remote:3" } }, { fx: "IR Cabinet", resource: { id: "local:a" } }] },
      ],
    }, registry);
    expect(result.ok).toBe(true);
    expect(result.warnings).toHaveLength(3);
    expect(result.warnings[0]).toMatch(/^full: .*full rig/);
    expect(result.warnings[1]).toMatch(/^bare: .*amp-only/);
    expect(result.warnings[2]).toMatch(/^pedal: .*pedal capture/);

    const clean = checkProposal({
      key: "k", name: "N", description: "d",
      scenes: [{ key: "ok", title: "Ok", chain: [{ fx: "Neural FX (NAM)", resource: { id: "remote:3" } }, { fx: "Neural Amp (NAM)", resource: { id: "remote:1" } }, { fx: "IR Cabinet", resource: { id: "local:a" } }] }],
    }, registry);
    expect(clean).toMatchObject({ ok: true, warnings: [] });
  });

  it("maps provider scope words onto the chain rules", () => {
    expect(normaliseCaptureScope("amp+cab")).toBe("full");
    expect(normaliseCaptureScope("Full rig (amp and cab)")).toBe("full");
    expect(normaliseCaptureScope("poweramp")).toBe("amp");
    expect(normaliseCaptureScope("preamp")).toBe("preamp");
    expect(normaliseCaptureScope("cab")).toBeNull();
  });
});
