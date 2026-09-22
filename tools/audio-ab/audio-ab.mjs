#!/usr/bin/env node
/**
 * Audio A/B between two versions of the app: does anything sound different?
 *
 * For each revision it builds core/tests/AudioSnapshot in Release (in a git worktree of its
 * own, so the base is built from exactly that commit with its own pinned dependencies),
 * renders every effect alone at its defaults and a set of whole chains through a real
 * PluginController at default settings (tools/audio-ab/snapshot.json), then compares the
 * two sets of renders and says which effects and chains changed audibly.
 *
 * The harness that renders both sides is always the one in this checkout: it is copied into
 * the base's worktree, so a difference can only come from the DSP, never from the harness.
 * The stimuli and test resources are read from this checkout too.
 *
 * Usage:
 *   node tools/audio-ab/audio-ab.mjs <base> [<target>]    # target defaults to WORKTREE
 *   node tools/audio-ab/audio-ab.mjs 1.5.0                # the public release vs your working tree
 *   node tools/audio-ab/audio-ab.mjs HEAD WORKTREE        # what your uncommitted changes do
 *   node tools/audio-ab/audio-ab.mjs 1.5.0 main --rate 44100 --signals di,di_hot
 *   node tools/audio-ab/audio-ab.mjs render <rev>         # build and render one side only
 *   node tools/audio-ab/audio-ab.mjs report <renderDirA> <renderDirB>
 *   node tools/audio-ab/audio-ab.mjs list                 # cached worktrees, builds and renders
 *   node tools/audio-ab/audio-ab.mjs clean [--all]        # remove worktrees and builds (--all: renders and reports too)
 *
 * A revision is anything git rev-parse accepts, or WORKTREE for this checkout as it stands,
 * uncommitted changes included.
 *
 * Options:
 *   --rate <hz>            render sample rate (default 48000)
 *   --block <n>            block size (default 64, a 48 kHz/64 ASIO rig)
 *   --signals a,b          stimuli: di, di_hot, sweep, impulse, silence, noise (default from snapshot.json)
 *   --filter <text>        only effects/chains whose name, alias or type contains <text>
 *   --only effects|chains  one pass only
 *   --definition <file>    another snapshot definition (default tools/audio-ab/snapshot.json)
 *   --work-dir <dir>       where worktrees, builds, renders and reports live
 *                          (default ../<repo>-audio-ab, or SSG_AUDIO_AB_DIR)
 *   --rerender             ignore cached renders
 *   --fail-on <verdict>    exit 1 if any subject is at least this bad: broken (default), audible, slight, never
 *   --threshold k=v        override a threshold in analysis.mjs, e.g. --threshold levelAudibleDb=1
 *
 * Renders of a committed revision are cached by commit, harness, definition and options, so
 * comparing the same base again only re-renders the working tree. Builds are incremental.
 * The first build of a revision fetches its dependencies and compiles the core: minutes.
 */

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { compareRuns, printSummary, writeHtml, writeJson, writeMarkdown } from "./report.mjs";
import { DEFAULT_THRESHOLDS, severityRank } from "./analysis.mjs";

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const IS_WIN = process.platform === "win32";
const HARNESS_FILES = ["AudioSnapshot.cpp", "AudioSnapshotChains.h", "AudioSnapshotSupport.h", "AudioSnapshot.cmake"];
const WORKTREE = "worktree";

// ---------------------------------------------------------------- arguments

function parseArgs(argv) {
  const opts = { positional: [], thresholds: {}, failOn: "broken" };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`${a} needs a value`);
      return argv[++i];
    };
    if (a === "--rate") opts.rate = Number(next());
    else if (a === "--block") opts.block = Number(next());
    else if (a === "--signals") opts.signals = next();
    else if (a === "--filter") opts.filter = next();
    else if (a === "--only") opts.only = next();
    else if (a === "--definition") opts.definition = path.resolve(next());
    else if (a === "--work-dir") opts.workDir = path.resolve(next());
    else if (a === "--rerender") opts.rerender = true;
    else if (a === "--fail-on") opts.failOn = next();
    else if (a === "--threshold") {
      const [k, v] = next().split("=");
      opts.thresholds[k] = Number(v);
    } else if (a === "--all") opts.all = true;
    else if (a === "--help" || a === "-h") opts.help = true;
    else if (a.startsWith("--")) throw new Error(`unknown option ${a}`);
    else opts.positional.push(a);
  }
  if (!["broken", "audible", "slight", "never"].includes(opts.failOn)) throw new Error(`--fail-on must be broken, audible, slight or never, not "${opts.failOn}"`);
  for (const [k, v] of Object.entries(opts.thresholds)) {
    if (!(k in DEFAULT_THRESHOLDS) || !Number.isFinite(v)) throw new Error(`unknown or non-numeric threshold "${k}" (see DEFAULT_THRESHOLDS in analysis.mjs)`);
  }
  opts.rate ??= 48000;
  opts.block ??= 64;
  opts.definition ??= path.join(REPO, "tools", "audio-ab", "snapshot.json");
  opts.workDir ??= process.env.SSG_AUDIO_AB_DIR ? path.resolve(process.env.SSG_AUDIO_AB_DIR) : path.resolve(REPO, "..", `${path.basename(REPO)}-audio-ab`);
  return opts;
}

// ---------------------------------------------------------------- helpers

const sha256 = (...parts) => {
  const h = createHash("sha256");
  for (const p of parts) h.update(p);
  return h.digest("hex");
};

function git(args, cwd = REPO) {
  const r = spawnSync("git", args, { cwd, encoding: "utf8" });
  if (r.status !== 0) throw new Error(`git ${args.join(" ")}: ${(r.stderr || r.stdout).trim()}`);
  return r.stdout.trim();
}

/** Runs a command with its output going to a log; returns the exit code. `onLine` sees each line. */
function run(cmd, args, { cwd, log, env, onLine } = {}) {
  return new Promise((resolve) => {
    fs.mkdirSync(path.dirname(log), { recursive: true });
    const out = fs.createWriteStream(log);
    out.write(`> ${cmd} ${args.join(" ")}\n`);
    const child = spawn(cmd, args, { cwd, env: env ?? process.env, windowsHide: true });
    let pending = "";
    const feed = (chunk) => {
      out.write(chunk);
      if (!onLine) return;
      pending += chunk.toString();
      const lines = pending.split(/\r?\n/);
      pending = lines.pop();
      lines.forEach(onLine);
    };
    child.stdout.on("data", feed);
    child.stderr.on("data", feed);
    child.on("error", (e) => { out.write(String(e)); out.end(() => resolve(-1)); });
    child.on("close", (code) => { if (pending && onLine) onLine(pending); out.end(() => resolve(code)); });
  });
}

function tailOf(log, pattern, count = 30) {
  const lines = fs.readFileSync(log, "utf8").split(/\r?\n/);
  const hits = pattern ? lines.filter((l) => pattern.test(l)) : [];
  return (hits.length ? hits : lines).slice(-count).join("\n");
}

const step = (msg) => console.log(`\n== ${msg}`);

// ---------------------------------------------------------------- revisions

function resolveRevision(spec, workDir) {
  if (/^(worktree|\.)$/i.test(spec)) {
    const sha = git(["rev-parse", "HEAD"]);
    const dirty = git(["status", "--porcelain", "--", "core"]).length > 0;
    return { spec, label: "working tree", key: WORKTREE, sha, describe: git(["describe", "--tags", "--always", "HEAD"]), dirty, srcDir: REPO };
  }
  const sha = git(["rev-parse", "--verify", `${spec}^{commit}`]);
  const key = sha.slice(0, 12);
  return { spec, label: spec, key, sha, describe: git(["describe", "--tags", "--always", sha]), dirty: false, srcDir: path.join(workDir, "src", key) };
}

function ensureWorktree(rev) {
  if (rev.key === WORKTREE) return;
  if (fs.existsSync(path.join(rev.srcDir, ".git"))) {
    const at = git(["rev-parse", "HEAD"], rev.srcDir);
    if (at === rev.sha) return;
    throw new Error(`${rev.srcDir} is a worktree at ${at}, not ${rev.sha}; run "clean" and try again`);
  }
  step(`checking out ${rev.label} (${rev.describe}) into ${rev.srcDir}`);
  fs.mkdirSync(path.dirname(rev.srcDir), { recursive: true });
  git(["worktree", "add", "--detach", rev.srcDir, rev.sha]);
}

/**
 * Puts this checkout's harness into the revision's tree, and includes its target from that
 * tree's core/tests/CMakeLists.txt if that file does not already. Files are only rewritten
 * when they differ, so an unchanged harness does not trigger a rebuild.
 */
function injectHarness(rev) {
  if (rev.key === WORKTREE) return;
  const testsDir = path.join(rev.srcDir, "core", "tests");
  for (const name of HARNESS_FILES) {
    const src = fs.readFileSync(path.join(REPO, "core", "tests", name));
    const dst = path.join(testsDir, name);
    if (!fs.existsSync(dst) || !fs.readFileSync(dst).equals(src)) fs.writeFileSync(dst, src);
  }
  const lists = path.join(testsDir, "CMakeLists.txt");
  const text = fs.readFileSync(lists, "utf8");
  if (!text.includes("AudioSnapshot.cmake")) {
    fs.writeFileSync(lists, `${text.replace(/\s*$/, "\n")}\n# Added by tools/audio-ab.\ninclude(AudioSnapshot.cmake)\n`);
  }
}

/** The FetchContent pins a tree declares, so two trees with the same pins can share sources. */
function dependencyPins(read) {
  const files = ["core/CMakeLists.txt", ...["GuitarfxAudioDSPTools.cmake", "GuitarfxIPP.cmake"].map((f) => `core/cmake/${f}`)];
  return files.map((f) => (read(f) ?? "").split(/\r?\n/).filter((l) => /\b(GIT_REPOSITORY|GIT_TAG|URL|URL_HASH|GUITARFX_WASMTIME_VERSION)\b/.test(l)).map((l) => l.trim()).join("\n")).join("\n--\n");
}

function sharedDepsArgs(rev) {
  const deps = path.join(REPO, "core", "build", "_deps");
  if (!fs.existsSync(deps)) return [];
  if (rev.key !== WORKTREE) {
    const ours = dependencyPins((f) => (fs.existsSync(path.join(REPO, f)) ? fs.readFileSync(path.join(REPO, f), "utf8") : null));
    const theirs = dependencyPins((f) => { try { return git(["show", `${rev.sha}:${f}`]); } catch { return null; } });
    if (ours !== theirs) return [];
  }
  const args = ["-DFETCHCONTENT_FULLY_DISCONNECTED=ON"];
  for (const d of fs.readdirSync(deps).filter((d) => d.endsWith("-src"))) {
    args.push(`-DFETCHCONTENT_SOURCE_DIR_${d.slice(0, -4).toUpperCase()}=${path.join(deps, d).replaceAll("\\", "/")}`);
  }
  return args;
}

function generatorArgs() {
  if (!IS_WIN) return ["-DCMAKE_BUILD_TYPE=Release"];
  const cache = path.join(REPO, "core", "build", "CMakeCache.txt");
  const gen = fs.existsSync(cache) ? /^CMAKE_GENERATOR:INTERNAL=(.+)$/m.exec(fs.readFileSync(cache, "utf8"))?.[1] : null;
  return ["-G", gen ?? "Visual Studio 18 2026", "-A", "x64"];
}

/** A stand-in for npm: the core's build runs the UI's TypeScript build, which a fresh worktree cannot do and the harness does not need. */
function noopNpm(workDir) {
  const file = path.join(workDir, IS_WIN ? "noop-npm.cmd" : "noop-npm");
  if (!fs.existsSync(file)) {
    fs.mkdirSync(workDir, { recursive: true });
    fs.writeFileSync(file, IS_WIN ? "@exit /b 0\r\n" : "#!/bin/sh\nexit 0\n", { mode: 0o755 });
  }
  return file.replaceAll("\\", "/");
}

async function build(rev, workDir) {
  const buildDir = path.join(workDir, "build", rev.key);
  const logs = path.join(workDir, "logs");
  if (!fs.existsSync(path.join(buildDir, "CMakeCache.txt"))) {
    const shared = sharedDepsArgs(rev);
    step(`configuring ${rev.label}${shared.length ? " (reusing this checkout's fetched dependencies)" : " (fetching its own dependencies)"}`);
    const code = await run("cmake", [...generatorArgs(), "-S", path.join(rev.srcDir, "core"), "-B", buildDir,
      "-DGUITARFX_CORE_BUILD_TESTS=ON", `-DNPM_EXECUTABLE=${noopNpm(workDir)}`, ...shared], { log: path.join(logs, `${rev.key}-configure.log`) });
    if (code !== 0) {
      fs.rmSync(path.join(buildDir, "CMakeCache.txt"), { force: true });
      throw new Error(`configure failed for ${rev.label}:\n${tailOf(path.join(logs, `${rev.key}-configure.log`), /error/i)}`);
    }
  }
  step(`building AudioSnapshot for ${rev.label} (Release)`);
  const log = path.join(logs, `${rev.key}-build.log`);
  let compiled = 0;
  const code = await run("cmake", ["--build", buildDir, "--config", "Release", "--target", "AudioSnapshot", "--parallel"], {
    log,
    onLine: (l) => {
      if (/\.(cpp|cc|c)\s*$/.test(l.trim())) {
        compiled++;
        if (compiled % 25 === 0) process.stdout.write(`   ${compiled} files compiled\r`);
      }
    },
  });
  if (code !== 0) {
    throw new Error(`build failed for ${rev.label} (full log: ${log}):\n${tailOf(log, /\berror\b/i)}` +
      (rev.key !== WORKTREE ? "\nThe harness is this checkout's; if it uses an API this revision lacks, guard it with a requires-check (see AudioSnapshotSupport.h)." : ""));
  }
  const exe = [path.join(buildDir, "tests", "Release", IS_WIN ? "AudioSnapshot.exe" : "AudioSnapshot"), path.join(buildDir, "tests", "AudioSnapshot")].find((p) => fs.existsSync(p));
  if (!exe) throw new Error(`built, but no AudioSnapshot executable under ${buildDir}`);
  return exe;
}

// ---------------------------------------------------------------- render

function renderKey(rev, opts) {
  const harness = sha256(...HARNESS_FILES.map((f) => fs.readFileSync(path.join(REPO, "core", "tests", f))));
  const definition = fs.readFileSync(opts.definition);
  const def = JSON.parse(definition);
  const assets = path.join(REPO, "core", "tests", "testdata", "assets");
  const inputs = [path.join(REPO, "core", "ui", "demo", "DI_Guitar_L.wav"), ...Object.values(def.resources ?? {}).map((r) => path.join(assets, r))]
    .map((f) => (fs.existsSync(f) ? `${f}:${fs.statSync(f).size}` : `${f}:missing`));
  return sha256(JSON.stringify({ sha: rev.sha, harness, definition: sha256(definition), inputs, rate: opts.rate, block: opts.block, signals: opts.signals, filter: opts.filter, only: opts.only })).slice(0, 10);
}

async function render(rev, opts) {
  const key = renderKey(rev, opts);
  const dir = path.join(opts.workDir, "renders", `${rev.key}-${opts.rate}-b${opts.block}${rev.key === WORKTREE ? "" : `-${key}`}`);
  const infoFile = path.join(dir, "render-info.json");
  if (rev.key !== WORKTREE && !opts.rerender && fs.existsSync(infoFile) && JSON.parse(fs.readFileSync(infoFile, "utf8")).key === key) {
    step(`${rev.label}: using cached renders in ${dir}`);
    return dir;
  }
  ensureWorktree(rev);
  injectHarness(rev);
  const exe = await build(rev, opts.workDir);

  step(`rendering ${rev.label} at ${opts.rate} Hz, ${opts.block}-sample blocks`);
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(path.join(dir, "_env", "local"), { recursive: true });
  const args = ["--out", dir, "--definition", opts.definition, "--assets", path.join(REPO, "core", "tests", "testdata", "assets"),
    "--di", path.join(REPO, "core", "ui", "demo", "DI_Guitar_L.wav"), "--rate", String(opts.rate), "--block", String(opts.block)];
  if (opts.signals) args.push("--signals", opts.signals);
  if (opts.filter) args.push("--filter", opts.filter);
  if (opts.only) args.push("--only", opts.only);
  // The harness gives each controller a profile of its own; this keeps anything else off the real one too.
  const env = { ...process.env, APPDATA: path.join(dir, "_env"), LOCALAPPDATA: path.join(dir, "_env", "local"), HOME: path.join(dir, "_env") };
  const started = Date.now();
  let count = 0;
  const log = path.join(opts.workDir, "logs", `${rev.key}-render.log`);
  const code = await run(exe, args, {
    cwd: dir, env, log,
    onLine: (l) => {
      if (/^ {2}\S+__\S+\s+peak/.test(l)) { count++; if (count % 20 === 0) process.stdout.write(`   ${count} renders\r`); }
      else if (/^(effects|chains):|NON-FINITE|skipped|ERROR|missing|cannot/.test(l.trim())) console.log(`   ${l.trim()}`);
    },
  });
  if (code !== 0) throw new Error(`AudioSnapshot failed for ${rev.label} (log: ${log}):\n${tailOf(log, null, 15)}`);
  fs.rmSync(path.join(dir, "_env"), { recursive: true, force: true });
  fs.writeFileSync(infoFile, JSON.stringify({ label: rev.label, spec: rev.spec, sha: rev.sha, describe: rev.describe, dirty: rev.dirty, key, rate: opts.rate, block: opts.block, renderedAt: new Date().toISOString() }, null, 1));
  console.log(`   ${count} renders in ${((Date.now() - started) / 1000).toFixed(0)} s -> ${dir}`);
  return dir;
}

// ---------------------------------------------------------------- commands

function report(dirA, dirB, opts, names) {
  const stamp = new Date().toISOString().slice(0, 19).replaceAll(":", "").replace("T", "-");
  const reportDir = path.join(opts.workDir, "reports", `${names ?? "adhoc"}-${opts.rate}-b${opts.block}-${stamp}`);
  step("comparing");
  const model = compareRuns(dirA, dirB, reportDir, { thresholds: opts.thresholds });
  writeHtml(model, reportDir);
  writeMarkdown(model, reportDir);
  writeJson(model, reportDir);
  printSummary(model, { reportDir });
  if (opts.failOn === "never") return 0;
  const bar = severityRank(opts.failOn);
  return model.subjects.some((s) => severityRank(s.verdict) >= bar && s.verdict !== "new" && s.verdict !== "removed") ? 1 : 0;
}

function list(opts) {
  const show = (title, dir) => {
    const items = fs.existsSync(dir) ? fs.readdirSync(dir) : [];
    console.log(`${title} (${dir}):${items.length ? "" : " none"}`);
    for (const i of items) {
      const info = path.join(dir, i, "render-info.json");
      console.log(`  ${i}${fs.existsSync(info) ? `  ${JSON.parse(fs.readFileSync(info, "utf8")).describe}` : ""}`);
    }
  };
  show("worktrees", path.join(opts.workDir, "src"));
  show("builds", path.join(opts.workDir, "build"));
  show("renders", path.join(opts.workDir, "renders"));
  show("reports", path.join(opts.workDir, "reports"));
}

function clean(opts) {
  const src = path.join(opts.workDir, "src");
  for (const key of fs.existsSync(src) ? fs.readdirSync(src) : []) {
    console.log(`removing worktree ${key}`);
    try { git(["worktree", "remove", "--force", path.join(src, key)]); } catch (e) { console.log(`  ${e.message}`); }
  }
  git(["worktree", "prune"]);
  for (const sub of opts.all ? ["src", "build", "logs", "renders", "reports"] : ["src", "build", "logs"]) {
    fs.rmSync(path.join(opts.workDir, sub), { recursive: true, force: true });
  }
  console.log(`cleaned ${opts.workDir}${opts.all ? "" : " (renders and reports kept; --all removes them)"}`);
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const [cmd, ...rest] = opts.positional;
  if (opts.help || !cmd) {
    console.log(fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("*/")[0].replace(/^#!.*\n\/\*\*\n?/, "").replace(/^ \* ?/gm, ""));
    return 0;
  }
  if (cmd === "list") {
    list(opts);
    return 0;
  }
  if (cmd === "clean") {
    clean(opts);
    return 0;
  }
  if (cmd === "report") {
    if (rest.length !== 2) throw new Error("report needs two render directories");
    return report(path.resolve(rest[0]), path.resolve(rest[1]), opts);
  }
  if (cmd === "render") {
    if (rest.length !== 1) throw new Error("render needs one revision");
    await render(resolveRevision(rest[0], opts.workDir), opts);
    return 0;
  }
  const base = resolveRevision(cmd, opts.workDir);
  const target = resolveRevision(rest[0] ?? "WORKTREE", opts.workDir);
  const dirA = await render(base, opts);
  const dirB = await render(target, opts);
  return report(dirA, dirB, opts, `${base.key}-vs-${target.key}`);
}

main().then((code) => process.exit(code ?? 0), (e) => {
  console.error(`\naudio-ab: ${e.message}`);
  process.exit(2);
});
