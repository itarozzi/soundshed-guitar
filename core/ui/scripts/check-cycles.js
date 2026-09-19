#!/usr/bin/env node
/**
 * Import-cycle guard for core/ui/ts.
 *
 * ES modules tolerate cycles right up until one module reads a binding from a
 * peer that has not finished evaluating — then it throws a TDZ ReferenceError
 * at import time and the WebView shows a blank window. `tsc` cannot see this,
 * so the cycle set is pinned to a baseline here instead.
 *
 * The gate compares *module-level* strongly-connected components. The baseline
 * is empty — every cycle was broken in September 2026 — so any import cycle,
 * across features or inside one, fails. A module that needs its feature redrawn
 * requests it through a registered hook instead (signalPath/render.ts,
 * presets/refresh.ts, toneSharingPanel/refresh.ts). Should a tangle ever have to
 * be accepted, --update pins it, and from then on it may shrink but not grow.
 *
 *   node scripts/check-cycles.js            # verify against the baseline
 *   node scripts/check-cycles.js --update   # re-pin the baseline (deliberate)
 *   node scripts/check-cycles.js --list     # print every cycle, no gate
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const TS_DIR = path.join(ROOT, 'ts');
const BASELINE = path.join(__dirname, 'cycles-baseline.json');

const toPosix = (p) => p.split(path.sep).join('/');

function listSources(dir, acc = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) listSources(full, acc);
    else if (entry.name.endsWith('.ts') && !entry.name.endsWith('.d.ts')) acc.push(full);
  }
  return acc;
}

/** Module id relative to ts/, without extension: "signalPath", "amp3d/ampView". */
function moduleId(file) {
  return toPosix(path.relative(TS_DIR, file)).replace(/\.ts$/, '');
}

/** Feature bucket used by the gate: "signalPath/mixer" -> "signalPath". */
function feature(id) {
  return id.split('/')[0];
}

function buildGraph() {
  const graph = new Map();
  for (const file of listSources(TS_DIR)) {
    // Comments are stripped first — a module path mentioned in prose is not an
    // import. `verbatimModuleSyntax` is on, so `import type` / `export type` are
    // erased wholesale and cannot produce a runtime cycle either.
    const source = fs
      .readFileSync(file, 'utf8')
      .replace(/\/\*[\s\S]*?\*\//g, '')
      .replace(/^\s*\/\/.*$/gm, '')
      .replace(/^\s*(?:import|export)\s+type\s[\s\S]*?["'][^"']+["'];?$/gm, '');
    const deps = new Set();
    // Matches `from "./x.js"`, `import "./x.js"` and `import("./x.js")`.
    const re = /(?:from|import)\s*\(?\s*["'](\.[^"']+)["']/g;
    let match;
    while ((match = re.exec(source))) {
      const target = path.resolve(path.dirname(file), match[1].replace(/\.js$/, ''));
      deps.add(toPosix(path.relative(TS_DIR, target)));
    }
    graph.set(moduleId(file), [...deps].sort());
  }
  // Drop edges to things that are not modules we scanned (type-only .d.ts etc).
  for (const [id, deps] of graph) {
    graph.set(id, deps.filter((d) => graph.has(d)));
  }
  return graph;
}

/** Tarjan's algorithm. Returns SCCs with more than one member, plus self-loops. */
function stronglyConnectedComponents(graph) {
  const index = new Map();
  const low = new Map();
  const onStack = new Set();
  const stack = [];
  const components = [];
  let counter = 0;

  const nodes = [...graph.keys()].sort();

  function strongConnect(node) {
    index.set(node, counter);
    low.set(node, counter);
    counter += 1;
    stack.push(node);
    onStack.add(node);

    for (const dep of graph.get(node) || []) {
      if (!index.has(dep)) {
        strongConnect(dep);
        low.set(node, Math.min(low.get(node), low.get(dep)));
      } else if (onStack.has(dep)) {
        low.set(node, Math.min(low.get(node), index.get(dep)));
      }
    }

    if (low.get(node) === index.get(node)) {
      const component = [];
      let member;
      do {
        member = stack.pop();
        onStack.delete(member);
        component.push(member);
      } while (member !== node);

      const isSelfLoop = component.length === 1 && (graph.get(node) || []).includes(node);
      if (component.length > 1 || isSelfLoop) components.push(component.sort());
    }
  }

  for (const node of nodes) if (!index.has(node)) strongConnect(node);
  return components.sort((a, b) => a[0].localeCompare(b[0]));
}

/** Collapse SCCs to their distinct feature buckets, to say which features a failure ties together. */
function featureGroups(components) {
  return components
    .map((members) => [...new Set(members.map(feature))].sort())
    .filter((features) => features.length > 1)
    .map((features) => features.join(' + '))
    .sort();
}

function main() {
  const args = process.argv.slice(2);
  const graph = buildGraph();
  const components = stronglyConnectedComponents(graph);

  if (args.includes('--list')) {
    console.log(`${graph.size} modules scanned.`);
    for (const members of components) {
      console.log(`\n  tangle (${members.length} modules):`);
      for (const member of members) console.log(`    ${member}`);
    }
    return;
  }

  if (args.includes('--update')) {
    fs.writeFileSync(BASELINE, `${JSON.stringify({ moduleTangles: components }, null, 2)}\n`, 'utf8');
    console.log(`[check-cycles] baseline re-pinned: ${components.length} import cycle(s).`);
    for (const members of components) console.log(`  ${members.join(' + ')}`);
    return;
  }

  if (!fs.existsSync(BASELINE)) {
    console.error('[check-cycles] no baseline found. Run: node scripts/check-cycles.js --update');
    process.exitCode = 1;
    return;
  }

  const expected = JSON.parse(fs.readFileSync(BASELINE, 'utf8')).moduleTangles ?? [];
  const baselineSets = expected.map((members) => new Set(members));

  // A cycle is acceptable when it is a subset of one the baseline already
  // accepted: unchanged, or smaller because a module was untangled. Anything
  // that grows a cycle, or forms a fresh one, is a regression.
  const failures = [];
  for (const current of components) {
    const covering = baselineSets.find((known) => current.every((member) => known.has(member)));
    if (!covering) {
      const nearest = baselineSets
        .map((known) => ({ known, extra: current.filter((m) => !known.has(m)) }))
        .sort((a, b) => a.extra.length - b.extra.length)[0];
      failures.push({ current, newcomers: nearest ? nearest.extra : current });
    }
  }

  if (failures.length > 0) {
    console.error(`[check-cycles] FAIL — ${failures.length} import cycle(s) grew or appeared:`);
    for (const { current, newcomers } of failures) {
      console.error(`  newly entangled: ${newcomers.join(', ')}`);
      console.error(`  full cycle:      ${current.join(' + ')}`);
    }
    const crossFeature = featureGroups(failures.map((failure) => failure.current));
    if (crossFeature.length > 0) console.error(`  across features: ${crossFeature.join('; ')}`);
    console.error(
      '\nBreak the cycle — have the lower module request the work through a registered hook, or pass ' +
        'the callback in — or re-pin deliberately with: npm run check:cycles -- --update'
    );
    process.exitCode = 1;
    return;
  }

  const baselineSize = expected.reduce((total, members) => total + members.length, 0);
  const currentSize = components.reduce((total, members) => total + members.length, 0);

  console.log(
    `[check-cycles] OK — ${currentSize} module(s) in ${components.length} import cycle(s), none new ` +
      `(${graph.size} modules scanned).`
  );

  if (currentSize < baselineSize) {
    console.log(
      `Improvement: ${baselineSize - currentSize} module(s) untangled since the baseline. ` +
        'Tighten it with: npm run check:cycles -- --update'
    );
  }
}

main();
