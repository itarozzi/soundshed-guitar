/**
 * Pairs two AudioSnapshot runs, measures every case (analysis.mjs), and writes the result
 * as a console summary, report.md, report.html (with A/B/difference players) and compare.json.
 *
 * Effects are paired by type GUID first, then by a shared alias, so an effect whose type was
 * retired into another (amp_nam into amp_nam_optimized) is compared against its successor.
 */

import fs from "node:fs";
import path from "node:path";
import { compareRenders, differenceChannels, formatHz, readWav, severityRank, writeWav, SEVERITY, DEFAULT_THRESHOLDS } from "./analysis.mjs";

const readJson = (file) => JSON.parse(fs.readFileSync(file, "utf8"));

function loadRun(dir) {
  const manifest = readJson(path.join(dir, "manifest.json"));
  const effects = fs.existsSync(path.join(dir, "effects.json")) ? readJson(path.join(dir, "effects.json")) : [];
  const info = fs.existsSync(path.join(dir, "render-info.json")) ? readJson(path.join(dir, "render-info.json")) : { label: path.basename(dir) };
  const cases = new Map(manifest.cases.map((c) => [c.id, c]));
  return { dir, manifest, effects, info, cases };
}

// ---------------------------------------------------------------- registry

function pairEffects(typesA, typesB) {
  const byTypeB = new Map(typesB.map((t) => [t.type, t]));
  const pairs = [];
  const pairedB = new Set();
  const shares = (x, y) => {
    const names = new Set([x.type, ...(x.aliases ?? [])]);
    return [y.type, ...(y.aliases ?? [])].some((a) => names.has(a));
  };

  // Same GUID first; then an unpaired type goes to whichever type now answers to one of its
  // names, even one already paired, since a retired type folds into a surviving one.
  for (const a of typesA) {
    const b = byTypeB.get(a.type) ?? typesB.find((t) => shares(a, t));
    pairs.push(b ? { a, b, how: b.type === a.type ? "same" : "alias" } : { a, b: null, how: "removed" });
    if (b) pairedB.add(b.type);
  }
  for (const b of typesB.filter((t) => !pairedB.has(t.type))) {
    const a = typesA.find((t) => shares(b, t));
    pairs.push(a ? { a, b, how: "alias" } : { a: null, b, how: "new" });
  }
  return pairs;
}

const num = (x) => (Number.isInteger(x) ? String(x) : Number(x.toPrecision(6)).toString());

/** What changed in an effect's definition, in words. */
function describeDefinitionChanges(a, b) {
  const out = [];
  if (a.name !== b.name) out.push(`renamed "${a.name}" → "${b.name}"`);
  if (a.category !== b.category) out.push(`category ${a.category} → ${b.category}`);
  if (a.freshPreset !== b.freshPreset) out.push(`starting preset "${a.freshPreset || "none"}" → "${b.freshPreset || "none"}"`);

  const fa = a.freshParams ?? {}, fb = b.freshParams ?? {};
  const freshChanged = new Set();
  for (const k of new Set([...Object.keys(fa), ...Object.keys(fb)])) {
    if (fa[k] === fb[k]) continue;
    freshChanged.add(k);
    if (!(k in fa)) out.push(`new node gets ${k} = ${num(fb[k])} (new parameter)`);
    else if (!(k in fb)) out.push(`new node no longer sets ${k} (was ${num(fa[k])})`);
    else out.push(`new node gets ${k} = ${num(fb[k])} (was ${num(fa[k])})`);
  }

  const pa = new Map((a.parameters ?? []).map((p) => [p.id, p]));
  const pb = new Map((b.parameters ?? []).map((p) => [p.id, p]));
  for (const [id, p] of pb) {
    const q = pa.get(id);
    if (!q) continue;
    const bits = [];
    if (q.min !== p.min || q.max !== p.max) bits.push(`range ${num(q.min)}..${num(q.max)} → ${num(p.min)}..${num(p.max)}`);
    if (q.unit !== p.unit) bits.push(`unit "${q.unit}" → "${p.unit}"`);
    if ((q.step ?? 0) !== (p.step ?? 0)) bits.push(`step ${num(q.step ?? 0)} → ${num(p.step ?? 0)}`);
    if (JSON.stringify(q.labels ?? []) !== JSON.stringify(p.labels ?? [])) bits.push("choices changed");
    if ((q.taper ?? 0) !== (p.taper ?? 0)) bits.push(`taper ${q.taper ? "log" : "linear"} → ${p.taper ? "log" : "linear"}`);
    if (q.default !== p.default && !freshChanged.has(id)) bits.push(`registered default ${num(q.default)} → ${num(p.default)} (a new node still gets ${num(fb[id])})`);
    if (bits.length) out.push(`${id}: ${bits.join(", ")}`);
  }
  return out;
}

// ---------------------------------------------------------------- cases

function compareCase(runA, runB, caseA, caseB, thresholds) {
  const A = readWav(path.join(runA.dir, caseA.file));
  const B = readWav(path.join(runB.dir, caseB.file));
  const m = compareRenders(A, B, {
    playFrames: caseA.playFrames,
    deterministicA: caseA.deterministic,
    deterministicB: caseB.deterministic,
    thresholds,
  });
  const latencyA = caseA.latencySamples, latencyB = caseB.latencySamples;
  if (latencyA !== undefined && latencyB !== undefined && latencyA !== latencyB) {
    m.flags.push({ kind: "reported-latency", severity: "note", text: `reported latency ${latencyA} → ${latencyB} samples` });
  }
  if ((caseB.processFailures ?? 0) > 0) {
    m.flags.push({ kind: "process", severity: "broken", text: `${caseB.processFailures} blocks the controller declined to process` });
    m.verdict = "broken";
  }
  return { m, A, B };
}

function subjectVerdict(cases) {
  return cases.reduce((w, c) => (severityRank(c.verdict) > severityRank(w) ? c.verdict : w), "identical");
}

/** Measures everything; for slight, audible and broken cases, keeps base, target and difference WAVs in the report. */
export function compareRuns(dirA, dirB, reportDir, { thresholds = {} } = {}) {
  const runA = loadRun(dirA), runB = loadRun(dirB);
  const t = { ...DEFAULT_THRESHOLDS, ...thresholds };
  const signals = runB.manifest.signals.map((s) => s.id).filter((id) => runA.manifest.signals.some((s) => s.id === id));
  const subjects = [];
  const definitionChanges = [];
  fs.mkdirSync(path.join(reportDir, "audio"), { recursive: true });

  const measure = (kind, key, name, category, subjectA, subjectB, slug, extra = {}) => {
    const cases = [];
    for (const sig of signals) {
      const caseA = runA.cases.get(`${kind}/${subjectA}/${sig}`);
      const caseB = runB.cases.get(`${kind}/${subjectB}/${sig}`);
      if (!caseA || !caseB) {
        cases.push({ signal: sig, verdict: "broken", flags: [{ kind: "missing", severity: "broken", text: `no ${!caseA ? "base" : "target"} render` }] });
        continue;
      }
      const { m, A, B } = compareCase(runA, runB, caseA, caseB, t);
      const c = { signal: sig, ...m, fileA: path.join(runA.dir, caseA.file), fileB: path.join(runB.dir, caseB.file) };
      delete c.bands;
      c.bands = m.bands?.map((b) => ({ hz: b.hz, dDb: b.dDb, sDb: b.sDb, relevant: b.relevant, aDb: b.aDb }));
      if (severityRank(m.verdict) >= severityRank("slight")) {
        // The report keeps its own copies: the working tree's renders are replaced every run,
        // and a report must go on playing what it measured.
        const stem = path.join(reportDir, "audio", `${kind}-${slug}__${sig}`);
        c.playA = `${stem}.base.wav`;
        c.playB = `${stem}.target.wav`;
        c.diffFile = `${stem}.difference.wav`;
        fs.copyFileSync(c.fileA, c.playA);
        fs.copyFileSync(c.fileB, c.playB);
        writeWav(c.diffFile, differenceChannels(A, B, m.lag ?? 0), A.rate);
      }
      cases.push(c);
    }
    subjects.push({ kind, key, name, category, slug, verdict: subjectVerdict(cases), cases, ...extra });
  };

  for (const { a, b, how } of pairEffects(runA.effects, runB.effects)) {
    if (!a || !b) {
      const t0 = a ?? b;
      subjects.push({ kind: "effect", key: t0.type, name: t0.name, category: t0.category, slug: t0.slug, verdict: a ? "removed" : "new", cases: [] });
      continue;
    }
    const name = how === "alias" ? `${a.name} → ${b.name}` : b.name;
    const changes = describeDefinitionChanges(a, b);
    if (how === "alias") changes.unshift(`${a.slug} (${a.type}) is now ${b.slug} (${b.type})`);
    if (changes.length) definitionChanges.push({ name, changes });
    const hasCases = runA.cases.has(`effect/${a.type}/${signals[0]}`) || runB.cases.has(`effect/${b.type}/${signals[0]}`);
    if (hasCases) measure("effect", `${a.type}→${b.type}`, name, b.category, a.type, b.type, how === "alias" ? `${a.slug}-to-${b.slug}` : b.slug, { definitionChanges: changes });
  }

  const chainIds = (run) => new Set([...run.cases.values()].filter((c) => c.kind === "chain").map((c) => c.subject));
  const chainsA = chainIds(runA), chainsB = chainIds(runB);
  const skippedB = new Map((runB.manifest.skipped ?? []).map((s) => [s.subject, s.reason]));
  const skippedA = new Map((runA.manifest.skipped ?? []).map((s) => [s.subject, s.reason]));
  for (const id of new Set([...chainsA, ...chainsB, ...skippedA.keys(), ...skippedB.keys()])) {
    const any = [...runB.cases.values(), ...runA.cases.values()].find((c) => c.kind === "chain" && c.subject === id);
    const name = any?.name ?? id;
    if (chainsA.has(id) && chainsB.has(id)) {
      measure("chain", id, name, "chain", id, id, id, { description: any?.description });
    } else {
      const reason = skippedA.get(id) ?? skippedB.get(id);
      subjects.push({ kind: "chain", key: id, name, category: "chain", slug: id, verdict: chainsA.has(id) ? "removed" : "new", cases: [], description: any?.description, reason });
    }
  }

  const rank = (s) => (s.verdict === "new" || s.verdict === "removed" ? severityRank("slight") + 0.5 : severityRank(s.verdict));
  subjects.sort((x, y) => rank(y) - rank(x) || x.name.localeCompare(y.name));
  const counts = {};
  for (const s of subjects) counts[s.kind] = { ...(counts[s.kind] ?? {}), [s.verdict]: (counts[s.kind]?.[s.verdict] ?? 0) + 1 };
  return { base: runA.info, target: runB.info, sampleRate: runB.manifest.sampleRate, block: runB.manifest.block, signals, thresholds: t, subjects, definitionChanges, counts };
}

// ---------------------------------------------------------------- output

const VERDICT_TEXT = {
  identical: "bit-identical",
  rounding: "float rounding only",
  inaudible: "measurably different, below audibility",
  slight: "slight, may be audible side by side",
  audible: "audible",
  broken: "broken: non-finite, silent or missing",
  new: "only in the target",
  removed: "only in the base",
};

const f = (x, n = 1) => (x === null || x === undefined || !Number.isFinite(x) ? "–" : (x > 0 ? "+" : "") + x.toFixed(n));
const lvl = (x) => (x === null || x === undefined ? "–" : x <= -199 ? "silent" : x.toFixed(1));
const flagText = (c) => (c.flags ?? []).filter((x) => x.severity !== "note" || x.kind === "latency").map((x) => x.text).join("; ");
const worstCase = (s) => s.cases.reduce((w, c) => (!w || severityRank(c.verdict) > severityRank(w.verdict) ? c : w), null);
const label = (info) => `${info.label}${info.sha ? ` (${info.describe ?? info.sha.slice(0, 10)}${info.dirty ? ", uncommitted changes" : ""})` : ""}`;

/** One line per subject whose measured alignment or reported latency moved. */
function latencyChanges(model) {
  const out = [];
  for (const s of model.subjects) {
    const notes = new Set(s.cases.flatMap((c) => (c.flags ?? []).filter((x) => x.kind === "latency" || x.kind === "reported-latency").map((x) => x.text)));
    if (notes.size) out.push(`${s.name}: ${[...notes].join("; ")}`);
  }
  return out;
}

export function printSummary(model, { reportDir }) {
  const lines = [];
  lines.push(`\nAudio A/B  ${label(model.base)}  →  ${label(model.target)}   ${model.sampleRate} Hz, ${model.block}-sample blocks`);
  for (const kind of ["effect", "chain"]) {
    const c = model.counts[kind] ?? {};
    const parts = [...SEVERITY, "new", "removed"].filter((v) => c[v]).map((v) => `${c[v]} ${v}`);
    lines.push(`  ${kind === "effect" ? "effects" : "chains "}: ${parts.join(", ") || "none"}`);
  }
  const flagged = model.subjects.filter((s) => severityRank(s.verdict) >= severityRank("slight") || s.verdict === "new" || s.verdict === "removed");
  if (flagged.length) lines.push("");
  for (const s of flagged) {
    const w = worstCase(s);
    lines.push(`  ${s.verdict.toUpperCase().padEnd(8)} ${s.kind === "chain" ? "chain " : ""}${s.name}${w ? ` [${w.signal}]: ${flagText(w)}` : s.reason ? `: ${s.reason}` : ""}`);
  }
  const latency = latencyChanges(model);
  if (latency.length) {
    lines.push("\n  Latency changes:");
    for (const l of latency) lines.push(`    ${l}`);
  }
  if (model.definitionChanges.length) {
    lines.push("\n  Definition changes:");
    for (const d of model.definitionChanges) lines.push(`    ${d.name}: ${d.changes.join("; ")}`);
  }
  lines.push(`\n  Report: ${path.join(reportDir, "report.html")}`);
  console.log(lines.join("\n"));
}

export function writeMarkdown(model, reportDir) {
  const out = [];
  out.push(`# Audio A/B: ${model.base.label} → ${model.target.label}`, "");
  out.push(`- Base: ${label(model.base)}`, `- Target: ${label(model.target)}`);
  out.push(`- ${model.sampleRate} Hz, ${model.block}-sample blocks; signals: ${model.signals.join(", ")}`, "");
  for (const kind of ["effect", "chain"]) {
    out.push(`## ${kind === "effect" ? "Effects, each alone at its defaults" : "Whole chains through the app at default settings"}`, "");
    out.push("| | Verdict | Worst signal | Level Δ dB | Tone Δ | Gain-matched null dB | Latency | What changed |", "|---|---|---|---|---|---|---|---|");
    for (const s of model.subjects.filter((x) => x.kind === kind)) {
      const w = worstCase(s);
      out.push(`| ${s.name} | ${s.verdict} | ${w?.signal ?? ""} | ${f(w?.dRmsDb, 2)} | ${w?.worstBand ? `${f(w.worstBand.sDb, 2)} dB @ ${formatHz(w.worstBand.hz)}` : "–"} | ${f(w?.nullGainDb)} | ${w?.lag ? `${w.lag} smp` : "–"} | ${(w ? flagText(w) : s.reason ?? "").replaceAll("|", "\\|")} |`);
    }
    out.push("");
  }
  const latency = latencyChanges(model);
  if (latency.length) {
    out.push("## Latency changes", "");
    for (const l of latency) out.push(`- ${l}`);
    out.push("");
  }
  if (model.definitionChanges.length) {
    out.push("## Definition changes", "");
    for (const d of model.definitionChanges) out.push(`- **${d.name}**: ${d.changes.join("; ")}`);
    out.push("");
  }
  out.push("## Verdicts", "");
  for (const v of [...SEVERITY, "new", "removed"]) out.push(`- **${v}**: ${VERDICT_TEXT[v]}`);
  fs.writeFileSync(path.join(reportDir, "report.md"), out.join("\n") + "\n");
}

const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const rel = (from, file) => path.relative(from, file).split(path.sep).map(encodeURIComponent).join("/");

/** Third-octave tone change (target minus base, less the overall level change) as bars; grey where a band carries no energy. */
function bandChart(bands) {
  if (!bands?.length) return "";
  const W = 560, H = 120, pad = 28, bw = (W - pad) / bands.length;
  const shape = (b) => b.sDb ?? b.dDb;
  const maxAbs = Math.max(1, ...bands.filter((b) => b.relevant).map((b) => Math.min(24, Math.abs(shape(b)))));
  const y0 = H / 2;
  const bars = bands.map((b, i) => {
    const d = Math.max(-24, Math.min(24, shape(b)));
    const h = (Math.abs(d) / maxAbs) * (H / 2 - 8);
    const x = pad + i * bw + 1;
    const cls = !b.relevant ? "bar-off" : d >= 0 ? "bar-up" : "bar-down";
    return `<rect class="${cls}" x="${x.toFixed(1)}" y="${(d >= 0 ? y0 - h : y0).toFixed(1)}" width="${(bw - 2).toFixed(1)}" height="${Math.max(0.5, h).toFixed(1)}"><title>${formatHz(b.hz)}: ${f(shape(b), 2)} dB tone, ${f(b.dDb, 2)} dB in all</title></rect>`;
  });
  const ticks = bands.map((b, i) => ([31.5, 125, 500, 2000, 8000].includes(b.hz) ? `<text x="${(pad + i * bw + bw / 2).toFixed(1)}" y="${H - 1}" class="tick">${formatHz(b.hz)}</text>` : "")).join("");
  return `<svg class="bands" viewBox="0 0 ${W} ${H + 12}" role="img" aria-label="Third-octave tone change, target minus base, beyond the level change">
<line x1="${pad}" x2="${W}" y1="${y0}" y2="${y0}" class="axis"/><text x="0" y="12" class="tick">+${maxAbs.toFixed(1)}</text><text x="0" y="${H - 8}" class="tick">−${maxAbs.toFixed(1)}</text>
${bars.join("")}${ticks}</svg>`;
}

export function writeHtml(model, reportDir) {
  const badge = (v) => `<span class="badge v-${v}">${esc(v)}</span>`;
  const sections = ["effect", "chain"].map((kind) => {
    const rows = model.subjects.filter((s) => s.kind === kind).map((s) => {
      const w = worstCase(s);
      const detail = s.cases.map((c) => {
        const players = c.diffFile
          ? `<div class="players">
<label>Base<audio controls preload="none" src="${rel(reportDir, c.playA)}"></audio></label>
<label>Target<audio controls preload="none" src="${rel(reportDir, c.playB)}"></audio></label>
<label>Difference<audio controls preload="none" src="${rel(reportDir, c.diffFile)}"></audio></label></div>`
          : "";
        return `<tr><td>${esc(c.signal)}</td><td>${badge(c.verdict)}</td><td>${lvl(c.rmsA)} → ${lvl(c.rmsB)}</td><td>${f(c.dRmsDb, 2)}</td>
<td>${c.worstBand ? `${f(c.worstBand.sDb, 2)} @ ${formatHz(c.worstBand.hz)}` : "–"}</td><td>${f(c.nullDb)} / ${f(c.nullGainDb)}</td><td>${c.lag ? c.lag : "–"}</td>
<td>${esc(flagText(c)) || (c.verdict === "identical" ? "" : "–")}</td></tr>
${c.verdict !== "identical" && c.verdict !== "rounding" && c.fileA ? `<tr class="media"><td colspan="8">${bandChart(c.bands)}${players}</td></tr>` : ""}`;
      }).join("");
      const summary = w ? `${esc(w.signal)}: ${esc(flagText(w)) || VERDICT_TEXT[w.verdict]}` : esc(s.reason ?? VERDICT_TEXT[s.verdict]);
      const defs = s.definitionChanges?.length ? `<ul class="defs">${s.definitionChanges.map((x) => `<li>${esc(x)}</li>`).join("")}</ul>` : "";
      return `<details class="subject"${severityRank(s.verdict) >= severityRank("slight") && s.cases.length ? " open" : ""}>
<summary>${badge(s.verdict)}<span class="name">${esc(s.name)}</span><span class="cat">${esc(s.category)}</span><span class="why">${summary}</span></summary>
${s.description ? `<p class="desc">${esc(s.description)}</p>` : ""}${defs}
${s.cases.length ? `<table><thead><tr><th>Signal</th><th>Verdict</th><th>RMS dBFS</th><th>Level Δ</th><th>Worst band Δ</th><th>Null / gain-matched dB</th><th>Lag</th><th>What changed</th></tr></thead><tbody>${detail}</tbody></table>` : ""}
</details>`;
    }).join("\n");
    const c = model.counts[kind] ?? {};
    const tiles = [...SEVERITY, "new", "removed"].filter((v) => c[v]).map((v) => `<div class="tile v-${v}"><b>${c[v]}</b>${esc(v)}</div>`).join("");
    return `<section><h2>${kind === "effect" ? "Effects, each alone at its defaults" : "Whole chains through the app at default settings"}</h2><div class="tiles">${tiles}</div>${rows}</section>`;
  });
  const defs = model.definitionChanges.length
    ? `<section><h2>Definition changes</h2><ul>${model.definitionChanges.map((d) => `<li><b>${esc(d.name)}</b>: ${esc(d.changes.join("; "))}</li>`).join("")}</ul></section>`
    : "";
  const legend = [...SEVERITY, "new", "removed"].map((v) => `<li>${badge(v)} ${esc(VERDICT_TEXT[v])}</li>`).join("");
  const t = model.thresholds;
  const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Audio A/B report</title>
<style>
:root{--bg:#fbfbfa;--fg:#1d1d1b;--muted:#6b6b66;--line:#e2e1dc;--card:#fff;--up:#c2572b;--down:#2f6fb0;--off:#cfcdc6;
--identical:#8a8a84;--rounding:#8a8a84;--inaudible:#4f8a5b;--slight:#b8860b;--audible:#c2572b;--broken:#b3261e;--new:#5b5bb0;--removed:#5b5bb0}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#161615;--fg:#ecebe6;--muted:#9c9b94;--line:#2e2e2b;--card:#1f1f1d;--off:#3a3a36;--up:#e0784c;--down:#5a9be0;--inaudible:#6fb07c;--slight:#d9a520;--audible:#e0784c;--broken:#e5534b}}
:root[data-theme="dark"]{--bg:#161615;--fg:#ecebe6;--muted:#9c9b94;--line:#2e2e2b;--card:#1f1f1d;--off:#3a3a36;--up:#e0784c;--down:#5a9be0;--inaudible:#6fb07c;--slight:#d9a520;--audible:#e0784c;--broken:#e5534b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1100px;margin:0 auto;padding:24px 16px 64px}h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:32px 0 12px}
.meta{color:var(--muted);margin:0 0 4px}.tiles{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 12px}
.tile{background:var(--card);border:1px solid var(--line);border-left:4px solid var(--c);border-radius:6px;padding:6px 12px;min-width:92px;color:var(--muted)}
.tile b{display:block;font-size:20px;color:var(--fg)}
.badge{display:inline-block;min-width:74px;text-align:center;border-radius:10px;padding:1px 8px;font-size:12px;font-weight:600;color:#fff;background:var(--c)}
${[...SEVERITY, "new", "removed"].map((v) => `.v-${v}{--c:var(--${v})}`).join("")}
.subject{background:var(--card);border:1px solid var(--line);border-radius:6px;margin:6px 0}
.subject summary{cursor:pointer;display:flex;gap:10px;align-items:baseline;padding:8px 12px;flex-wrap:wrap}
.name{font-weight:600}.cat{color:var(--muted);font-size:12px}.why{color:var(--muted);flex:1 1 300px}
.desc,.defs{margin:0 12px 8px;color:var(--muted)}table{border-collapse:collapse;width:100%;font-size:13px}
th,td{text-align:left;padding:5px 12px;border-top:1px solid var(--line);vertical-align:top}th{font-weight:600;color:var(--muted)}
.subject table{display:block;overflow-x:auto}tr.media td{border-top:none;padding-top:0}
.players{display:flex;flex-wrap:wrap;gap:12px;margin:6px 0}.players label{display:flex;flex-direction:column;font-size:12px;color:var(--muted)}
audio{height:32px;max-width:260px}.bands{width:100%;max-width:560px;height:auto;display:block}
.bar-up{fill:var(--up)}.bar-down{fill:var(--down)}.bar-off{fill:var(--off)}.axis{stroke:var(--line)}.tick{font-size:9px;fill:var(--muted)}
ul{padding-left:20px}.legend{list-style:none;padding:0}.legend li{margin:4px 0}
</style></head><body><main>
<h1>Audio A/B: ${esc(model.base.label)} → ${esc(model.target.label)}</h1>
<p class="meta">Base ${esc(label(model.base))} · Target ${esc(label(model.target))}</p>
<p class="meta">${model.sampleRate} Hz, ${model.block}-sample blocks · signals ${esc(model.signals.join(", "))} · generated ${esc(new Date().toISOString().slice(0, 16).replace("T", " "))}</p>
${sections.join("\n")}
${defs}
<section><h2>How to read this</h2><ul class="legend">${legend}</ul>
<p class="meta">Level is rms over the whole render. Tone is the largest change in any third-octave band within ${t.toneRangeDb} dB of the loudest, beyond the overall level change (slight ≥ ${t.toneSlightDb} dB, audible ≥ ${t.toneAudibleDb} dB; bars are that change per band, grey where the band carries no energy). The gain-matched null is what remains after aligning the target to the base and matching its gain: above ${t.characterSlightNullDb} dB the waveform itself has changed. Level: slight ≥ ${t.levelSlightDb} dB, audible ≥ ${t.levelAudibleDb} dB. Nothing below ${t.audibleFloorDbfs} dBFS raises a flag. "Difference" plays the aligned target minus the base at its real level.</p></section>
</main></body></html>`;
  fs.writeFileSync(path.join(reportDir, "report.html"), html);
}

export function writeJson(model, reportDir) {
  fs.writeFileSync(path.join(reportDir, "compare.json"), JSON.stringify(model, null, 1));
}
