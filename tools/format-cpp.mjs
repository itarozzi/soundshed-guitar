#!/usr/bin/env node
/**
 * Formats our C++ to the repo standard: uncrustify (tools/uncrustify.cfg) for the blank lines
 * around control flow, then clang-format (the repo-root .clang-format) for everything else, as
 * .github/copilot-instructions.md describes. This file is the one place that says which C++ is
 * ours and which tool versions the standard is defined by.
 *
 *   node tools/format-cpp.mjs                  # format every file in scope
 *   node tools/format-cpp.mjs --changed        # only files changed against main, and new ones
 *   node tools/format-cpp.mjs <files...>       # only these (files out of scope are skipped)
 *   node tools/format-cpp.mjs --check [...]    # change nothing; list what is off the standard
 *                                              # and fail (the CI gate, cpp-structure.yml)
 *   node tools/format-cpp.mjs --pin <tool>     # print the pinned version (CI installs it)
 *
 * The tools are found through CLANG_FORMAT and UNCRUSTIFY, then PATH, then (clang-format only)
 * Visual Studio's copy. Another version formats a little differently, so both must be the pinned
 * one: `pip install clang-format==<pin>`, and uncrustify built or installed at its release tag.
 * A version bump is deliberate: change the pin, run this over the tree, commit both together.
 *
 * A file is run through both tools until it stops changing (one whose namespace is split across
 * #if branches takes two passes). Its line endings are kept.
 */

import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

const PINS = { 'clang-format': '22.1.3', uncrustify: '0.83.0' };

// Our C++. Vendored code keeps its own style and is never formatted: the JUCE submodule,
// juce/modules (clap-juce-extensions) and juce/ASIOSDK lie outside these folders anyway.
const SCOPE = [
  'core/src/',
  'core/tests/',
  'core/config/',
  'nanoq/src/',
  'nanoq/tests/',
  'juce/source/',
  'juce/tests/',
  'juce/benchmarks/',
  'tools/nam-benchmark/',
  'tools/nano-fonts/',
];
const EXCLUDE = new Set(['core/src/compat/json.hpp']);
// Objective-C++ (.mm) is left out: the root .clang-format has no ObjC section.
const EXTENSIONS = /\.(cpp|cc|h|hpp)$/;

const MAX_PASSES = 4;

function inScope(file) {
  return SCOPE.some((dir) => file.startsWith(dir)) && EXTENSIONS.test(file) && !EXCLUDE.has(file);
}

function git(...args) {
  return execFileSync('git', args, { cwd: ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean);
}

function allFiles() {
  return git('ls-files', '--', ...SCOPE).filter(inScope);
}

/** Files changed since this branch left main, uncommitted changes, and new untracked files. */
function changedFiles() {
  let base = null;
  for (const ref of ['origin/main', 'main']) {
    try {
      base = git('merge-base', 'HEAD', ref)[0];
      break;
    } catch {
      // try the next
    }
  }
  const files = new Set([
    ...(base ? git('diff', '--name-only', '--diff-filter=d', base, 'HEAD') : []),
    ...git('diff', '--name-only', '--diff-filter=d', 'HEAD'),
    ...git('ls-files', '--others', '--exclude-standard', '--', ...SCOPE),
  ]);
  return [...files].filter(inScope).sort();
}

function findTool(name, envVar) {
  const candidates = [];
  if (process.env[envVar]) candidates.push(process.env[envVar]);
  candidates.push(name);
  if (name === 'clang-format' && process.platform === 'win32') {
    // Visual Studio ships one: VC/Tools/Llvm/x64/bin under each installed edition.
    for (const base of ['C:/Program Files/Microsoft Visual Studio', 'C:/Program Files (x86)/Microsoft Visual Studio']) {
      if (!fs.existsSync(base)) continue;
      for (const year of fs.readdirSync(base)) {
        const yearDir = path.join(base, year);
        if (!fs.statSync(yearDir).isDirectory()) continue;
        for (const edition of fs.readdirSync(yearDir)) {
          const exe = path.join(yearDir, edition, 'VC/Tools/Llvm/x64/bin/clang-format.exe');
          if (fs.existsSync(exe)) candidates.push(exe);
        }
      }
    }
  }
  const found = [];
  for (const exe of candidates) {
    let version;
    try {
      version = execFileSync(exe, ['--version'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    } catch {
      continue;
    }
    const match = version.match(/(\d+\.\d+\.\d+)/);
    const v = match ? match[1] : version.trim();
    if (v === PINS[name]) return exe;
    found.push(`${exe} (${v})`);
  }
  const hint =
    name === 'clang-format'
      ? `pip install clang-format==${PINS[name]}`
      : `build uncrustify at its tag uncrustify-${PINS[name]}, or install that release`;
  throw new Error(
    `${name} ${PINS[name]} not found${found.length ? `; found ${found.join(', ')}` : ''}. ` +
      `Another version formats differently. Set ${envVar} to it, or: ${hint}`,
  );
}

/** Runs `exe` with `input` on stdin; resolves with stdout (bytes as latin1, so nothing is re-encoded). */
function run(exe, args, input) {
  return new Promise((resolve, reject) => {
    const child = spawn(exe, args, { cwd: ROOT, stdio: ['pipe', 'pipe', 'pipe'] });
    const out = [];
    const err = [];
    child.stdout.on('data', (chunk) => out.push(chunk));
    child.stderr.on('data', (chunk) => err.push(chunk));
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) resolve(Buffer.concat(out).toString('latin1'));
      else reject(new Error(`${path.basename(exe)} exited ${code}: ${Buffer.concat(err).toString().trim()}`));
    });
    child.stdin.end(Buffer.from(input, 'latin1'));
  });
}

async function format(tools, file, text) {
  let current = text;
  for (let pass = 0; pass < MAX_PASSES; pass++) {
    const unc = await run(tools.uncrustify, ['-q', '-c', 'tools/uncrustify.cfg', '-l', 'CPP'], current);
    const next = (await run(tools.clangFormat, ['--style=file', `--assume-filename=${file}`], unc)).replace(/\r\n/g, '\n');
    if (next === current) return current;
    current = next;
  }
  throw new Error(`${file}: still changing after ${MAX_PASSES} passes`);
}

async function main() {
  const args = process.argv.slice(2);
  const pinAt = args.indexOf('--pin');
  if (pinAt >= 0) {
    const tool = args[pinAt + 1];
    if (!PINS[tool]) throw new Error(`--pin takes one of: ${Object.keys(PINS).join(', ')}`);
    console.log(PINS[tool]);
    return;
  }
  const check = args.includes('--check');
  const changed = args.includes('--changed');
  const named = args.filter((a) => !a.startsWith('--')).map((f) => path.relative(ROOT, path.resolve(f)).replaceAll('\\', '/'));

  let files;
  if (named.length > 0) {
    files = named.filter(inScope);
    for (const f of named.filter((f) => !inScope(f))) console.log(`skipped (not our C++): ${f}`);
  } else {
    files = changed ? changedFiles() : allFiles();
  }
  if (files.length === 0) {
    console.log('No C++ files to format.');
    return;
  }

  const tools = { clangFormat: findTool('clang-format', 'CLANG_FORMAT'), uncrustify: findTool('uncrustify', 'UNCRUSTIFY') };

  const off = [];
  const failures = [];
  let next = 0;
  const worker = async () => {
    while (next < files.length) {
      const file = files[next++];
      const full = path.join(ROOT, file);
      try {
        const original = fs.readFileSync(full, 'latin1');
        const lf = original.replace(/\r\n/g, '\n');
        const formatted = await format(tools, file, lf);
        if (formatted === lf) continue;
        off.push(file);
        if (!check) fs.writeFileSync(full, original.includes('\r\n') ? formatted.replace(/\n/g, '\r\n') : formatted, 'latin1');
      } catch (error) {
        failures.push(`${file}: ${error.message}`);
      }
    }
  };
  await Promise.all(Array.from({ length: Math.min(8, os.cpus().length, files.length) }, worker));

  off.sort();
  for (const failure of failures) console.error(`error: ${failure}`);
  if (check) {
    for (const file of off) console.log(`off the standard: ${file}`);
    console.log(`${off.length} of ${files.length} files off the standard.`);
    if (off.length > 0) console.log('Fix with: node tools/format-cpp.mjs <files>, or --changed for everything you touched.');
    if (off.length > 0 || failures.length > 0) process.exitCode = 1;
  } else {
    for (const file of off) console.log(`formatted: ${file}`);
    console.log(`${off.length} of ${files.length} files formatted.`);
    if (failures.length > 0) process.exitCode = 1;
  }
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
