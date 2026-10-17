// node --test tools/audio-ab/perf.test.mjs
// How AudioPerf's rounds become the perf table: best round, ratio, flags.
import assert from "node:assert/strict";
import test from "node:test";
import { summarisePerf } from "./perf.mjs";

const OPTIONS = { rate: 48000, block: 64, rounds: 3, seconds: 10, stereo: false };
const rev = (label) => ({ label, spec: label, sha: "0".repeat(40), describe: label, dirty: false, config: null });
const timing = (id, round, meanUs, extra = {}) => ({ id, name: id, round, meanUs, medianUs: meanUs, p99Us: meanUs, maxUs: meanUs, rms: 0.2, latencySamples: 0, passthrough: false, ...extra });
const summarise = (results) => summarisePerf({ revisions: results.map((_, i) => rev(`r${i}`)), results, options: OPTIONS, machine: {} });

test("each case keeps its best round, and the ratio is against the first revision", () => {
  const model = summarise([
    [timing("nam", 1, 66), timing("nam", 2, 64), timing("nam", 3, 70)],
    [timing("nam", 1, 22), timing("nam", 2, 21), timing("nam", 3, 21.5)],
  ]);
  const [base, target] = model.cases[0].results;
  assert.equal(base.best.meanUs, 64);
  assert.equal(base.best.round, 2);
  assert.equal(base.ratio, 1);
  assert.equal(target.best.meanUs, 21);
  assert.ok(Math.abs(target.ratio - 21 / 64) < 1e-12);
  assert.ok(Math.abs(model.deadlineUs - 1333.333) < 0.001);
});

test("cases keep the order they first appear in, across revisions", () => {
  const model = summarise([[timing("a", 1, 1), timing("b", 1, 1)], [timing("c", 1, 1), timing("a", 1, 1)]]);
  assert.deepEqual(model.cases.map((c) => c.id), ["a", "b", "c"]);
  assert.equal(model.cases[1].results[1].status, "missing");
  assert.equal(model.cases[2].results[0].status, "missing");
  assert.equal(model.cases[2].results[1].ratio, undefined);
});

test("a case that failed on every round is an error, and a failed round is ignored", () => {
  const model = summarise([
    [timing("x", 1, 10), { id: "x", name: "x", round: 2, error: "boom" }],
    [{ id: "x", name: "x", round: 1, error: "unknown effect type 'x'" }],
  ]);
  const [base, target] = model.cases[0].results;
  assert.equal(base.status, "ok");
  assert.deepEqual(base.roundMeansUs, [10]);
  assert.equal(target.status, "error");
  assert.equal(target.error, "unknown effect type 'x'");
});

test("passthrough, silent and noisy results are flagged", () => {
  const model = summarise([
    [timing("p", 1, 1, { passthrough: true }), timing("s", 1, 5, { rms: 0 }), timing("n", 1, 10), timing("n", 2, 12)],
  ]);
  const flags = Object.fromEntries(model.cases.map((c) => [c.id, c.results[0].flags]));
  assert.deepEqual(flags.p, ["passthrough"]);
  assert.deepEqual(flags.s, ["silent"]);
  assert.deepEqual(flags.n, ["noisy"]);
});

test("one slow round among close ones is not noise", () => {
  const model = summarise([[timing("x", 1, 72), timing("x", 2, 68.7), timing("x", 3, 86.6), timing("x", 4, 70)]]);
  const r = model.cases[0].results[0];
  assert.equal(r.best.meanUs, 68.7);
  assert.ok(Math.abs(r.spread - (70 / 68.7 - 1)) < 1e-12);
  assert.deepEqual(r.flags, []);
});

test("a single round has no spread", () => {
  const r = summarise([[timing("x", 1, 10)]]).cases[0].results[0];
  assert.equal(r.spread, null);
  assert.deepEqual(r.flags, []);
});
