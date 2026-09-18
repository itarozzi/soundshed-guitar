#!/usr/bin/env node
/**
 * Measure how much LLM-style compression changes a standard NAM model's output.
 *
 * Re-implements the standard NAM WaveNet forward pass (two layer arrays, non-gated,
 * Tanh) in double precision, then runs the same model with its numbers degraded in
 * each of the ways LLM inference is compressed:
 *
 *   fast_tanh     the NAM core's rational tanh (Soundshed enables it; NamModelCache.cpp)
 *   fp16 / bf16   reduced-precision weights and matmul inputs, fp32 or fp16 accumulation
 *   int8 / int4   per-output-channel weight quantisation, optionally int8/int16 activations
 *                 with static per-tensor scales calibrated on a second signal
 *   low rank      each array-1 matrix projected onto its top-r singular vectors
 *
 * Each variant is scored as SNR in dB against the unmodified model: overall, and over
 * the quietest quarter of 20 ms frames, where added noise shows first. It also prints
 * how many singular values each layer needs to keep 99% of its weight energy.
 *
 * The input is a synthetic Karplus-Strong DI (chugs, a ringing power chord, single
 * notes, a chord decaying towards silence) peaking at -8 dBFS, with 0.25 s of leading
 * silence so the model state matches the C++ core's prewarm.
 *
 * Usage:
 *   node tools/nam-compression.mjs                       # two bundled test models
 *   node tools/nam-compression.mjs "Guitar/A/B.nam" C:/x.nam   # paths under core/tests/testdata/assets/amps, or absolute
 *   node tools/nam-compression.mjs --quick               # skip fp16 accumulation (slow: ~1 min per model)
 *   node tools/nam-compression.mjs --build-render        # build the reference renderer first
 *   node tools/nam-compression.mjs --render <exe>        # default core/build-nam-tools/tools/Release/render(.exe)
 *
 * Validation: when the NAM core's own render tool is present, the model is also
 * rendered in C++ and the JS reference must agree with it (about 130 dB), which is what
 * makes the other numbers trustworthy. --build-render configures the pinned core that
 * core/build fetched (core/build/_deps/neuralampmodelercore-src) into core/build-nam-tools.
 * On Windows that uses clang-cl (-T ClangCL): the core's tools CMakeLists passes
 * -Wno-error, which MSVC rejects, and avrt.lib is linked for its MMCSS calls.
 *
 * Only standard WaveNet files are measured. A2 files are SlimmableContainers with a
 * different layer shape and are skipped.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const modelsRoot = path.join(repoRoot, 'core', 'tests', 'testdata', 'assets', 'amps');
const namSourceDir = path.join(repoRoot, 'core', 'build', '_deps', 'neuralampmodelercore-src');
const renderBuildDir = path.join(repoRoot, 'core', 'build-nam-tools');
const renderExeName = process.platform === 'win32' ? 'render.exe' : 'render';
const workDir = path.join(os.tmpdir(), 'soundshed-nam-compression');

const SR = 48000;
const LEAD = 12000; // Leading silence longer than the standard model's ~4,100-sample receptive field.

const defaultModels = [
    'Guitar/TimR/JCM800 2203 1985/JCM800 Hi P6 B8 M4 T7 G10.nam',
    'Guitar/TimR/Fender Twin Reverb - Breakup/Tim R Fender Twin Reverb Ch1 BR G04.nam',
];

// ---------- arguments ----------

function parseArgs(argv)
{
    const args = { models: [], quick: false, buildRender: false, render: null };

    for (let i = 0; i < argv.length; i++)
    {
        const arg = argv[i];

        if (arg === '--quick') args.quick = true;
        else if (arg === '--build-render') args.buildRender = true;
        else if (arg === '--render') args.render = argv[++i];
        else if (arg === '--help' || arg === '-h') args.help = true;
        else args.models.push(arg);
    }

    if (args.models.length === 0) args.models = defaultModels;
    return args;
}

// ---------- test signal ----------

function rng(seed)
{
    let s = seed >>> 0;
    return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296) * 2 - 1;
}

function pluck(out, start, freq, amp, decay, dur, rand)
{
    const n = Math.max(2, Math.round(SR / freq));
    const buf = new Float64Array(n);
    for (let i = 0; i < n; i++) buf[i] = rand() * amp;

    let idx = 0;
    const len = Math.min(out.length - start, Math.round(dur * SR));

    for (let t = 0; t < len; t++)
    {
        const a = buf[idx];
        const b = buf[(idx + 1) % n];
        const env = t > len - 480 ? (len - t) / 480 : 1; // 10 ms mute at the end
        out[start + t] += a * env;
        buf[idx] = decay * 0.5 * (a + b);
        idx = (idx + 1) % n;
    }
}

function makeSignal(seed)
{
    const rand = rng(seed);
    const x = new Float64Array(LEAD + Math.round(4.5 * SR));
    const at = (sec) => LEAD + Math.round(sec * SR);
    const E2 = 82.41, A2 = 110, B2 = 123.47, E3 = 164.81, G3 = 196, B3 = 246.94, E4 = 329.63;

    for (let i = 0; i < 8; i++) // palm-muted chugs
    {
        pluck(x, at(i * 0.125), E2, 0.9, 0.97, 0.11, rand);
        pluck(x, at(i * 0.125), B2, 0.7, 0.97, 0.11, rand);
    }

    for (const f of [E2, B2, E3]) pluck(x, at(1.05), f, 0.8, 0.996, 1.0, rand); // ringing power chord
    [E4, G3, A2, B3, E3, G3].forEach((f, i) => pluck(x, at(2.1 + i * 0.16), f, 0.25 + 0.12 * i, 0.995, 0.3, rand));
    for (const f of [E2, B2, E3, G3, B3, E4]) pluck(x, at(3.1), f, 0.5, 0.9985, 1.35, rand); // decays towards silence

    let peak = 0;
    for (const v of x) peak = Math.max(peak, Math.abs(v));
    for (let i = 0; i < x.length; i++) x[i] = Math.fround((x[i] * 0.4) / peak);
    return x;
}

function writeWavF32(file, x)
{
    const n = x.length;
    const buf = Buffer.alloc(44 + n * 4);
    buf.write('RIFF', 0);
    buf.writeUInt32LE(36 + n * 4, 4);
    buf.write('WAVE', 8);
    buf.write('fmt ', 12);
    buf.writeUInt32LE(16, 16);
    buf.writeUInt16LE(3, 20); // IEEE float
    buf.writeUInt16LE(1, 22);
    buf.writeUInt32LE(SR, 24);
    buf.writeUInt32LE(SR * 4, 28);
    buf.writeUInt16LE(4, 32);
    buf.writeUInt16LE(32, 34);
    buf.write('data', 36);
    buf.writeUInt32LE(n * 4, 40);
    for (let i = 0; i < n; i++) buf.writeFloatLE(x[i], 44 + i * 4);
    fs.writeFileSync(file, buf);
}

function readWavF32(file)
{
    const b = fs.readFileSync(file);

    for (let p = 12; p < b.length;)
    {
        const id = b.toString('ascii', p, p + 4);
        const size = b.readUInt32LE(p + 4);

        if (id === 'data')
        {
            const out = new Float64Array(size / 4);
            for (let i = 0; i < out.length; i++) out[i] = b.readFloatLE(p + 8 + i * 4);
            return out;
        }

        p += 8 + size + (size & 1);
    }

    throw new Error(`${file}: no data chunk`);
}

// ---------- number formats ----------

const f32 = new Float32Array(1);
const u32 = new Uint32Array(f32.buffer);

// Round to nearest even, keeping (23 - drop) bits of the float32 mantissa.
function roundMantissa(v, drop)
{
    f32[0] = v;
    const u = u32[0];
    u32[0] = ((u + ((1 << (drop - 1)) - 1) + ((u >>> drop) & 1)) >>> 0) & ~((1 << drop) - 1);
    return f32[0];
}

const bf16 = (v) => roundMantissa(v, 16);

function fp16(v)
{
    if (Math.abs(v) < 6.103515625e-5) return Math.round(v * 16777216) / 16777216; // subnormal grid, 2^-24
    return Math.max(-65504, Math.min(65504, roundMantissa(v, 13)));
}

// NAM core fast_tanh (NAM/activations.h).
function fastTanh(x)
{
    const ax = Math.abs(x);
    const x2 = x * x;
    return (x * (2.45550750702956 + 2.45550750702956 * ax + (0.893229853513558 + 0.821226666969744 * ax) * x2))
        / (2.44506634652299 + (2.44506634652299 + x2) * Math.abs(x + 0.814642734961073 * x * ax));
}

// ---------- model ----------

// Conv weights are W[k][out][in]; the .nam order is out, in, tap, then the bias.
function loadModel(file)
{
    const j = JSON.parse(fs.readFileSync(file, 'utf8'));

    if (j.architecture !== 'WaveNet')
    {
        return { skip: `architecture is ${j.architecture}, not a plain WaveNet` };
    }

    for (const lc of j.config.layers)
    {
        if (lc.gated || lc.activation !== 'Tanh' || !Array.isArray(lc.dilations) || lc.kernel_size === undefined)
        {
            return { skip: 'not the standard non-gated Tanh layer shape' };
        }
    }

    const w = j.weights;
    let p = 0;
    const take = () => w[p++];
    const conv = (cin, cout, K, bias) =>
    {
        const W = Array.from({ length: K }, () => Array.from({ length: cout }, () => new Float64Array(cin)));
        for (let o = 0; o < cout; o++) for (let i = 0; i < cin; i++) for (let k = 0; k < K; k++) W[k][o][i] = take();
        const b = bias ? Float64Array.from({ length: cout }, take) : null;
        return { W, b, K, cin, cout };
    };

    const arrays = j.config.layers.map((lc) =>
    {
        const C = lc.channels;
        const rechannel = conv(lc.input_size, C, 1, false);
        const layers = lc.dilations.map((d) => ({
            d,
            conv: conv(C, C, lc.kernel_size, true),
            mixin: conv(lc.condition_size, C, 1, false),
            l1x1: conv(C, C, 1, true),
        }));
        const head = conv(C, lc.head_size, 1, lc.head_bias);
        return { C, rechannel, layers, head };
    });

    const headScale = take();

    if (p !== w.length)
    {
        return { skip: `weight count mismatch (${p} read, ${w.length} in file)` };
    }

    return { arrays, headScale, nWeights: w.length };
}

const cloneConv = (c) => ({ ...c, W: c.W.map((m) => m.map((r) => Float64Array.from(r))), b: c.b && Float64Array.from(c.b) });

// Copy the model, passing every conv through fn(conv, arrayIndex, kind).
function mapConvs(model, fn)
{
    return {
        ...model,
        arrays: model.arrays.map((a, ai) => ({
            ...a,
            rechannel: fn(cloneConv(a.rechannel), ai, 'rechannel'),
            head: fn(cloneConv(a.head), ai, 'head'),
            layers: a.layers.map((l) => ({
                ...l,
                conv: fn(cloneConv(l.conv), ai, 'conv'),
                mixin: fn(cloneConv(l.mixin), ai, 'mixin'),
                l1x1: fn(cloneConv(l.l1x1), ai, '1x1'),
            })),
        })),
    };
}

// ---------- forward pass ----------
// opts.act          activation (default Math.tanh)
// opts.qin          (site, channels) => channels: quantise a matmul input
// opts.store        rounding applied to every stored tensor
// opts.macRound     rounding after every multiply and add (fp16 accumulation)

function runConv(c, xs, dil, T, opts)
{
    const out = Array.from({ length: c.cout }, (_, o) => new Float64Array(T).fill(c.b ? c.b[o] : 0));
    const R = opts.macRound;

    for (let k = 0; k < c.K; k++)
    {
        const off = dil * (c.K - 1 - k);

        for (let o = 0; o < c.cout; o++)
        {
            const dst = out[o];
            const row = c.W[k][o];

            for (let i = 0; i < c.cin; i++)
            {
                const wv = row[i];
                if (wv === 0) continue;
                const src = xs[i];
                if (R) for (let t = off; t < T; t++) dst[t] = R(dst[t] + R(wv * src[t - off]));
                else for (let t = off; t < T; t++) dst[t] += wv * src[t - off];
            }
        }
    }

    if (opts.store) for (const a of out) for (let t = 0; t < T; t++) a[t] = opts.store(a[t]);
    return out;
}

function forward(model, input, opts = {})
{
    const T = input.length;
    const act = opts.act || Math.tanh;
    const q = opts.qin || ((_, a) => a);
    const S = opts.store || ((v) => v);
    const cond = [input];
    let layerIn = [input];
    let headIn = null;

    model.arrays.forEach((arr, ai) =>
    {
        const x = runConv(arr.rechannel, q(`a${ai}.rechannel`, layerIn), 1, T, opts);
        const head = headIn
            ? headIn.map((h) => Float64Array.from(h))
            : Array.from({ length: arr.C }, () => new Float64Array(T));

        arr.layers.forEach((l, li) =>
        {
            const z = runConv(l.conv, q(`a${ai}.l${li}.x`, x), l.d, T, opts);
            const m = runConv(l.mixin, cond, 1, T, opts);

            for (let c = 0; c < arr.C; c++)
            {
                const zc = z[c], mc = m[c], hc = head[c];
                for (let t = 0; t < T; t++) zc[t] = S(act(zc[t] + mc[t]));
                for (let t = 0; t < T; t++) hc[t] = S(hc[t] + zc[t]);
            }

            const r = runConv(l.l1x1, q(`a${ai}.l${li}.a`, z), 1, T, opts);
            for (let c = 0; c < arr.C; c++)
            {
                const xc = x[c], rc = r[c];
                for (let t = 0; t < T; t++) xc[t] = S(xc[t] + rc[t]);
            }
        });

        headIn = runConv(arr.head, q(`a${ai}.head`, head), 1, T, opts);
        layerIn = x;
    });

    const out = new Float64Array(T);
    for (let t = 0; t < T; t++) out[t] = model.headScale * headIn[0][t];
    return out;
}

// ---------- compression variants ----------

function quantWeightsPerChannel(model, bits)
{
    const qmax = 2 ** (bits - 1) - 1;

    return mapConvs(model, (c) =>
    {
        for (let o = 0; o < c.cout; o++)
        {
            let m = 0;
            for (let k = 0; k < c.K; k++) for (const v of c.W[k][o]) m = Math.max(m, Math.abs(v));
            const s = m / qmax || 1;
            for (let k = 0; k < c.K; k++)
            {
                const r = c.W[k][o];
                for (let i = 0; i < r.length; i++) r[i] = Math.round(r[i] / s) * s;
            }
        }
        return c; // biases stay float, as int32 biases would in an int8 kernel
    });
}

function roundWeights(model, fn)
{
    return mapConvs(model, (c) =>
    {
        for (const m of c.W) for (const r of m) for (let i = 0; i < r.length; i++) r[i] = fn(r[i]);
        if (c.b) for (let i = 0; i < c.b.length; i++) c.b[i] = fn(c.b[i]);
        return c;
    });
}

function calibrateSites(model, calInput)
{
    const maxes = {};
    const qin = (site, arrs) =>
    {
        let m = maxes[site] || 0;
        for (const a of arrs) for (const v of a) m = Math.max(m, Math.abs(v));
        maxes[site] = m;
        return arrs;
    };
    forward(model, calInput, { qin });
    return maxes;
}

// Static per-tensor scales; the raw audio into the first 1x1 stays float, as it would in practice.
function intActivations(maxes, bits)
{
    const qmax = 2 ** (bits - 1) - 1;

    return (site, arrs) =>
    {
        if (site === 'a0.rechannel') return arrs;
        const s = (maxes[site] || 1) / qmax;
        return arrs.map((a) =>
        {
            const o = new Float64Array(a.length);
            for (let t = 0; t < a.length; t++) o[t] = Math.max(-qmax, Math.min(qmax, Math.round(a[t] / s))) * s;
            return o;
        });
    };
}

function floatActivations(fn)
{
    return (_, arrs) => arrs.map((a) =>
    {
        const o = new Float64Array(a.length);
        for (let t = 0; t < a.length; t++) o[t] = fn(a[t]);
        return o;
    });
}

// ---------- low rank ----------

// Jacobi eigendecomposition of a small symmetric matrix, eigenvalues descending.
function symEig(A)
{
    const n = A.length;
    const a = A.map((r) => Float64Array.from(r));
    const V = Array.from({ length: n }, (_, i) => Float64Array.from({ length: n }, (_, j) => +(i === j)));

    for (let sweep = 0; sweep < 100; sweep++)
    {
        let off = 0;
        for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) off += a[i][j] ** 2;
        if (off < 1e-22) break;

        for (let p = 0; p < n; p++)
        {
            for (let q = p + 1; q < n; q++)
            {
                if (Math.abs(a[p][q]) < 1e-300) continue;
                const th = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const t = Math.sign(th || 1) / (Math.abs(th) + Math.sqrt(th * th + 1));
                const c = 1 / Math.sqrt(t * t + 1);
                const s = t * c;
                for (let k = 0; k < n; k++) { const kp = a[k][p], kq = a[k][q]; a[k][p] = c * kp - s * kq; a[k][q] = s * kp + c * kq; }
                for (let k = 0; k < n; k++) { const pk = a[p][k], qk = a[q][k]; a[p][k] = c * pk - s * qk; a[q][k] = s * pk + c * qk; }
                for (let k = 0; k < n; k++) { const kp = V[k][p], kq = V[k][q]; V[k][p] = c * kp - s * kq; V[k][q] = s * kp + c * kq; }
            }
        }
    }

    const order = [...Array(n).keys()].sort((i, j) => a[j][j] - a[i][i]);
    return { vals: order.map((i) => Math.max(0, a[i][i])), vecs: order.map((i) => V.map((r) => r[i])) };
}

// A conv as an out x (in * K) matrix.
function flatten(c)
{
    return Array.from({ length: c.cout }, (_, o) =>
    {
        const r = [];
        for (let k = 0; k < c.K; k++) r.push(...c.W[k][o]);
        return r;
    });
}

const gram = (M) => M.map((ri) => M.map((rj) => ri.reduce((s, v, k) => s + v * rj[k], 0)));

function lowRank(model, rank)
{
    return mapConvs(model, (c, ai, kind) =>
    {
        if (ai !== 0 || (kind !== 'conv' && kind !== '1x1') || rank >= Math.min(c.cout, c.cin * c.K)) return c;
        const M = flatten(c);
        const U = symEig(gram(M)).vecs.slice(0, rank);
        const cols = M[0].length;
        const projected = M.map((_, o) => Array.from({ length: cols }, (__, j) =>
            U.reduce((s, u) => s + u[o] * u.reduce((ss, uu, oo) => ss + uu * M[oo][j], 0), 0)));
        for (let o = 0; o < c.cout; o++)
        {
            let col = 0;
            for (let k = 0; k < c.K; k++) for (let i = 0; i < c.cin; i++) c.W[k][o][i] = projected[o][col++];
        }
        return c;
    });
}

function energyRank(c, fraction)
{
    const { vals } = symEig(gram(flatten(c)));
    const total = vals.reduce((a, b) => a + b, 0);
    let acc = 0;
    for (let r = 0; r < vals.length; r++)
    {
        acc += vals[r];
        if (acc >= fraction * total) return r + 1;
    }
    return vals.length;
}

// ---------- metrics ----------

function compare(ref, y)
{
    let e = 0, s = 0;
    for (let t = LEAD; t < ref.length; t++) { e += (y[t] - ref[t]) ** 2; s += ref[t] ** 2; }

    const F = 960; // 20 ms
    const frames = [];
    for (let t = LEAD; t + F <= ref.length; t += F)
    {
        let fe = 0, fs2 = 0;
        for (let i = t; i < t + F; i++) { fe += (y[i] - ref[i]) ** 2; fs2 += ref[i] ** 2; }
        frames.push({ fe, fs2 });
    }
    frames.sort((a, b) => a.fs2 - b.fs2);
    const quiet = frames.slice(0, Math.floor(frames.length / 4));
    const qe = quiet.reduce((a, f) => a + f.fe, 0);
    const qs = quiet.reduce((a, f) => a + f.fs2, 0);

    return { snr: 10 * Math.log10(s / e), quietSnr: 10 * Math.log10(qs / qe) };
}

// ---------- reference renderer ----------

function buildRender()
{
    if (!fs.existsSync(namSourceDir))
    {
        throw new Error(`${namSourceDir} is missing: configure core/build first so it fetches the pinned NAM core`);
    }

    const configure = ['-S', namSourceDir, '-B', renderBuildDir];

    if (process.platform === 'win32')
    {
        configure.push('-G', 'Visual Studio 18 2026', '-A', 'x64', '-T', 'ClangCL',
            '-DCMAKE_CXX_STANDARD_LIBRARIES=avrt.lib kernel32.lib user32.lib');
    }
    else
    {
        configure.push('-DCMAKE_BUILD_TYPE=Release');
    }

    execFileSync('cmake', configure, { stdio: 'inherit' });
    execFileSync('cmake', ['--build', renderBuildDir, '--target', 'render', '--config', 'Release'], { stdio: 'inherit' });
}

function findRender(explicit)
{
    const candidates = explicit
        ? [explicit]
        : [path.join(renderBuildDir, 'tools', 'Release', renderExeName), path.join(renderBuildDir, 'tools', renderExeName)];
    return candidates.find((p) => fs.existsSync(p)) || null;
}

// ---------- main ----------

function main()
{
    const args = parseArgs(process.argv.slice(2));

    if (args.help)
    {
        console.log('Usage: node tools/nam-compression.mjs [--quick] [--build-render] [--render <exe>] [model.nam ...]');
        return;
    }

    if (args.buildRender) buildRender();

    const renderExe = findRender(args.render);
    if (!renderExe) console.log('C++ render tool not found: skipping validation (build it with --build-render).');

    fs.mkdirSync(workDir, { recursive: true });
    const input = makeSignal(1234);
    const cal = makeSignal(98765);
    const inWav = path.join(workDir, 'di_test.wav');
    const outWav = path.join(workDir, 'render_out.wav');
    writeWavF32(inWav, input);

    for (const rel of args.models)
    {
        const file = path.isAbsolute(rel) ? rel : path.join(modelsRoot, rel);
        const model = loadModel(file);

        if (model.skip)
        {
            console.log(`\n=== ${path.basename(file)}: skipped, ${model.skip}`);
            continue;
        }

        console.log(`\n=== ${path.basename(file)}  (${model.nWeights} weights, about that many multiply-adds per sample)`);
        const ref = forward(model, input);

        if (renderExe)
        {
            execFileSync(renderExe, [file, inWav, outWav], { stdio: 'ignore' });
            const v = compare(ref, readWavF32(outWav));
            const ok = v.snr > 100;
            console.log(`validation: JS reference vs C++ render ${v.snr.toFixed(1)} dB${ok ? '' : '  <-- MISMATCH, numbers below are not trustworthy'}`);
        }

        const maxes = calibrateSites(model, cal);
        const variants = [
            ['fp32', () => [roundWeights(model, Math.fround), { store: Math.fround, qin: floatActivations(Math.fround) }]],
            ['fast_tanh', () => [model, { act: fastTanh }]],
            ['bf16 W+A, fp32 accumulate', () => [roundWeights(model, bf16), { qin: floatActivations(bf16) }]],
            ['fp16 W+A, fp32 accumulate', () => [roundWeights(model, fp16), { qin: floatActivations(fp16) }]],
            ['fp16 everything', () => [roundWeights(model, fp16), { qin: floatActivations(fp16), store: fp16, macRound: fp16 }], true],
            ['int8 weights only', () => [quantWeightsPerChannel(model, 8), {}]],
            ['int4 weights only', () => [quantWeightsPerChannel(model, 4), {}]],
            ['int8 weights, int16 activations', () => [quantWeightsPerChannel(model, 8), { qin: intActivations(maxes, 16) }]],
            ['int8 weights, int8 activations', () => [quantWeightsPerChannel(model, 8), { qin: intActivations(maxes, 8) }]],
            ...[12, 8, 4].map((r) => [`low rank r=${r}, array 1`, () => [lowRank(model, r), {}]]),
        ];

        console.log(`${'variant'.padEnd(34)}${'SNR dB'.padStart(8)}${'quiet SNR'.padStart(11)}`);

        for (const [name, make, slow] of variants)
        {
            if (slow && args.quick) continue;
            const [m, opts] = make();
            const r = compare(ref, forward(m, input, opts));
            console.log(`${name.padEnd(34)}${r.snr.toFixed(1).padStart(8)}${r.quietSnr.toFixed(1).padStart(11)}`);
        }

        model.arrays.forEach((a, i) =>
        {
            const conv99 = a.layers.map((l) => energyRank(l.conv, 0.99)).join(',');
            const pointwise99 = a.layers.map((l) => energyRank(l.l1x1, 0.99)).join(',');
            console.log(`array ${i + 1} (${a.C} channels), rank for 99% of weight energy: conv ${conv99}; 1x1 ${pointwise99}`);
        });
    }
}

main();
