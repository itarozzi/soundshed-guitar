#!/usr/bin/env node
// Builds the NAM benchmark tools against the NAM core the app fetches, with the app's Release flags.
//
//   node tools/nam-benchmark/build.mjs [--clang] [--sse] [--define NAME[=VALUE]]... [--clean]
//
// Output: tools/nam-benchmark/build/<compiler>-<isa>/{bench,parts,invariance,loadtime,coverage,sensitivity}.exe
// Windows only (it drives vcvars64.bat). The NAM library is compiled once per output directory;
// --clean rebuilds it, which you want after the NAM pin moves.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..', '..');
const args = process.argv.slice(2);
const clang = args.includes('--clang');
const sse = args.includes('--sse');
const defines = [];
for (let i = 0; i < args.length; ++i) if (args[i] === '--define') defines.push(args[++i]);

const fwd = (p) => p.replace(/\\/g, '/');

function findVs() {
  if (process.env.SSG_VS_ROOT) return process.env.SSG_VS_ROOT;
  const vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe';
  if (existsSync(vswhere)) {
    // -requires, or the newest install may be one with no C++ tools (SQL Server Management Studio, say).
    const query = ['-latest', '-prerelease', '-products', '*', '-property', 'installationPath'];
    const r = spawnSync(vswhere, [...query, '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'], {
      encoding: 'utf8',
    });
    const p = r.stdout.trim().split(/\r?\n/)[0];
    if (p) return p;
  }
  throw new Error('Visual Studio not found: set SSG_VS_ROOT to its installation folder');
}

// The fetched sources from whichever build tree exists: the core's, or the JUCE app's.
function findDeps() {
  for (const d of ['core/build/_deps', 'juce/builds/_deps']) {
    const p = join(repo, d);
    if (existsSync(join(p, 'neuralampmodelercore-src', 'NAM', 'dsp.h'))) return p;
  }
  throw new Error('No fetched NAM core: configure core/build or juce/builds first');
}

const vs = findVs();
const deps = findDeps();
const nam = fwd(join(deps, 'neuralampmodelercore-src'));
const out = join(here, 'build', `${clang ? 'clang' : 'msvc'}-${sse ? 'sse' : 'avx2'}`);
if (args.includes('--clean')) rmSync(out, { recursive: true, force: true });
mkdirSync(out, { recursive: true });

// The core's Release flags (guitarfx_apply_release_optimizations), NAM_ENABLE_A2_FAST as the core sets it.
const comp = clang
  ? `"${fwd(vs)}/VC/Tools/Llvm/x64/bin/clang-cl.exe" /O2 /clang:-O3 /clang:-ffast-math ${sse ? '-msse4.2' : '-mavx2 -mfma'} /MT`
  : `cl /Ox /fp:fast ${sse ? '' : '/arch:AVX2'} /MT`;
const inc = [
  here,
  join(repo, 'core/src/compat'),
  nam,
  `${nam}/Dependencies/eigen`,
  join(deps, 'nlohmann_json-src/include'),
]
  .map((p) => `/I "${fwd(p)}"`)
  .join(' ');
const defs = ['NAM_ENABLE_A2_FAST=1', 'NDEBUG', '_CRT_SECURE_NO_WARNINGS', ...defines].map((d) => `/D${d}`).join(' ');
const common = `${comp} /nologo /std:c++20 /EHsc ${defs} ${inc}`;

const lines = ['@echo off', `call "${fwd(vs)}/VC/Auxiliary/Build/vcvars64.bat" >nul 2>&1`, `cd /d "${fwd(out)}"`];
if (!existsSync(join(out, 'nam.lib'))) {
  lines.push(
    `${common} /c "${nam}/NAM/*.cpp" "${nam}/NAM/wavenet/*.cpp" "${fwd(join(repo, 'core/src/dsp/NAMFactoryForce.cpp'))}" || exit /b 1`,
    'lib /nologo /out:nam.lib *.obj || exit /b 1',
    'del *.obj',
  );
}
for (const tool of ['bench', 'parts', 'invariance', 'loadtime', 'coverage', 'sensitivity'])
  lines.push(`${common} "${fwd(join(here, tool + '.cpp'))}" nam.lib avrt.lib /Fe:${tool}.exe || exit /b 1`);
const bat = join(out, 'build.bat');
writeFileSync(bat, lines.join('\r\n') + '\r\n');

const r = spawnSync('cmd.exe', ['/c', bat], { encoding: 'utf8' });
const log = (r.stdout || '') + (r.stderr || '');
const problems = log.split(/\r?\n/).filter((l) => /error|warning C4(?!996)|D9002/i.test(l));
if (problems.length) console.log(problems.join('\n'));
if (r.status !== 0) {
  console.error(log);
  console.error(`build failed (${r.status}${r.error ? ': ' + r.error.message : ''})`);
  process.exit(1);
}
console.log(`built ${out}`);
