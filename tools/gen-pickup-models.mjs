#!/usr/bin/env node
/**
 * Pickup Match's per-model table, fitted from measured pickup frequency responses.
 *
 * Each source page holds one pickup's measured response, 28 Hz to 20 kHz in dB, under eight cable
 * and pot loads (`data.setValue(row, column, value)`; column 0 is the frequency and 1-8 are the
 * loads). This fits the first load, 10 ft of low-capacitance cable into 500K, with the
 * second-order resonant low-pass Pickup Match runs:
 *
 *     |H(f)| dB = level - 10 log10((1 - x^2)^2 + (x / Q)^2),   x = f / resonance
 *
 * up to 12 kHz, and writes resonance, Q, level and the RMS fit error per pickup to
 * core/src/dsp/effects/PickupModels.generated.h.
 *
 * The table is append-only, because presets store a pickup as its index. A model already in the
 * header keeps its place, even if its source disappears (it then keeps its old numbers), and new
 * models go on the end. Each model has a stable id, its slug, to match on.
 *
 *   node tools/gen-pickup-models.mjs <dir holding pickups.txt and the [N] Name.txt pages>
 *
 * The measurements are not in this repository, so there is no CI drift check: the header is the
 * record.
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT = path.join(ROOT, 'core', 'src', 'dsp', 'effects', 'PickupModels.generated.h');
const FIT_LIMIT_HZ = 12000;

const sourceDir = process.argv[2];

if (!sourceDir || !fs.existsSync(path.join(sourceDir, 'pickups.txt')))
{
    console.error('usage: node tools/gen-pickup-models.mjs <dir holding pickups.txt>');
    process.exit(2);
}

// Types the names do not give away, by source index. Everything else is read from the name.
const FAMILY_OVERRIDES = {
    1: 'Other', 2: 'Other', 3: 'Other', // switch positions of a whole guitar, not one pickup
    19: 'Humbucker', 20: 'SingleCoil', 21: 'SingleCoil', 22: 'Humbucker',
    26: 'Active', 27: 'Active', 28: 'Active', 29: 'Active', 30: 'Active',
    36: 'Humbucker', 37: 'Humbucker',
    50: 'SingleCoil', 51: 'SingleCoil', 52: 'SingleCoil', 53: 'SingleCoil',
    62: 'SingleCoil', 63: 'SingleCoil', 64: 'Humbucker', 65: 'Humbucker',
    77: 'SingleCoil', 78: 'SingleCoil',
    82: 'Other', 83: 'Other', 84: 'Other', 85: 'Other', 86: 'Other',
    93: 'Humbucker', 94: 'Humbucker', 95: 'Humbucker', 96: 'Humbucker',
    107: 'Other', 108: 'Other', 109: 'Other', 110: 'Humbucker',
    138: 'Humbucker', 139: 'Humbucker',
    156: 'SingleCoil', 157: 'SingleCoil', 159: 'SingleCoil',
    164: 'SingleCoil', 165: 'SingleCoil', 166: 'SingleCoil',
    170: 'Humbucker', 171: 'SingleCoil', 182: 'Humbucker', 183: 'Humbucker',
    194: 'Active', 195: 'Active',
    224: 'Humbucker', 225: 'Humbucker', 226: 'Humbucker', 227: 'Humbucker', 228: 'Humbucker',
    229: 'Humbucker', 230: 'Humbucker', 231: 'SingleCoil', 232: 'SingleCoil', 233: 'SingleCoil',
    235: 'Humbucker', 238: 'SingleCoil', 239: 'SingleCoil',
    249: 'Humbucker', 250: 'Humbucker', 251: 'Other',
};

const MULTI_WORD_BRANDS = ['B.C. Rich', 'Brian Moore', 'Duncan Designed', 'Joe Barden', 'Kent Armstrong',
                           'Michael Kelley', 'Paul Reed Smith', 'Seymour Duncan'];

function familyFromName(name)
{
    const n = name.toLowerCase();

    if (/\bactive\b|\bemg\b/.test(n)) return 'Active';
    if (/p-?90|soapbar/.test(n)) return 'P90';
    if (/humbuck|bucker|filtertron|\bhb\b|hot rails/.test(n)) return 'Humbucker';
    if (/single|strat|tele/.test(n)) return 'SingleCoil';
    return null;
}

function positionFromName(name)
{
    if (/\(neck/i.test(name)) return 'Neck';
    if (/\(middle/i.test(name)) return 'Middle';
    if (/\(bridge/i.test(name)) return 'Bridge';
    return 'Any';
}

function brandFromName(name)
{
    const brand = MULTI_WORD_BRANDS.find((b) => name.startsWith(b + ' ')) ?? name.split(' ')[0];
    return brand === 'Paul Reed Smith' ? 'PRS' : brand;
}

function slug(text)
{
    return text.toLowerCase()
        .replace(/&/g, ' and ')
        .replace(/\+/g, ' plus ')
        .replace(/[^a-z0-9]+/g, '-')
        .replace(/^-|-$/g, '');
}

/// The pickup the page actually holds: its first selected option, series A. The scraper put names
/// in the query unencoded, so a "+" or "&" in a name reached the server mangled, and it answered
/// with its first pickup instead. Those pages must not pass for the pickup they were asked for.
function readSelected(file)
{
    const m = fs.readFileSync(file, 'utf8').match(/<OPTION SELECTED>([^<]*)<\/OPTION>/i);
    return m ? m[1].replace(/&amp;/g, '&').replace(/\s+/g, ' ').trim() : null;
}

/// The page's first-load curve, as [frequency, dB] pairs.
function readCurve(file)
{
    const rows = new Map();

    for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/))
    {
        const m = line.trim().match(/^data\.setValue\((\d+),\s*(\d+),\s*'?([-\d.]+)'?\);/);

        if (m && (m[2] === '0' || m[2] === '1'))
        {
            const row = rows.get(+m[1]) ?? [];
            row[+m[2]] = +m[3];
            rows.set(+m[1], row);
        }
    }

    return [...rows.keys()].sort((a, b) => a - b).map((k) => rows.get(k)).filter((r) => r.length === 2);
}

function shapeDb(f, f0, q)
{
    const x = f / f0;
    return -10 * Math.log10((1 - x * x) ** 2 + (x / q) ** 2);
}

/// Best (resonance, Q) by a coarse grid then a fine one around its winner; the level that
/// goes with them is the mean residual, which is what minimises the RMS error.
function fit(curve)
{
    const points = curve.filter(([f]) => f <= FIT_LIMIT_HZ);
    const evaluate = (f0, q) =>
    {
        const residual = points.map(([f, db]) => db - shapeDb(f, f0, q));
        const level = residual.reduce((a, b) => a + b, 0) / residual.length;
        const err = Math.sqrt(residual.reduce((a, r) => a + (r - level) ** 2, 0) / residual.length);
        return { f0, q, level, err };
    };

    let best = { err: Infinity };

    for (let f0 = 800; f0 <= 16000; f0 *= 1.02)
        for (let q = 0.3; q <= 8; q *= 1.03)
        {
            const r = evaluate(f0, q);
            if (r.err < best.err) best = r;
        }

    const coarse = best;

    for (let i = -40; i <= 40; ++i)
        for (let j = -30; j <= 30; ++j)
        {
            const r = evaluate(coarse.f0 * (1 + i * 0.0005), coarse.q * (1 + j * 0.001));
            if (r.err < best.err) best = r;
        }

    return best;
}

function sourceFileFor(rawName)
{
    return path.join(sourceDir, rawName.replace(/&/g, ' and ').replace(/\//g, '_') + '.txt');
}

// The header as it stands: its models keep their places.
function readExisting()
{
    if (!fs.existsSync(OUT)) return [];

    const models = [];
    const re = /\{"([^"]+)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", PickupFamily::(\w+), PickupPosition::(\w+), ([\d.]+), ([\d.]+), (-?[\d.]+), ([\d.]+)\}/g;

    for (const m of fs.readFileSync(OUT, 'utf8').matchAll(re))
    {
        models.push({ id: m[1], label: m[2], brand: m[3], family: m[4], position: m[5], f0: +m[6], q: +m[7],
                      level: +m[8], err: +m[9] });
    }

    return models;
}

const fresh = [];
const seenLabels = new Map();
const seenCurves = new Map();

for (const line of fs.readFileSync(path.join(sourceDir, 'pickups.txt'), 'utf8').replace(/^﻿/, '').split(/\r?\n/))
{
    const m = line.trim().match(/^\[(\d+)\] (.+)$/);

    if (!m) continue;

    const index = +m[1];
    const rawName = line.trim();
    const name = m[2].replace(/\(tm\)/gi, '').replace(/\s+/g, ' ').trim();
    const file = sourceFileFor(rawName);

    if (!fs.existsSync(file))
    {
        console.warn(`missing page for [${index}] ${name}`);
        continue;
    }

    const selected = readSelected(file);

    if (selected !== rawName.replace(/\s+/g, ' ').trim())
    {
        console.warn(`skipped [${index}] ${name}: the page holds "${selected}"`);
        continue;
    }

    const curve = readCurve(file);
    const curveKey = JSON.stringify(curve);

    if (seenCurves.has(curveKey))
    {
        console.warn(`skipped [${index}] ${name}: the same measurement as ${seenCurves.get(curveKey)}`);
        continue;
    }

    seenCurves.set(curveKey, `[${index}] ${name}`);

    if (curve.length < 40)
    {
        console.warn(`too few points for [${index}] ${name}: ${curve.length}`);
        continue;
    }

    const family = FAMILY_OVERRIDES[index] ?? familyFromName(name) ?? 'Other';
    // Case-blind, so the source's "FilterTron" and "Filtertron" become one name and its #2.
    const key = slug(name);
    const count = (seenLabels.get(key) ?? 0) + 1;
    seenLabels.set(key, count);
    const label = count > 1 ? `${name} #${count}` : name;
    const result = fit(curve);
    fresh.push({ id: slug(label), label, brand: brandFromName(name), family, position: positionFromName(name),
                 f0: result.f0, q: result.q, level: result.level, err: result.err });
}

const existing = readExisting();
const freshById = new Map(fresh.map((m) => [m.id, m]));
const models = existing.map((old) => freshById.get(old.id) ?? old);
const known = new Set(existing.map((m) => m.id));

for (const model of fresh)
{
    if (!known.has(model.id)) models.push(model);
}

const ids = new Set();

for (const model of models)
{
    if (ids.has(model.id)) throw new Error(`duplicate id ${model.id}`);
    ids.add(model.id);
}

const esc = (s) => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
const rows = models.map((m) => `    {"${m.id}", "${esc(m.label)}", "${esc(m.brand)}", PickupFamily::${m.family}, ` +
                               `PickupPosition::${m.position}, ${m.f0.toFixed(1)}, ${m.q.toFixed(3)}, ` +
                               `${m.level.toFixed(2)}, ${m.err.toFixed(2)}},`);

const header = `// Generated by tools/gen-pickup-models.mjs from measured pickup responses - do not edit by hand.
// Append-only: presets store a pickup as its index, so a model never moves once it is here.
#pragma once

#include "dsp/effects/PickupTypes.h"

#include <array>

namespace guitarfx::pickup_match
{
/// One fitted second-order response per measured pickup, into 10 ft of low-capacitance cable and
/// a 500K pot: resonance in Hz, Q, level in dB, and the fit's RMS error in dB up to 12 kHz.
inline constexpr std::array<PickupModel, ${models.length}> kMeasuredModels = {{
${rows.join('\n')}
}};
} // namespace guitarfx::pickup_match
`;

fs.writeFileSync(OUT, header.replace(/\n/g, '\r\n'));

const byFamily = {};

for (const m of models) byFamily[m.family] = (byFamily[m.family] ?? 0) + 1;

const errors = models.map((m) => m.err).sort((a, b) => a - b);
console.log(`[gen-pickup-models] wrote ${path.relative(ROOT, OUT)}: ${models.length} models ` +
            `(${models.length - existing.length} new), ${JSON.stringify(byFamily)}; ` +
            `fit error median ${errors[Math.floor(errors.length / 2)].toFixed(2)} dB, ` +
            `worst ${errors[errors.length - 1].toFixed(2)} dB`);
