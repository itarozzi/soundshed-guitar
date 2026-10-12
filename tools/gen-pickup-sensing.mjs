#!/usr/bin/env node
/**
 * Pickup Match's string-sensing profiles: what a pickup's place on the string and its sensing
 * width do to the sound, averaged over the notes a guitar plays so they hold for chords.
 *
 * A pickup at distance p from the bridge saddle on scale L senses harmonic n of a string in
 * proportion to sin(pi n p / L): near the bridge the low harmonics, and the fundamental most of
 * all, barely move the string, which is why a bridge pickup sounds thin and a neck pickup full.
 * A coil senses a window of width w around p, multiplying by sinc(pi n w / 2L); a humbucker
 * is two windows d apart, adding 2 cos(pi n d / 2L), which takes the upper harmonics down.
 *
 * Each geometry's response is averaged over the notes from low E to the high E's 12th fret,
 * one per semitone, and over neighbouring harmonics so the comb becomes its envelope. It is
 * set to 0 dB across 500 Hz-2 kHz (position reshapes, the electrics set level) and fitted with a
 * low shelf, a peak and a high shelf, written to core/src/dsp/effects/PickupSensing.generated.h.
 *
 * The geometries are typical, not measured: Fender 648 mm and Gibson 628 mm scales; neck,
 * middle and bridge at 160/100/41 mm (single coils) and 152/100/45 mm (P-90s and humbuckers);
 * coil windows of 8 mm (single coil), 13 mm (P-90) and 7 mm (each humbucker coil, 18 mm apart).
 *
 *   node tools/gen-pickup-sensing.mjs          # regenerate
 *   node tools/gen-pickup-sensing.mjs --check  # fail if the header is out of date (CI)
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, 'core', 'src', 'dsp', 'effects', 'PickupSensing.generated.h');
const SAMPLE_RATE = 48000;

// In SensingProfile order (PickupSensing.h): family-major, then neck, middle, bridge.
const FAMILIES = [
    { name: 'SingleCoil', label: 'single coil', L: 648, positions: [160, 100, 41], w: 8, d: 0 },
    { name: 'P90', label: 'P-90', L: 628, positions: [152, 100, 45], w: 13, d: 0 },
    { name: 'Humbucker', label: 'humbucker', L: 628, positions: [152, 100, 45], w: 7, d: 18 },
];
const POSITIONS = ['Neck', 'Middle', 'Bridge'];

const sinc = (x) => (Math.abs(x) < 1e-9 ? 1 : Math.sin(x) / x);

function sensing(g, n)
{
    const position = Math.sin((Math.PI * n * g.p) / g.L);
    const window = sinc((Math.PI * n * g.w) / (2 * g.L));
    const pair = g.d > 0 ? Math.cos((Math.PI * n * g.d) / (2 * g.L)) : 1;
    return position * window * pair;
}

const NOTES = Array.from({ length: 37 }, (_, i) => 82.41 * Math.pow(2, i / 12));

function envelopeDb(g, f)
{
    // Below the lowest fundamental there is nothing to sense: hold the lowest note's response.
    const frequency = Math.max(f, NOTES[0]);
    let power = 0;
    let count = 0;

    for (const f0 of NOTES)
    {
        if (frequency < f0) continue;
        const n = frequency / f0;
        let local = 0;
        const steps = 16;
        for (let k = 0; k < steps; k++)
        {
            local += sensing(g, Math.max(1, n - 0.5 + (k + 0.5) / steps)) ** 2;
        }
        power += local / steps;
        count += 1;
    }

    return 10 * Math.log10(power / count);
}

// 1/6 octave from 40 Hz to 16 kHz: the fit's grid.
const GRID = Array.from({ length: 53 }, (_, i) => 40 * Math.pow(2, i / 6));

function targetCurve(g)
{
    const curve = GRID.map((f) => envelopeDb(g, f));
    const ref = curve.filter((_, i) => GRID[i] >= 500 && GRID[i] <= 2000);
    const mean = ref.reduce((a, b) => a + b, 0) / ref.length;
    return curve.map((v) => v - mean);
}

// RBJ designs, as core/src/dsp/BiquadDesign.h has them.
function design(type, f, q, gainDb)
{
    const A = Math.pow(10, gainDb / 40);
    const w0 = (2 * Math.PI * f) / SAMPLE_RATE;
    const cos = Math.cos(w0);
    const alpha = Math.sin(w0) / (2 * q);
    let b0, b1, b2, a0, a1, a2;

    if (type === 'peak')
    {
        b0 = 1 + alpha * A; b1 = -2 * cos; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cos; a2 = 1 - alpha / A;
    }
    else
    {
        const s = 2 * Math.sqrt(A) * alpha;
        const sign = type === 'low' ? 1 : -1;
        b0 = A * ((A + 1) - sign * (A - 1) * cos + s);
        b1 = sign * 2 * A * ((A - 1) - sign * (A + 1) * cos);
        b2 = A * ((A + 1) - sign * (A - 1) * cos - s);
        a0 = (A + 1) + sign * (A - 1) * cos + s;
        a1 = -sign * 2 * ((A - 1) + sign * (A + 1) * cos);
        a2 = (A + 1) + sign * (A - 1) * cos - s;
    }

    return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

function responseDb(c, f)
{
    const w = (2 * Math.PI * f) / SAMPLE_RATE;
    const re = (k) => Math.cos(k * w);
    const im = (k) => -Math.sin(k * w);
    const nr = c[0] + c[1] * re(1) + c[2] * re(2);
    const ni = c[1] * im(1) + c[2] * im(2);
    const dr = 1 + c[3] * re(1) + c[4] * re(2);
    const di = c[3] * im(1) + c[4] * im(2);
    return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

const SHELF_Q = 0.7071;

// Parameters: [lowHz, lowDb, peakHz, peakDb, peakQ, highHz, highDb], searched in log space for
// the frequencies and Q.
function bandsOf(x)
{
    return [
        { type: 'low', hz: Math.exp(x[0]), q: SHELF_Q, db: x[1] },
        { type: 'peak', hz: Math.exp(x[2]), q: Math.exp(x[4]), db: x[3] },
        { type: 'high', hz: Math.exp(x[5]), q: SHELF_Q, db: x[6] },
    ];
}

function cost(x, target)
{
    const bands = bandsOf(x);
    if (bands[0].hz < 30 || bands[0].hz > 2000 || bands[1].hz < 60 || bands[1].hz > 12000 || bands[2].hz < 800 ||
        bands[2].hz > 16000 || bands[1].q < 0.2 || bands[1].q > 4)
    {
        return 1e9;
    }
    const coeffs = bands.map((b) => design(b.type, b.hz, b.q, b.db));
    let sum = 0;
    GRID.forEach((f, i) => {
        const model = coeffs.reduce((a, c) => a + responseDb(c, f), 0);
        sum += (model - target[i]) ** 2;
    });
    return Math.sqrt(sum / GRID.length);
}

/// Nelder-Mead from a fixed start: deterministic, so --check can compare.
function fit(target)
{
    const start = [Math.log(250), target[GRID.findIndex((f) => f >= 100)], Math.log(1200), 0, Math.log(0.7),
                   Math.log(5000), target[GRID.findIndex((f) => f >= 8000)]];
    const scales = [0.5, 3, 0.5, 2, 0.4, 0.5, 3];
    let simplex = [start, ...start.map((_, i) => start.map((v, j) => (i === j ? v + scales[i] : v)))];
    let values = simplex.map((x) => cost(x, target));

    for (let iteration = 0; iteration < 6000; iteration++)
    {
        const order = values.map((v, i) => i).sort((a, b) => values[a] - values[b]);
        simplex = order.map((i) => simplex[i]);
        values = order.map((i) => values[i]);
        const n = start.length;
        const centroid = start.map((_, j) => simplex.slice(0, n).reduce((a, x) => a + x[j], 0) / n);
        const along = (t) => centroid.map((c, j) => c + t * (simplex[n][j] - c));
        const reflected = along(-1);
        const fr = cost(reflected, target);

        if (fr < values[0])
        {
            const expanded = along(-2);
            const fe = cost(expanded, target);
            [simplex[n], values[n]] = fe < fr ? [expanded, fe] : [reflected, fr];
        }
        else if (fr < values[n - 1])
        {
            [simplex[n], values[n]] = [reflected, fr];
        }
        else
        {
            const contracted = along(0.5);
            const fc = cost(contracted, target);
            if (fc < values[n])
            {
                [simplex[n], values[n]] = [contracted, fc];
            }
            else
            {
                for (let i = 1; i <= n; i++)
                {
                    simplex[i] = simplex[i].map((v, j) => simplex[0][j] + 0.5 * (v - simplex[0][j]));
                    values[i] = cost(simplex[i], target);
                }
            }
        }
    }

    const best = values.indexOf(Math.min(...values));
    return { bands: bandsOf(simplex[best]), errorDb: values[best] };
}

const rows = [];
const report = [];

for (const family of FAMILIES)
{
    family.positions.forEach((p, i) => {
        const target = targetCurve({ L: family.L, p, w: family.w, d: family.d });
        const { bands, errorDb } = fit(target);
        const at = (hz) => target[GRID.findIndex((f) => f >= hz)].toFixed(1);
        const r = (v, d) => Number(v.toFixed(d));
        rows.push(`    // ${family.label}, ${POSITIONS[i].toLowerCase()} (${p} mm on ${family.L} mm): ` +
                  `${at(100)} dB at 100 Hz, ${at(8000)} dB at 8 kHz; fit ${errorDb.toFixed(2)} dB RMS\n` +
                  `    {{{{${r(bands[0].hz, 1)}, ${r(bands[0].q, 4)}, ${r(bands[0].db, 2)}}, ` +
                  `{${r(bands[1].hz, 1)}, ${r(bands[1].q, 3)}, ${r(bands[1].db, 2)}}, ` +
                  `{${r(bands[2].hz, 1)}, ${r(bands[2].q, 4)}, ${r(bands[2].db, 2)}}}}},`);
        report.push(`${family.name}${POSITIONS[i]}: fit ${errorDb.toFixed(2)} dB`);
    });
}

const header = `// Generated by tools/gen-pickup-sensing.mjs - do not edit by hand.
// Regenerate with: node tools/gen-pickup-sensing.mjs
#pragma once

#include <array>

namespace guitarfx::pickup_match
{
/// One shelf or peak of a sensing profile: frequency in Hz, Q, gain in dB.
struct SensingBand
{
    double frequencyHz;
    double q;
    double gainDb;
};

/// A geometry's averaged sensing response as a low shelf, a peak and a high shelf.
struct SensingFit
{
    std::array<SensingBand, 3> bands;
};

/// In SensingProfile order: single coil, P-90, humbucker; each neck, middle, bridge.
inline constexpr std::array<SensingFit, ${rows.length}> kSensingFits = {{
${rows.join('\n')}
}};
} // namespace guitarfx::pickup_match
`.replace(/\n/g, '\r\n');

if (process.argv.includes('--check'))
{
    const current = fs.existsSync(OUT) ? fs.readFileSync(OUT, 'utf8') : '';
    if (current.replace(/\r\n/g, '\n') !== header.replace(/\r\n/g, '\n'))
    {
        console.error(`[gen-pickup-sensing] ${path.relative(ROOT, OUT)} is out of date: run node tools/gen-pickup-sensing.mjs`);
        process.exit(1);
    }
    console.log('[gen-pickup-sensing] up to date');
}
else
{
    fs.writeFileSync(OUT, header);
    console.log(`[gen-pickup-sensing] wrote ${path.relative(ROOT, OUT)}: ${report.join(', ')}`);
}
