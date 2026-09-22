/**
 * Measures how two renders of the same case differ, and whether a player would hear it.
 *
 * Every measure is taken per channel and the worst channel reported. What decides the
 * verdict (thresholds below, overridable from the command line):
 *
 *   level      rms change over the whole render
 *   tone       the largest change in any third-octave band that carries real energy
 *              (within `toneRangeDb` of the loudest band), beyond the overall level change,
 *              from a Welch average spectrum
 *   character  what is left after aligning B to A and matching its gain: a residual that
 *              is loud relative to the signal means the waveform itself changed (a new
 *              nonlinearity, a different modulation, a timing change)
 *   tail       rms of what follows the stimulus (reverb and delay decay; for `silence`,
 *              the self-noise)
 *   latency    the integer lag that best aligns B to A; reported, never a verdict
 *
 * `rounding` means the plain null is below `roundingNullDb`, or the residual itself is below
 * `roundingResidualDbfs`: float noise, not a change. Nothing quieter than `audibleFloorDbfs`
 * (a residual, a band, a level) can raise a flag, so two renders that differ only in their
 * denormal-level noise are `inaudible`, not `audible`; and a change that is itself quieter
 * than `quietDbfs` (a residual, a band, a tail, a level) is at most `slight`.
 * A case that was not deterministic in its own build is judged on level, tone and tail
 * only, since its null says nothing.
 */

import fs from "node:fs";

export const DEFAULT_THRESHOLDS = {
  roundingNullDb: -120,
  roundingResidualDbfs: -140,
  audibleFloorDbfs: -100,
  quietDbfs: -70,
  levelSlightDb: 0.1,
  levelAudibleDb: 0.5,
  toneSlightDb: 0.3,
  toneAudibleDb: 1.0,
  toneRangeDb: 40,
  characterSlightNullDb: -40,
  characterAudibleNullDb: -20,
  tailSlightDb: 0.5,
  tailAudibleDb: 1.5,
  tailFloorDbfs: -90,
  maxLagMs: 150,
};

export const SEVERITY = ["identical", "rounding", "inaudible", "slight", "audible", "broken"];

export const severityRank = (verdict) => {
  const i = SEVERITY.indexOf(verdict);
  return i < 0 ? SEVERITY.length : i; // missing / new rank above everything
};

const FLOOR_DB = -200;
export const db = (x) => (x > 0 ? Math.max(FLOOR_DB, 20 * Math.log10(x)) : FLOOR_DB);
export const powDb = (p) => (p > 0 ? Math.max(FLOOR_DB, 10 * Math.log10(p)) : FLOOR_DB);

// ---------------------------------------------------------------- WAV

/** Reads a 32-bit float or 16/24-bit PCM WAV into Float32Array channels. */
export function readWav(file) {
  const buf = fs.readFileSync(file);
  if (buf.toString("ascii", 0, 4) !== "RIFF" || buf.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error(`not a WAV file: ${file}`);
  }
  let p = 12;
  let format = 1, channels = 1, rate = 48000, bits = 16;
  while (p + 8 <= buf.length) {
    const id = buf.toString("ascii", p, p + 4);
    const size = buf.readUInt32LE(p + 4);
    if (id === "fmt ") {
      format = buf.readUInt16LE(p + 8);
      channels = buf.readUInt16LE(p + 10);
      rate = buf.readUInt32LE(p + 12);
      bits = buf.readUInt16LE(p + 22);
      if (format === 0xfffe && size >= 26) format = buf.readUInt16LE(p + 32);
    } else if (id === "data") {
      const bytes = Math.min(size, buf.length - (p + 8));
      const stride = channels * (bits / 8);
      const frames = Math.floor(bytes / stride);
      const out = Array.from({ length: channels }, () => new Float32Array(frames));
      const base = p + 8;
      for (let i = 0; i < frames; i++) {
        for (let c = 0; c < channels; c++) {
          const o = base + i * stride + c * (bits / 8);
          out[c][i] =
            format === 3 ? buf.readFloatLE(o) : bits === 16 ? buf.readInt16LE(o) / 32768 : buf.readIntLE(o, 3) / 8388608;
        }
      }
      return { rate, channels: out, frames };
    }
    p += 8 + size + (size & 1);
  }
  throw new Error(`no data chunk: ${file}`);
}

/** Writes Float32Array channels as a 32-bit float WAV. */
export function writeWav(file, channels, rate) {
  const frames = channels[0].length;
  const n = channels.length;
  const data = Buffer.alloc(frames * n * 4);
  for (let i = 0; i < frames; i++) for (let c = 0; c < n; c++) data.writeFloatLE(channels[c][i], (i * n + c) * 4);
  const h = Buffer.alloc(58);
  h.write("RIFF", 0);
  h.writeUInt32LE(50 + data.length, 4);
  h.write("WAVEfmt ", 8);
  h.writeUInt32LE(18, 16);
  h.writeUInt16LE(3, 20);
  h.writeUInt16LE(n, 22);
  h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * n * 4, 28);
  h.writeUInt16LE(n * 4, 32);
  h.writeUInt16LE(32, 34);
  h.writeUInt16LE(0, 36);
  h.write("fact", 38);
  h.writeUInt32LE(4, 42);
  h.writeUInt32LE(frames, 46);
  h.write("data", 50);
  h.writeUInt32LE(data.length, 54);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

// ---------------------------------------------------------------- FFT

/** In-place iterative radix-2 FFT. */
export function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      let t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    const half = len >> 1;
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < half; k++) {
        const a = i + k, b = a + half;
        const vr = re[b] * cr - im[b] * ci;
        const vi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - vr; im[b] = im[a] - vi;
        re[a] += vr; im[a] += vi;
        const t = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = t;
      }
    }
  }
}

const nextPow2 = (n) => 2 ** Math.ceil(Math.log2(Math.max(2, n)));

// ---------------------------------------------------------------- measures

function sumSq(x, from = 0, to = x.length) {
  let s = 0;
  for (let i = from; i < to; i++) s += x[i] * x[i];
  return s;
}

function countNonFinite(x) {
  let n = 0;
  for (let i = 0; i < x.length; i++) if (!Number.isFinite(x[i])) n++;
  return n;
}

/** Copy with non-finite samples zeroed, so one NaN does not poison every measure. */
function finiteCopy(x) {
  const y = new Float64Array(x.length);
  for (let i = 0; i < x.length; i++) y[i] = Number.isFinite(x[i]) ? x[i] : 0;
  return y;
}

/** The start of the loudest `len`-sample window of x, on a coarse grid. */
function loudestWindow(x, len) {
  if (x.length <= len) return 0;
  const hop = Math.max(1, len >> 3);
  let best = 0, bestE = -1;
  for (let s = 0; s + len <= x.length; s += hop) {
    const e = sumSq(x, s, s + len);
    if (e > bestE) { bestE = e; best = s; }
  }
  return best;
}

/**
 * The lag (B relative to A, in samples, within ±maxLag) that best aligns them, found by FFT
 * cross-correlation over A's loudest window. Positive means B is later than A.
 */
export function bestLag(a, b, maxLag) {
  const W = Math.min(32768, a.length);
  if (W < 64 || sumSq(a) === 0 || sumSq(b) === 0) return 0;
  const start = loudestWindow(a, W);
  const N = nextPow2(W + 2 * maxLag);
  const ar = new Float64Array(N), ai = new Float64Array(N);
  const br = new Float64Array(N), bi = new Float64Array(N);
  for (let i = 0; i < W; i++) ar[i] = a[start + i];
  for (let i = 0; i < W + 2 * maxLag; i++) {
    const j = start - maxLag + i;
    br[i] = j >= 0 && j < b.length ? b[j] : 0;
  }
  fft(ar, ai);
  fft(br, bi);
  // conj(A) * B, then inverse via the conjugate trick.
  for (let k = 0; k < N; k++) {
    const r = ar[k] * br[k] + ai[k] * bi[k];
    const i = ar[k] * bi[k] - ai[k] * br[k];
    ar[k] = r; ai[k] = -i;
  }
  fft(ar, ai);
  let best = 0, bestV = -Infinity;
  for (let lag = -maxLag; lag <= maxLag; lag++) {
    const v = ar[lag + maxLag];
    if (v > bestV) { bestV = v; best = lag; }
  }
  return best;
}

/** Plain and gain-matched residual of B (shifted by lag) against A, in dB relative to A. */
function nullTest(a, b, lag) {
  let ee = 0, ab = 0, bb = 0, aa = 0;
  for (let i = 0; i < a.length; i++) {
    const j = i + lag;
    const bv = j >= 0 && j < b.length ? b[j] : 0;
    const av = a[i];
    const d = av - bv;
    ee += d * d; ab += av * bv; bb += bv * bv; aa += av * av;
  }
  const g = bb > 0 ? ab / bb : 1; // least-squares gain
  const eg = Math.max(0, aa - 2 * g * ab + g * g * bb);
  return {
    nullDb: aa > 0 ? powDb(ee / aa) : null,
    nullGainDb: aa > 0 ? powDb(eg / aa) : null,
    residualDbfs: powDb(ee / Math.max(1, a.length)),
    residualGainDbfs: powDb(eg / Math.max(1, a.length)),
    gainDb: g > 0 ? db(g) : null,
  };
}

const THIRD_OCTAVES = [
  25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150,
  4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000,
];

/** Third-octave band powers from a Welch average (8192-point Hann, half overlap). */
export function bandPowers(x, rate) {
  const N = 8192, hop = N / 2;
  const win = new Float64Array(N);
  let wss = 0;
  for (let i = 0; i < N; i++) { win[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / N); wss += win[i] * win[i]; }
  const psd = new Float64Array(N / 2);
  let frames = 0;
  const re = new Float64Array(N), im = new Float64Array(N);
  for (let s = 0; s === 0 || s + N <= x.length; s += hop) {
    for (let i = 0; i < N; i++) { re[i] = (x[s + i] ?? 0) * win[i]; im[i] = 0; }
    fft(re, im);
    for (let k = 0; k < N / 2; k++) psd[k] += re[k] * re[k] + im[k] * im[k];
    frames++;
    if (x.length < N) break;
  }
  const binHz = rate / N;
  return THIRD_OCTAVES.filter((fc) => fc * 2 ** (1 / 6) < rate / 2).map((fc) => {
    const lo = Math.max(1, Math.ceil((fc * 2 ** (-1 / 6)) / binHz));
    const hi = Math.min(N / 2 - 1, Math.floor((fc * 2 ** (1 / 6)) / binHz));
    let p = 0;
    for (let k = lo; k <= hi; k++) p += psd[k];
    // Bands narrower than a bin still get the bin they sit in.
    if (hi < lo) p = psd[Math.min(N / 2 - 1, Math.round(fc / binHz))];
    return { hz: fc, power: p / (frames * wss) };
  });
}

/**
 * Per-band change, and its shape: the change less the overall level change, so that a pure
 * gain change is a level change and not also a tone change in every band.
 */
function compareBands(a, b, rate, rangeDb, floorDb) {
  const pa = bandPowers(a, rate), pb = bandPowers(b, rate);
  const top = Math.max(...pa.map((x) => x.power), ...pb.map((x) => x.power));
  const overall = powDb(pb.reduce((s, x) => s + x.power, 0)) - powDb(pa.reduce((s, x) => s + x.power, 0));
  const bands = pa.map((x, i) => {
    const aDb = powDb(x.power), bDb = powDb(pb[i].power);
    const relevant = top > 0 && Math.max(x.power, pb[i].power) >= top * 10 ** (-rangeDb / 10) && Math.max(aDb, bDb) > floorDb;
    return { hz: x.hz, aDb, bDb, dDb: bDb - aDb, sDb: bDb - aDb - overall, relevant };
  });
  let worst = null;
  for (const band of bands) if (band.relevant && (!worst || Math.abs(band.sDb) > Math.abs(worst.sDb))) worst = band;
  return { bands, worst };
}

/** Everything about one channel pair. */
function compareChannel(aRaw, bRaw, ctx) {
  const a = finiteCopy(aRaw), b = finiteCopy(bRaw);
  const len = Math.min(a.length, b.length);
  const rmsA = Math.sqrt(sumSq(a, 0, len) / Math.max(1, len));
  const rmsB = Math.sqrt(sumSq(b, 0, len) / Math.max(1, len));
  let peakA = 0, peakB = 0, dcA = 0, dcB = 0;
  for (let i = 0; i < len; i++) {
    peakA = Math.max(peakA, Math.abs(a[i])); peakB = Math.max(peakB, Math.abs(b[i]));
    dcA += a[i]; dcB += b[i];
  }
  const maxLag = Math.round((ctx.thresholds.maxLagMs / 1000) * ctx.rate);
  const lag = bestLag(a.subarray(0, len), b.subarray(0, len), maxLag);
  const nt = nullTest(a.subarray(0, len), b.subarray(0, len), lag);
  const tone = compareBands(a.subarray(0, len), b.subarray(0, len), ctx.rate, ctx.thresholds.toneRangeDb,
    ctx.thresholds.audibleFloorDbfs);
  const tailFrom = Math.min(len, ctx.playFrames + Math.round(0.05 * ctx.rate));
  const tailLen = len - tailFrom;
  const tailA = tailLen > 0 ? Math.sqrt(sumSq(a, tailFrom, len) / tailLen) : 0;
  const tailB = tailLen > 0 ? Math.sqrt(sumSq(b, tailFrom, len) / tailLen) : 0;
  return {
    rmsA: db(rmsA), rmsB: db(rmsB), dRmsDb: db(rmsB) - db(rmsA),
    peakA: db(peakA), peakB: db(peakB), dPeakDb: db(peakB) - db(peakA),
    dcA: dcA / Math.max(1, len), dcB: dcB / Math.max(1, len),
    lag, ...nt,
    bands: tone.bands, worstBand: tone.worst,
    tailA: tailLen > 0 ? db(tailA) : null, tailB: tailLen > 0 ? db(tailB) : null,
    nonFiniteA: countNonFinite(aRaw), nonFiniteB: countNonFinite(bRaw),
  };
}

function bitExact(a, b) {
  if (a.channels.length !== b.channels.length || a.frames !== b.frames) return false;
  for (let c = 0; c < a.channels.length; c++) {
    const x = a.channels[c], y = b.channels[c];
    for (let i = 0; i < x.length; i++) if (Object.is(x[i], y[i]) === false && x[i] !== y[i]) return false;
  }
  return true;
}

const pick = (list, key) => list.reduce((w, m) => (Math.abs(m[key] ?? 0) > Math.abs(w[key] ?? 0) ? m : w), list[0]);
const fmt = (x, n = 1) => (x === null || x === undefined ? "–" : (x > 0 ? "+" : "") + x.toFixed(n));

/**
 * Compares render B against render A (both from readWav). `ctx` carries playFrames and
 * whether each side was deterministic. Returns the measures, the flags and a verdict.
 */
export function compareRenders(A, B, ctx) {
  const thresholds = { ...DEFAULT_THRESHOLDS, ...(ctx.thresholds ?? {}) };
  const rate = A.rate;
  const exact = bitExact(A, B);
  const stereo = A.channels.length > 1 || B.channels.length > 1;
  const chA = stereo ? [A.channels[0], A.channels[1] ?? A.channels[0]] : [A.channels[0]];
  const chB = stereo ? [B.channels[0], B.channels[1] ?? B.channels[0]] : [B.channels[0]];
  const per = exact ? [] : chA.map((a, i) => compareChannel(a, chB[i], { rate, playFrames: ctx.playFrames, thresholds }));

  const result = { bitExact: exact, stereo, flags: [], verdict: "identical" };
  if (exact) return result;

  // Worst channel for each measure.
  const level = pick(per, "dRmsDb");
  const peak = pick(per, "dPeakDb");
  const nullWorst = per.reduce((w, m) => ((m.nullDb ?? FLOOR_DB) > (w.nullDb ?? FLOOR_DB) ? m : w), per[0]);
  const charWorst = per.reduce((w, m) => ((m.nullGainDb ?? FLOOR_DB) > (w.nullGainDb ?? FLOOR_DB) ? m : w), per[0]);
  const toneWorst = per.reduce(
    (w, m) => (m.worstBand && (!w.worstBand || Math.abs(m.worstBand.sDb) > Math.abs(w.worstBand.sDb)) ? m : w),
    per[0]
  );
  const tailWorst = per.reduce(
    (w, m) => (Math.abs((m.tailB ?? 0) - (m.tailA ?? 0)) > Math.abs((w.tailB ?? 0) - (w.tailA ?? 0)) ? m : w),
    per[0]
  );
  Object.assign(result, {
    rmsA: level.rmsA, rmsB: level.rmsB, dRmsDb: level.dRmsDb,
    peakA: peak.peakA, peakB: peak.peakB, dPeakDb: peak.dPeakDb,
    lag: per[0].lag,
    nullDb: nullWorst.nullDb, residualDbfs: nullWorst.residualDbfs,
    nullGainDb: charWorst.nullGainDb, residualGainDbfs: charWorst.residualGainDbfs, gainDb: charWorst.gainDb,
    worstBand: toneWorst.worstBand, bands: toneWorst.bands,
    tailA: tailWorst.tailA, tailB: tailWorst.tailB,
    nonFiniteA: per.reduce((s, m) => s + m.nonFiniteA, 0),
    nonFiniteB: per.reduce((s, m) => s + m.nonFiniteB, 0),
    dcA: per[0].dcA, dcB: per[0].dcB,
  });

  const flag = (kind, severity, text) => result.flags.push({ kind, severity, text });
  const deterministic = ctx.deterministicA !== false && ctx.deterministicB !== false;
  if (!deterministic) flag("nondeterministic", "note", "not deterministic in its own build: judged on level, tone and tail only");

  if (result.nonFiniteB > 0) flag("nonfinite", "broken", `${result.nonFiniteB} non-finite samples (base had ${result.nonFiniteA})`);
  if (result.rmsA > -80 && result.rmsB < result.rmsA - 40) flag("silent", "broken", `output fell silent (${fmt(result.rmsA)} → ${fmt(result.rmsB)} dBFS rms)`);

  const floor = thresholds.audibleFloorDbfs;
  // A pure latency change nulls perfectly once aligned, but it is still a change: it moves a
  // parallel path against its neighbours. It stays at least `inaudible`, with its note.
  const rounding =
    result.lag === 0 &&
    ((result.nullDb !== null && result.nullDb <= thresholds.roundingNullDb) ||
      result.residualDbfs <= thresholds.roundingResidualDbfs);
  if (rounding && result.flags.every((f) => f.severity !== "broken")) {
    result.verdict = "rounding";
    return result;
  }

  const grade = (value, slight, audible) => (value >= audible ? "audible" : value >= slight ? "slight" : null);
  const cap = (severity, levelDbfs) => (severity === "audible" && levelDbfs < thresholds.quietDbfs ? "slight" : severity);

  const lvl = grade(Math.abs(result.dRmsDb), thresholds.levelSlightDb, thresholds.levelAudibleDb);
  if (lvl && Math.max(result.rmsA, result.rmsB) > floor) flag("level", cap(lvl, Math.max(result.rmsA, result.rmsB)), `level ${fmt(result.dRmsDb, 2)} dB`);

  if (result.worstBand) {
    const tn = grade(Math.abs(result.worstBand.sDb), thresholds.toneSlightDb, thresholds.toneAudibleDb);
    if (tn) flag("tone", cap(tn, Math.max(result.worstBand.aDb, result.worstBand.bDb)), `tone ${fmt(result.worstBand.sDb, 2)} dB at ${formatHz(result.worstBand.hz)}${Math.abs(result.dRmsDb) >= thresholds.levelSlightDb ? " beyond the level change" : ""}`);
  }

  if (deterministic && result.nullGainDb !== null && result.residualGainDbfs > floor) {
    const ch = result.nullGainDb >= thresholds.characterAudibleNullDb ? "audible"
      : result.nullGainDb >= thresholds.characterSlightNullDb ? "slight" : null;
    if (ch) flag("character", cap(ch, result.residualGainDbfs), `waveform differs: gain-matched null ${fmt(result.nullGainDb)} dB`);
  }

  if (result.tailA !== null && Math.max(result.tailA, result.tailB) > thresholds.tailFloorDbfs) {
    const quietSide = Math.min(result.tailA, result.tailB);
    const d = result.tailB - result.tailA;
    // A tail appearing out of digital silence is a change however quiet the base was.
    const tl = quietSide < thresholds.tailFloorDbfs ? "audible" : grade(Math.abs(d), thresholds.tailSlightDb, thresholds.tailAudibleDb);
    if (tl) flag("tail", cap(tl, Math.max(result.tailA, result.tailB)), `${ctx.playFrames === 0 ? "self-noise" : "tail"} ${fmt(result.tailA)} → ${fmt(result.tailB)} dBFS`);
  }

  // A lag only means something when the aligned waveforms agree; for a render whose waveform
  // changed outright, the best-correlating lag is noise.
  const aligned = result.nullGainDb !== null && result.nullGainDb <= thresholds.characterAudibleNullDb;
  if (result.lag !== 0 && aligned) {
    flag("latency", "note", `B is ${Math.abs(result.lag)} samples (${(Math.abs(result.lag) / rate * 1000).toFixed(2)} ms) ${result.lag > 0 ? "later" : "earlier"}`);
  }

  // Notes (latency, non-determinism) inform; only graded flags decide.
  result.verdict = result.flags
    .filter((f) => SEVERITY.includes(f.severity))
    .reduce((w, f) => (severityRank(f.severity) > severityRank(w) ? f.severity : w), "inaudible");
  return result;
}

export function formatHz(hz) {
  return hz >= 1000 ? `${(hz / 1000).toFixed(hz % 1000 === 0 ? 0 : 1)} kHz` : `${hz} Hz`;
}

/** B aligned to A, minus A: what changed, at its real level. */
export function differenceChannels(A, B, lag) {
  const n = Math.max(A.channels.length, B.channels.length);
  return Array.from({ length: n }, (_, c) => {
    const a = A.channels[c] ?? A.channels[0];
    const b = B.channels[c] ?? B.channels[0];
    const d = new Float32Array(a.length);
    for (let i = 0; i < a.length; i++) {
      const j = i + lag;
      const bv = j >= 0 && j < b.length ? b[j] : 0;
      d[i] = (Number.isFinite(bv) ? bv : 0) - (Number.isFinite(a[i]) ? a[i] : 0);
    }
    return d;
  });
}
