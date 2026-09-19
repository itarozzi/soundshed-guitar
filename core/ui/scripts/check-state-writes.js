#!/usr/bin/env node
/**
 * Who writes each part of `uiState`, pinned so it can only shrink.
 *
 * `uiState` (ts/state.ts) is one mutable object that any module can import and
 * assign into. When a dozen modules write the same field, none of them owns
 * it: a value can change underneath the code that rendered it, and a rule
 * such as "record it locally and send it to the engine" gets remembered in
 * some places and forgotten in others. The fix is a feature-owned slice — one
 * module with commands to change it, like ts/appSettingsStore.ts — and this
 * gate is what keeps a migrated slice migrated.
 *
 * For every top-level field it records which modules write it, and fails when
 * a module writes a field it is not pinned for. Moving the writes behind an
 * owner shrinks the pin; --update then tightens it.
 *
 * A write is an assignment (`=`, `+=`, `++`...), a `delete`, or a mutating call
 * (`push`, `set`, `splice`...) on `uiState.<field>` or anything below it. A
 * `setAppSetting(` call counts as a write to `appSettings`: sending a setting
 * without recording it is how the UI's copy used to drift. Writes through an
 * alias (`const mixer = uiState.mixer; mixer.x = 1`) are not seen.
 *
 *   node scripts/check-state-writes.js            # verify against the baseline
 *   node scripts/check-state-writes.js --update   # re-pin the baseline (deliberate)
 *   node scripts/check-state-writes.js --list     # print writers per field, no gate
 *   node scripts/check-state-writes.js --where presets,mixer   # every write site of those fields
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const TS_DIR = path.join(ROOT, 'ts');
const BASELINE = path.join(__dirname, 'state-writers-baseline.json');

const toPosix = (p) => p.split(path.sep).join('/');

function listSources(dir, acc = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) listSources(full, acc);
    else if (entry.name.endsWith('.ts') && !entry.name.endsWith('.d.ts')) acc.push(full);
  }
  return acc;
}

const MUTATORS = 'push|pop|shift|unshift|splice|sort|reverse|fill|set|delete|clear|add';
// uiState.field, then any chain of .member / [index] / ?.member below it.
const TARGET = String.raw`uiState\.([A-Za-z_$][\w$]*)(?:\??\.[A-Za-z_$][\w$]*|\??\.?\[[^\]\n]+\])*`;
const WRITE_PATTERNS = [
  new RegExp(String.raw`(?<![\w$.])${TARGET}\s*(?:[-+*/%&|^]|\?\?|\|\||&&|<<|>>>?|\*\*)?=(?!=)`, 'g'),
  new RegExp(String.raw`(?<![\w$.])${TARGET}\s*(?:\+\+|--)`, 'g'),
  new RegExp(String.raw`(?:\+\+|--)\s*${TARGET}`, 'g'),
  new RegExp(String.raw`\bdelete\s+${TARGET}`, 'g'),
  new RegExp(String.raw`(?<![\w$.])${TARGET}\??\.(?:${MUTATORS})\(`, 'g'),
];
const SETTING_SEND = /(?<![\w$.])setAppSetting\s*\(/g;

/** Blanks comments without moving anything, so match offsets still give line numbers. */
function stripComments(text) {
  const blank = (m) => m.replace(/[^\n]/g, ' ');
  return text.replace(/\/\*[\s\S]*?\*\//g, blank).replace(/^\s*\/\/.*$/gm, blank);
}

/** Map of field -> Set of module ids that write it; `sites` collects "id:line" per field. */
function collectWriters(sites = null) {
  const writers = new Map();
  const note = (field, id, source, index) => {
    if (!writers.has(field)) writers.set(field, new Set());
    writers.get(field).add(id);
    if (sites) {
      if (!sites.has(field)) sites.set(field, []);
      sites.get(field).push(`${id}:${source.slice(0, index).split('\n').length}`);
    }
  };
  for (const file of listSources(TS_DIR)) {
    const id = toPosix(path.relative(TS_DIR, file));
    const source = stripComments(fs.readFileSync(file, 'utf8'));
    for (const pattern of WRITE_PATTERNS) {
      pattern.lastIndex = 0;
      let match;
      while ((match = pattern.exec(source))) note(match[1], id, source, match.index);
    }
    const sends = source.replace(/function(\s+)setAppSetting(\s*)\(/g, 'function$1_etAppSetting$2(');
    SETTING_SEND.lastIndex = 0;
    let send;
    while ((send = SETTING_SEND.exec(sends))) note('appSettings', id, sends, send.index);
  }
  return writers;
}

function toPlain(writers) {
  const out = {};
  for (const field of [...writers.keys()].sort()) out[field] = [...writers.get(field)].sort();
  return out;
}

function main() {
  const args = process.argv.slice(2);
  const whereIndex = args.indexOf('--where');
  if (whereIndex >= 0) {
    const sites = new Map();
    collectWriters(sites);
    for (const field of (args[whereIndex + 1] ?? '').split(',').filter(Boolean)) {
      console.log(`uiState.${field}:`);
      for (const site of sites.get(field) ?? []) console.log(`  ${site}`);
    }
    return;
  }

  const current = toPlain(collectWriters());

  if (args.includes('--list')) {
    const rows = Object.entries(current).sort((a, b) => b[1].length - a[1].length || a[0].localeCompare(b[0]));
    for (const [field, modules] of rows) console.log(`${field.padEnd(26)} ${String(modules.length).padStart(3)}  ${modules.join(' ')}`);
    return;
  }

  if (args.includes('--update')) {
    fs.writeFileSync(BASELINE, `${JSON.stringify(current, null, 2)}\n`, 'utf8');
    const pairs = Object.values(current).reduce((total, modules) => total + modules.length, 0);
    console.log(`[check-state-writes] baseline re-pinned: ${pairs} (field, module) write pair(s) across ${Object.keys(current).length} field(s).`);
    return;
  }

  if (!fs.existsSync(BASELINE)) {
    console.error('[check-state-writes] no baseline found. Run: node scripts/check-state-writes.js --update');
    process.exitCode = 1;
    return;
  }

  const expected = JSON.parse(fs.readFileSync(BASELINE, 'utf8'));
  const failures = [];
  let removed = 0;
  for (const [field, modules] of Object.entries(current)) {
    const allowed = new Set(expected[field] ?? []);
    for (const id of modules) if (!allowed.has(id)) failures.push({ field, id, owners: expected[field] ?? [] });
  }
  for (const [field, modules] of Object.entries(expected)) {
    const now = new Set(current[field] ?? []);
    removed += modules.filter((id) => !now.has(id)).length;
  }

  if (failures.length > 0) {
    console.error(`[check-state-writes] FAIL — ${failures.length} new write(s) into uiState:`);
    for (const { field, id, owners } of failures) {
      const hint = owners.length === 1 ? ` (owned by ${owners[0]} — call its commands instead)` : owners.length === 0 ? ' (new field — give it one owner)' : '';
      console.error(`  ${id} writes uiState.${field}${hint}`);
    }
    console.error('\nRoute the write through the module that owns that state, or re-pin deliberately with: npm run check:state-writes -- --update');
    process.exitCode = 1;
    return;
  }

  const pairs = Object.values(current).reduce((total, modules) => total + modules.length, 0);
  console.log(`[check-state-writes] OK — ${pairs} (field, module) write pair(s) across ${Object.keys(current).length} field(s), none new.`);
  if (removed > 0) {
    console.log(`Improvement: ${removed} write pair(s) gone since the baseline. Tighten it with: npm run check:state-writes -- --update`);
  }
}

main();
