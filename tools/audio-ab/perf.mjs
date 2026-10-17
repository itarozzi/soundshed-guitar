/**
 * DSP CPU between revisions: turns AudioPerf's per-round results into a table of µs per block,
 * the best round of each case on each revision, with its ratio against the first revision.
 * audio-ab.mjs runs the timing; this summarises and reports it.
 */

import fs from "node:fs";
import path from "node:path";

/**
 * A case whose second-best round ran more than this above its best is called noisy: with no
 * other round near it, the best may itself be luck. One slow round among close ones is not
 * noise, since interference only ever adds time and the best round discards it.
 */
export const NOISY_SPREAD = 0.05;

/**
 * @param revisions [{ label, spec, sha, describe, dirty, config }], config being AudioPerf's config line
 * @param results   per revision, every AudioPerf result line from every round
 */
export function summarisePerf({ revisions, results, options, machine }) {
  const ids = [];
  const names = new Map();
  for (const rs of results) {
    for (const r of rs) {
      if (!names.has(r.id)) {
        ids.push(r.id);
        names.set(r.id, r.name ?? r.id);
      }
    }
  }
  const cases = ids.map((id) => {
    const perRevision = results.map((rs) => {
      const rounds = rs.filter((r) => r.id === id);
      if (!rounds.length) return { status: "missing" };
      const timed = rounds.filter((r) => !r.error && Number.isFinite(r.meanUs));
      if (!timed.length) return { status: "error", error: rounds[0].error ?? "no timing" };
      const sorted = [...timed].sort((a, b) => a.meanUs - b.meanUs);
      const best = sorted[0];
      const flags = [];
      if (best.passthrough) flags.push("passthrough");
      if (best.rms === 0) flags.push("silent");
      const spread = sorted.length > 1 ? sorted[1].meanUs / best.meanUs - 1 : null;
      if (spread > NOISY_SPREAD) flags.push("noisy");
      return { status: "ok", best, roundMeansUs: timed.map((r) => r.meanUs), spread, flags };
    });
    const base = perRevision[0].best?.meanUs;
    for (const p of perRevision) {
      if (p.best && base) p.ratio = p.best.meanUs / base;
    }
    return { id, name: names.get(id), results: perRevision };
  });
  return {
    createdAt: new Date().toISOString(),
    options,
    machine,
    deadlineUs: (1e6 * options.block) / options.rate,
    revisions,
    cases,
  };
}

const us = (v) => (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2));
const pct = (v) => (v === null ? "–" : `${(100 * v).toFixed(1)}%`);
const ratio = (v) => `${v.toFixed(2)}×`;

function cell(r, first) {
  if (r.status === "missing") return "missing";
  if (r.status === "error") return "error";
  const flag = r.flags.filter((f) => f !== "noisy").map((f) => ` ${f}!`).join("");
  return `${us(r.best.meanUs)}${first || r.ratio === undefined ? "" : ` (${ratio(r.ratio)})`}${flag}`;
}

function bestOf(options) {
  const rounds = options.rounds === 1 ? "only round" : `best of ${options.rounds} rounds`;
  return `the mean of each case's ${rounds} of ${options.seconds} s`;
}

function inputText(options) {
  return options.stereo ? "stereo input (a different part of the DI on each side)" : "mono input";
}

function describeRevision(rev) {
  return `${rev.label} = ${rev.sha.slice(0, 12)} (${rev.describe}${rev.dirty ? ", uncommitted changes" : ""})`;
}

/** Notes on cases a reader should not take at face value. */
function warnings(model) {
  const out = [];
  for (const c of model.cases) {
    c.results.forEach((r, i) => {
      const who = `${c.name} on ${model.revisions[i].label}`;
      if (r.status === "missing") out.push(`${who}: not in this revision's results`);
      else if (r.status === "error") out.push(`${who}: ${r.error}`);
      else {
        if (r.flags.includes("passthrough")) out.push(`${who}: the output equals the input, so nothing was processed (a resource that failed to load?)`);
        if (r.flags.includes("silent")) out.push(`${who}: the output is silent`);
        if (r.flags.includes("noisy")) out.push(`${who}: the second-best round ran ${pct(r.spread)} above the best, so the best may be luck; rerun with more --rounds`);
      }
    });
  }
  return out;
}

export function printPerf(model, { reportDir }) {
  const { options } = model;
  const header = ["", ...model.revisions.map((r) => r.label)];
  const rows = model.cases.map((c) => [c.name, ...c.results.map((r, i) => cell(r, i === 0))]);
  const widths = header.map((_, col) => Math.max(...[header, ...rows].map((row) => row[col].length)));
  const line = (row) => row.map((v, col) => (col === 0 ? v.padEnd(widths[col]) : v.padStart(widths[col]))).join("   ");
  const out = [];
  out.push(`\nDSP CPU  µs per ${options.block}-sample block at ${options.rate} Hz, ${inputText(options)}`);
  out.push(`  ${bestOf(options)}; the deadline is ${us(model.deadlineUs)} µs${model.revisions.length > 1 ? `; ratios are against ${model.revisions[0].label}, below 1 is faster` : ""}\n`);
  out.push(`  ${line(header)}`);
  for (const row of rows) out.push(`  ${line(row)}`);
  out.push("");
  for (const rev of model.revisions) out.push(`  ${describeRevision(rev)}`);
  const notes = warnings(model);
  if (notes.length) {
    out.push("");
    for (const n of notes) out.push(`  ! ${n}`);
  }
  out.push(`\n  Report: ${path.join(reportDir, "perf.md")}`);
  console.log(out.join("\n"));
}

export function writePerfMarkdown(model, reportDir) {
  const { options } = model;
  const revs = model.revisions;
  const out = [];
  out.push(`# DSP CPU: ${revs.map((r) => r.label).join(" → ")}`, "");
  out.push(
    `µs per ${options.block}-sample block at ${options.rate} Hz, ${inputText(options)}: ${bestOf(options)}, ` +
      `one effect or chain per \`SignalGraphExecutor\`. The block deadline is ${us(model.deadlineUs)} µs.` +
      (revs.length > 1 ? ` The ratio is against ${revs[0].label}; below 1 is faster.` : ""),
    "",
  );
  out.push(`| Case | ${revs.map((r) => r.label).join(" | ")} |`, `|---|${revs.map(() => "--:").join("|")}|`);
  for (const c of model.cases) out.push(`| ${c.name} | ${c.results.map((r, i) => cell(r, i === 0)).join(" | ")} |`);
  out.push("");
  const notes = warnings(model);
  if (notes.length) {
    out.push("## Warnings", "");
    for (const n of notes) out.push(`- ${n}`);
    out.push("");
  }
  out.push("## Revisions", "", "| | Commit | Describe | Input layout |", "|---|---|---|---|");
  for (const r of revs) out.push(`| ${r.label} | ${r.sha.slice(0, 12)}${r.dirty ? " + uncommitted" : ""} | ${r.describe} | ${r.config?.layout ?? "?"} |`);
  out.push("", "## Detail", "");
  out.push("Each figure is from the case's best round. The round spread is how far the second-best round's mean ran above the best's: small means the best is not luck.", "");
  out.push("| Case | Revision | Mean µs | Median µs | p99 µs | Max µs | % of deadline | Round spread | Latency (samples) | Output rms |", "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|");
  for (const c of model.cases) {
    c.results.forEach((r, i) => {
      const name = `| ${i === 0 ? c.name : ""} | ${revs[i].label} |`;
      if (!r.best) out.push(`${name} ${r.status}${r.error ? `: ${r.error.replaceAll("|", "\\|")}` : ""} | | | | | | | |`);
      else {
        const b = r.best;
        out.push(`${name} ${us(b.meanUs)} | ${us(b.medianUs)} | ${us(b.p99Us)} | ${us(b.maxUs)} | ${pct(b.meanUs / model.deadlineUs)} | ${pct(r.spread)} | ${b.latencySamples} | ${b.rms.toFixed(4)} |`);
      }
    });
  }
  const m = model.machine;
  const core = revs[0].config?.pinnedCore ?? -1;
  out.push("", `Machine: ${m.cpu}, ${m.logicalCores} logical cores, ${m.platform} ${m.release}. The timing thread ran ${core >= 0 ? `pinned to core ${core}` : "unpinned"} at high priority, with denormals flushed, after a second of warm-up. Run ${model.createdAt}.`, "");
  fs.writeFileSync(path.join(reportDir, "perf.md"), out.join("\n"));
}

export function writePerfJson(model, reportDir) {
  fs.writeFileSync(path.join(reportDir, "perf.json"), JSON.stringify(model, null, 1));
}
