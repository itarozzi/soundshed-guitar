#!/usr/bin/env node
/**
 * File-size budget for the UI source tree.
 *
 * A file over the budget is not a bug, but it is where bugs hide: the panel
 * builders and message switches that grew past a screenful are the ones nobody
 * can review. New files must come in under budget; the known offenders are
 * listed explicitly below and the list is only ever allowed to shrink.
 *
 *   node scripts/check-file-sizes.js            # verify
 *   node scripts/check-file-sizes.js --update   # tighten pins to the files' current sizes
 *   node scripts/check-file-sizes.js --repin    # re-pin everything, raising pins too
 *
 * The list only moves one way. `--update` lowers a pin when its file has shrunk and drops
 * files that are gone or back under budget; it never raises one or adds a file. A pin more
 * than STALE_SLACK above its file's size fails the check, so a change that shrinks a file
 * tightens its pin with it and the file cannot quietly grow back. `--repin` is the
 * deliberate exception, for a file that has to grow.
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const ALLOWLIST = path.join(__dirname, 'file-size-allowlist.json');

const BUDGET = 800;

/** How far a pin may sit above its file before the check calls it stale. */
const STALE_SLACK = 0.05;

const TARGETS = [
  { dir: path.join(ROOT, 'ts'), ext: '.ts' },
  { dir: path.join(ROOT, 'css'), ext: '.css' },
  { dir: path.join(ROOT, 'ui-components'), ext: '.html' },
];

const toPosix = (p) => p.split(path.sep).join('/');

function listFiles(dir, ext, acc = []) {
  if (!fs.existsSync(dir)) return acc;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) listFiles(full, ext, acc);
    else if (entry.name.endsWith(ext)) acc.push(full);
  }
  return acc;
}

function measure() {
  const sizes = new Map();
  for (const { dir, ext } of TARGETS) {
    for (const file of listFiles(dir, ext)) {
      const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/).length;
      sizes.set(toPosix(path.relative(ROOT, file)), lines);
    }
  }
  return sizes;
}

function main() {
  const sizes = measure();
  const over = [...sizes.entries()].filter(([, lines]) => lines > BUDGET).sort((a, b) => b[1] - a[1]);

  if (process.argv.includes('--update') && fs.existsSync(ALLOWLIST)) {
    const previous = JSON.parse(fs.readFileSync(ALLOWLIST, 'utf8')).allowed;
    const allowed = Object.fromEntries(
      over.filter(([file]) => previous[file] !== undefined).map(([file, lines]) => [file, Math.min(previous[file], lines)])
    );
    fs.writeFileSync(ALLOWLIST, `${JSON.stringify({ budget: BUDGET, allowed }, null, 2)}\n`, 'utf8');
    console.log(`[check-file-sizes] allowlist tightened: ${Object.keys(allowed).length} file(s) pinned over ${BUDGET} lines.`);
    return;
  }

  if (process.argv.includes('--repin') || process.argv.includes('--update')) {
    const allowed = Object.fromEntries(over);
    fs.writeFileSync(ALLOWLIST, `${JSON.stringify({ budget: BUDGET, allowed }, null, 2)}\n`, 'utf8');
    console.log(`[check-file-sizes] allowlist re-pinned: ${over.length} file(s) over ${BUDGET} lines.`);
    return;
  }

  if (!fs.existsSync(ALLOWLIST)) {
    console.error('[check-file-sizes] no allowlist found. Run: node scripts/check-file-sizes.js --update');
    process.exitCode = 1;
    return;
  }

  const { allowed } = JSON.parse(fs.readFileSync(ALLOWLIST, 'utf8'));
  const failures = [];
  const improvements = [];
  const stale = [];

  for (const [file, lines] of over) {
    const ceiling = allowed[file];
    if (ceiling === undefined) failures.push(`${file} — ${lines} lines, budget is ${BUDGET} (new file over budget)`);
    else if (lines > ceiling) failures.push(`${file} — grew to ${lines} lines, was ${ceiling}`);
  }

  for (const [file, ceiling] of Object.entries(allowed)) {
    const lines = sizes.get(file);
    if (lines === undefined) stale.push(`${file} — gone, still pinned at ${ceiling}`);
    else if (lines <= BUDGET) stale.push(`${file} — now ${lines} lines, under the budget, and still pinned`);
    else if (ceiling > lines * (1 + STALE_SLACK)) stale.push(`${file} — ${lines} lines, pinned at ${ceiling}`);
    else if (lines < ceiling) improvements.push(`${file} — ${ceiling} to ${lines} lines`);
  }

  for (const note of improvements) console.log(`[check-file-sizes] improved: ${note}`);

  if (stale.length > 0) {
    console.error(`[check-file-sizes] FAIL — ${stale.length} pin(s) looser than the files need; a file could grow back unnoticed:`);
    for (const note of stale) console.error(`  ${note}`);
    console.error('\nTighten them: npm run check:sizes -- --update');
    process.exitCode = 1;
  }

  if (failures.length > 0) {
    console.error(`[check-file-sizes] FAIL — ${failures.length} file(s) over budget:`);
    for (const failure of failures) console.error(`  ${failure}`);
    console.error('\nSplit the file, or raise its pin deliberately with: npm run check:sizes -- --repin');
    process.exitCode = 1;
    return;
  }

  if (stale.length > 0) {
    return;
  }

  console.log(
    `[check-file-sizes] OK — ${sizes.size} files checked, ` +
      `${over.length} over the ${BUDGET}-line budget (all known).`
  );

  if (improvements.length > 0) {
    console.log('Allowlist can be tightened: npm run check:sizes -- --update');
  }
}

main();
