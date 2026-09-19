#!/usr/bin/env node
/**
 * Every dependency the build fetches must name an exact revision.
 *
 * FetchContent with `GIT_TAG main` rebuilds against whatever the upstream branch
 * holds that day, so two clean builds of the same commit can differ, and a
 * regression can arrive without a change in this repository. This check fails
 * when a FetchContent_Declare in our own CMake files tracks a branch instead of a
 * commit SHA or a release tag.
 *
 *   node tools/check-dependency-pins.mjs              # gate: fail on a moving pin
 *   node tools/check-dependency-pins.mjs --upstream   # report which pins have newer
 *                                                     # upstream commits or tags
 *
 * Bumping a pin is deliberate: change the SHA or tag in CMake, rebuild, run the tests.
 * A downloaded archive (URL) should carry URL_HASH; one that does not is reported but
 * not failed, since a versioned release path does not change under us either.
 *
 * Vendored third-party trees (juce/JUCE, juce/modules, juce/ASIOSDK, Dependencies)
 * are not ours to pin and are not scanned.
 */

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SCANNED = ['core/CMakeLists.txt', 'core/cmake', 'juce/CMakeLists.txt', 'android/CMakeLists.txt', 'android/app/CMakeLists.txt'];

const SHA = /^[0-9a-f]{40}$/i;
// A release tag: an optional v, then dotted numbers, optionally a pre-release suffix.
const RELEASE_TAG = /^v?\d+(\.\d+)*([-+][0-9A-Za-z.-]+)?$/;

function cmakeFiles() {
  const files = [];
  for (const entry of SCANNED) {
    const full = path.join(ROOT, entry);
    if (!fs.existsSync(full)) continue;
    if (fs.statSync(full).isDirectory()) {
      for (const name of fs.readdirSync(full).sort()) {
        if (name.endsWith('.cmake') || name === 'CMakeLists.txt') files.push(path.join(full, name));
      }
    } else {
      files.push(full);
    }
  }
  return files;
}

/** Every FetchContent_Declare(...) in a file, with the fields that decide what gets fetched. */
function declarations(file) {
  const text = fs.readFileSync(file, 'utf8').replace(/#[^\n]*/g, '');
  const found = [];
  const re = /FetchContent_Declare\s*\(/g;
  let match;
  while ((match = re.exec(text))) {
    let depth = 1;
    let i = re.lastIndex;
    while (i < text.length && depth > 0) {
      if (text[i] === '(') depth += 1;
      else if (text[i] === ')') depth -= 1;
      i += 1;
    }
    const body = text.slice(re.lastIndex, i - 1).trim();
    const tokens = body.match(/"[^"]*"|\S+/g) ?? [];
    const unquote = (t) => (t && t.startsWith('"') ? t.slice(1, -1) : t);
    const field = (name) => {
      const at = tokens.indexOf(name);
      return at >= 0 ? unquote(tokens[at + 1]) : undefined;
    };
    found.push({
      file: path.relative(ROOT, file).split(path.sep).join('/'),
      line: text.slice(0, match.index).split('\n').length,
      name: unquote(tokens[0]),
      repository: field('GIT_REPOSITORY'),
      tag: field('GIT_TAG'),
      url: field('URL'),
      urlHash: field('URL_HASH'),
    });
  }
  return found;
}

function gitLsRemote(args) {
  return execFileSync('git', ['ls-remote', ...args], { encoding: 'utf8', timeout: 30000 })
    .split('\n')
    .filter(Boolean)
    .map((line) => line.split('\t'));
}

function compareVersions(a, b) {
  const parts = (t) => t.replace(/^v/, '').split(/[.-]/).map((p) => (/^\d+$/.test(p) ? Number(p) : p));
  const pa = parts(a);
  const pb = parts(b);
  for (let i = 0; i < Math.max(pa.length, pb.length); i += 1) {
    const x = pa[i] ?? 0;
    const y = pb[i] ?? 0;
    if (x === y) continue;
    if (typeof x === 'number' && typeof y === 'number') return x - y;
    return String(x).localeCompare(String(y));
  }
  return 0;
}

function upstreamNote(dep) {
  try {
    if (SHA.test(dep.tag)) {
      const [[head]] = gitLsRemote([dep.repository, 'HEAD']);
      return head.toLowerCase() === dep.tag.toLowerCase() ? 'up to date with the default branch' : `default branch is now ${head.slice(0, 12)}`;
    }
    // Only tags shaped like the pinned one: a project that renamed its scheme (miniz went
    // from v115 to 3.0.2) would otherwise report its oldest release as the newest.
    const sameShape = (t) => t.startsWith('v') === dep.tag.startsWith('v') && t.includes('.') === dep.tag.includes('.');
    const tags = gitLsRemote(['--tags', '--refs', dep.repository])
      .map(([, ref]) => ref.replace('refs/tags/', ''))
      .filter((t) => RELEASE_TAG.test(t) && !/-/.test(t) && sameShape(t));
    const newest = tags.sort(compareVersions).at(-1);
    return newest && compareVersions(newest, dep.tag) > 0 ? `newest release tag is ${newest}` : 'newest release tag';
  } catch (error) {
    return `could not reach upstream (${error.message.split('\n')[0]})`;
  }
}

function main() {
  const upstream = process.argv.includes('--upstream');
  const deps = cmakeFiles().flatMap(declarations);
  const failures = [];
  const warnings = [];

  for (const dep of deps) {
    const where = `${dep.file}:${dep.line} ${dep.name}`;
    if (dep.repository) {
      if (!dep.tag) failures.push(`${where}: GIT_REPOSITORY with no GIT_TAG fetches the default branch`);
      else if (!SHA.test(dep.tag) && !RELEASE_TAG.test(dep.tag)) failures.push(`${where}: GIT_TAG ${dep.tag} is a branch, not a commit or release tag`);
    } else if (dep.url && !dep.urlHash) {
      warnings.push(`${where}: URL download without URL_HASH`);
    }
  }

  if (upstream) {
    for (const dep of deps.filter((d) => d.repository && d.tag)) {
      console.log(`${dep.name.padEnd(34)} ${dep.tag.slice(0, 12).padEnd(12)}  ${upstreamNote(dep)}`);
    }
  }

  for (const warning of warnings) console.log(`[check-dependency-pins] note: ${warning}`);

  if (failures.length > 0) {
    console.error(`[check-dependency-pins] FAIL — ${failures.length} dependency pin(s) can move:`);
    for (const failure of failures) console.error(`  ${failure}`);
    console.error('\nPin the commit the build was verified against (git -C <build>/_deps/<name>-src rev-parse HEAD).');
    process.exitCode = 1;
    return;
  }

  console.log(`[check-dependency-pins] OK — ${deps.length} fetched dependencies, every git one pinned to a commit or release tag.`);
}

main();
