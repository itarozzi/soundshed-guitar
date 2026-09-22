// node --test tools/audio-ab/analysis.test.mjs
// The verdicts on synthetic renders whose answer is known.
import assert from "node:assert/strict";
import test from "node:test";
import { compareRenders } from "./analysis.mjs";

const RATE = 48000;

/** A decaying guitar-ish note over 2 s, then 0.5 s of silence. */
function note() {
  const x = new Float32Array(RATE * 2.5);
  for (let i = 0; i < RATE * 2; i++) {
    const t = i / RATE;
    let v = 0;
    for (let h = 1; h <= 8; h++) v += Math.sin(2 * Math.PI * 110 * h * t) / h;
    x[i] = 0.25 * Math.exp(-t * 1.5) * v;
  }
  return x;
}

const render = (...channels) => ({ rate: RATE, channels, frames: channels[0].length });
const judge = (a, b, extra = {}) => compareRenders(render(a), render(b), { playFrames: RATE * 2, ...extra });
const map = (x, fn) => Float32Array.from(x, fn);

test("the same samples are identical", () => {
  const a = note();
  assert.equal(judge(a, Float32Array.from(a)).verdict, "identical");
});

test("float-rounding differences are rounding", () => {
  const a = note();
  assert.equal(judge(a, map(a, (v) => v * (1 + 1e-7))).verdict, "rounding");
});

test("a 0.2 dB level change is slight, 1 dB is audible", () => {
  const a = note();
  const slight = judge(a, map(a, (v) => v * 10 ** (0.2 / 20)));
  assert.equal(slight.verdict, "slight");
  assert.ok(slight.flags.some((f) => f.kind === "level"));
  assert.equal(judge(a, map(a, (v) => v * 10 ** (1 / 20))).verdict, "audible");
});

test("a pure delay is a latency note, not rounding", () => {
  const a = note();
  const b = new Float32Array(a.length);
  b.set(a.subarray(0, a.length - 32), 32);
  const r = judge(a, b);
  assert.equal(r.lag, 32);
  assert.equal(r.verdict, "inaudible");
  assert.ok(r.flags.some((f) => f.kind === "latency"));
});

test("a one-pole low-pass is a tone change", () => {
  const a = note();
  let y = 0;
  const b = map(a, (v) => (y += 0.05 * (v - y))); // about 390 Hz, through the note's harmonics
  const r = judge(a, b);
  assert.equal(r.verdict, "audible");
  assert.ok(r.flags.some((f) => f.kind === "tone"));
});

test("added distortion at the same level changes the character", () => {
  const a = note();
  const shaped = map(a, (v) => Math.tanh(6 * v));
  const rms = (x) => Math.sqrt(x.reduce((s, v) => s + v * v, 0) / x.length);
  const g = rms(a) / rms(shaped);
  const r = judge(a, map(shaped, (v) => v * g));
  assert.ok(r.flags.some((f) => f.kind === "character" && f.severity === "audible"));
});

test("differences below the audible floor raise nothing", () => {
  const quiet = map(note(), (v) => v * 1e-6); // about -130 dBFS
  const r = judge(quiet, map(quiet, (v, i) => v + (i % 2 ? 1e-8 : -1e-8)));
  assert.ok(["rounding", "inaudible"].includes(r.verdict), r.verdict);
});

test("self-noise appearing out of silence is audible", () => {
  const silent = new Float32Array(RATE);
  const hiss = map(silent, (_, i) => 1e-3 * Math.sin(i * 1.7)); // about -63 dBFS
  const r = compareRenders(render(silent), render(hiss), { playFrames: 0 });
  assert.equal(r.verdict, "audible");
  assert.ok(r.flags.some((f) => f.kind === "tail"));
});

test("non-finite output is broken", () => {
  const a = note();
  const b = Float32Array.from(a);
  b[1000] = NaN;
  assert.equal(judge(a, b).verdict, "broken");
});

test("a case that is not deterministic is not judged on its null", () => {
  const a = note();
  const shaped = map(a, (v) => Math.tanh(6 * v));
  const r = judge(a, shaped, { deterministicB: false });
  assert.ok(!r.flags.some((f) => f.kind === "character"));
});

test("stereo is judged on its worst channel", () => {
  const a = note();
  const r = compareRenders(render(a, a), render(a, map(a, (v) => v * 2)), { playFrames: RATE * 2 });
  assert.equal(r.verdict, "audible");
  assert.ok(Math.abs(r.dRmsDb - 6.02) < 0.05, String(r.dRmsDb));
});

test("a change that is itself very quiet is at most slight", () => {
  const faint = map(note(), (v) => v * 10 ** (-60 / 20)); // about -80 dBFS
  const r = judge(faint, map(faint, (v) => Math.tanh(4e3 * v) / 4e3));
  assert.equal(r.verdict, "slight");
});
