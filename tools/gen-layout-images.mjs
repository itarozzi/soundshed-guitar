#!/usr/bin/env node
/**
 * Writes the factory layout images: the starter knobs, backgrounds and switches the layout
 * designer offers under each category. The engine lists every image in
 * core/ui/assets/layout-images/<category>/ as a factory image filed under that category, its id the
 * file's stem (LayoutSupport.cpp, AddFactoryCategoryImages).
 *
 * Usage: node tools/gen-layout-images.mjs
 *
 * Knob faces point at 12 o'clock and turn about their centre (-135 to 135 degrees by default); a
 * base or scale ring stays still under a face, and the glare lies over it. The LED ring is a
 * filmstrip of 21 frames stacked top to bottom. Switches come in pairs, one image for each state.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "core", "ui", "assets", "layout-images");

const svg = (width, height, body) =>
  `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">\n${body.trim()}\n</svg>\n`;

const round = (value) => Math.round(value * 100) / 100;
const polar = (cx, cy, r, degrees) => {
  const radians = ((degrees - 90) * Math.PI) / 180;
  return [round(cx + r * Math.cos(radians)), round(cy + r * Math.sin(radians))];
};

/** Notches around a knob's rim, `count` of them, as a path of short radial lines. */
const knurl = (cx, cy, r1, r2, count) =>
  Array.from({ length: count }, (_, i) => {
    const angle = (360 / count) * i;
    const [x1, y1] = polar(cx, cy, r1, angle);
    const [x2, y2] = polar(cx, cy, r2, angle);
    return `M${x1} ${y1}L${x2} ${y2}`;
  }).join("");

const images = {
  knob: {
    "knob-chicken-head": svg(256, 256, `
<defs>
  <radialGradient id="body" cx="40%" cy="30%" r="75%"><stop offset="0" stop-color="#4a4a4e"/><stop offset="0.6" stop-color="#1c1c1f"/><stop offset="1" stop-color="#0a0a0b"/></radialGradient>
  <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="5" stdDeviation="6" flood-opacity="0.55"/></filter>
</defs>
<g filter="url(#shadow)">
  <circle cx="128" cy="128" r="70" fill="url(#body)"/>
  <path d="M128 18 C146 18 158 70 160 128 C162 176 150 214 128 214 C106 214 94 176 96 128 C98 70 110 18 128 18 Z" fill="url(#body)" stroke="#000" stroke-opacity="0.5" stroke-width="2"/>
</g>
<path d="M128 30 L128 112" stroke="#f4f1e8" stroke-width="7" stroke-linecap="round"/>
<ellipse cx="116" cy="96" rx="14" ry="38" fill="#fff" opacity="0.08"/>`),

    "knob-silver-knurled": svg(256, 256, `
<defs>
  <radialGradient id="cap" cx="38%" cy="32%" r="80%"><stop offset="0" stop-color="#ffffff"/><stop offset="0.45" stop-color="#c9ccd1"/><stop offset="1" stop-color="#7c8088"/></radialGradient>
  <linearGradient id="rim" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#e9ebee"/><stop offset="0.5" stop-color="#8d9199"/><stop offset="1" stop-color="#4f535a"/></linearGradient>
  <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="4" stdDeviation="5" flood-opacity="0.5"/></filter>
</defs>
<circle cx="128" cy="128" r="100" fill="url(#rim)" filter="url(#shadow)"/>
<path d="${knurl(128, 128, 88, 100, 72)}" stroke="#3d4046" stroke-opacity="0.55" stroke-width="2"/>
<circle cx="128" cy="128" r="84" fill="url(#cap)" stroke="#5f636a" stroke-width="2"/>
<circle cx="128" cy="128" r="62" fill="none" stroke="#ffffff" stroke-opacity="0.35" stroke-width="1.5"/>
<path d="M128 48 L128 92" stroke="#1d1f23" stroke-width="7" stroke-linecap="round"/>`),

    "knob-vintage-cream": svg(256, 256, `
<defs>
  <radialGradient id="cap" cx="40%" cy="32%" r="80%"><stop offset="0" stop-color="#fffaf0"/><stop offset="0.55" stop-color="#ece0c4"/><stop offset="1" stop-color="#b8a582"/></radialGradient>
  <linearGradient id="skirt" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f3e9d2"/><stop offset="1" stop-color="#a8956f"/></linearGradient>
  <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="5" stdDeviation="6" flood-opacity="0.5"/></filter>
</defs>
<circle cx="128" cy="128" r="104" fill="url(#skirt)" filter="url(#shadow)"/>
<circle cx="128" cy="128" r="104" fill="none" stroke="#7d6a48" stroke-width="2"/>
<circle cx="128" cy="128" r="70" fill="url(#cap)" stroke="#8c7a55" stroke-width="2"/>
<path d="${knurl(128, 128, 60, 68, 40)}" stroke="#8c7a55" stroke-opacity="0.45" stroke-width="2"/>
<path d="M128 28 L128 86" stroke="#1a1712" stroke-width="6" stroke-linecap="round"/>`),

    "knob-pedal-black": svg(256, 256, `
<defs>
  <linearGradient id="side" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a3a3e"/><stop offset="1" stop-color="#0d0d0f"/></linearGradient>
  <radialGradient id="top" cx="42%" cy="35%" r="75%"><stop offset="0" stop-color="#3c3c41"/><stop offset="1" stop-color="#151517"/></radialGradient>
  <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="4" stdDeviation="5" flood-opacity="0.6"/></filter>
</defs>
<circle cx="128" cy="128" r="96" fill="url(#side)" filter="url(#shadow)"/>
<path d="${knurl(128, 128, 84, 96, 36)}" stroke="#000" stroke-opacity="0.6" stroke-width="5"/>
<circle cx="128" cy="128" r="78" fill="url(#top)" stroke="#000" stroke-opacity="0.4" stroke-width="2"/>
<circle cx="128" cy="62" r="8" fill="#f2f2f2"/>`),

    "knob-scale-ring": svg(256, 256, Array.from({ length: 11 }, (_, i) => {
      const angle = -135 + 27 * i;
      const [x1, y1] = polar(128, 128, 112, angle);
      const [x2, y2] = polar(128, 128, i % 5 === 0 ? 124 : 120, angle);
      return `<path d="M${x1} ${y1}L${x2} ${y2}" stroke="#e8e8e8" stroke-width="${i % 5 === 0 ? 4 : 3}" stroke-linecap="round"/>`;
    }).join("\n") + `\n<path d="${(() => {
      const [x1, y1] = polar(128, 128, 106, -135);
      const [x2, y2] = polar(128, 128, 106, 135);
      return `M${x1} ${y1}A106 106 0 1 1 ${x2} ${y2}`;
    })()}" fill="none" stroke="#e8e8e8" stroke-opacity="0.35" stroke-width="2"/>`),

    "knob-glare": svg(256, 256, `
<defs>
  <radialGradient id="glare" cx="38%" cy="28%" r="55%"><stop offset="0" stop-color="#fff" stop-opacity="0.55"/><stop offset="0.5" stop-color="#fff" stop-opacity="0.12"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
</defs>
<circle cx="128" cy="128" r="96" fill="url(#glare)"/>`),

    "knob-led-ring-21f": (() => {
      const frames = 21;
      const size = 128;
      const leds = 15;
      const body = Array.from({ length: frames }, (_, frame) => {
        const lit = Math.round((frame / (frames - 1)) * leds);
        const cy = size / 2 + frame * size;
        const dots = Array.from({ length: leds }, (_, i) => {
          const [x, y] = polar(size / 2, cy, 52, -135 + (270 / (leds - 1)) * i);
          const on = i < lit;
          return `<circle cx="${x}" cy="${y}" r="4.5" fill="${on ? "#ff9a2e" : "#3a2a1c"}"${on ? ` filter="url(#glow)"` : ""}/>`;
        }).join("");
        return `<circle cx="${size / 2}" cy="${cy}" r="38" fill="url(#cap)" stroke="#000" stroke-opacity="0.6" stroke-width="2"/>${dots}`;
      }).join("\n");
      return svg(size, size * frames, `
<defs>
  <radialGradient id="cap" cx="42%" cy="35%" r="75%"><stop offset="0" stop-color="#46464c"/><stop offset="1" stop-color="#121214"/></radialGradient>
  <filter id="glow" x="-100%" y="-100%" width="300%" height="300%"><feGaussianBlur stdDeviation="2" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
</defs>
${body}`);
    })(),
  },

  background: {
    "bg-brushed-aluminium": svg(800, 560, `
<defs>
  <linearGradient id="sheen" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#d7dade"/><stop offset="0.5" stop-color="#b3b7bd"/><stop offset="1" stop-color="#9ca1a8"/></linearGradient>
  <filter id="brush"><feTurbulence type="fractalNoise" baseFrequency="0.002 0.9" numOctaves="2" seed="4"/><feColorMatrix values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 0.22 0"/></filter>
</defs>
<rect width="800" height="560" fill="url(#sheen)"/>
<rect width="800" height="560" filter="url(#brush)"/>
<rect x="1" y="1" width="798" height="558" fill="none" stroke="#6d7279" stroke-width="2"/>`),

    "bg-black-tolex": svg(800, 560, `
<defs>
  <filter id="grain"><feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="3" seed="7"/><feColorMatrix values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 0.09 0"/></filter>
  <radialGradient id="light" cx="50%" cy="30%" r="80%"><stop offset="0" stop-color="#2a2a2d"/><stop offset="1" stop-color="#121214"/></radialGradient>
</defs>
<rect width="800" height="560" fill="url(#light)"/>
<rect width="800" height="560" filter="url(#grain)"/>
<rect x="14" y="14" width="772" height="532" rx="10" fill="none" stroke="#bfa36a" stroke-opacity="0.55" stroke-width="3"/>`),

    "bg-walnut": svg(800, 560, `
<defs>
  <filter id="grain"><feTurbulence type="fractalNoise" baseFrequency="0.0025 0.2" numOctaves="5" seed="11"/><feColorMatrix values="0 0 0 0 0.16  0 0 0 0 0.07  0 0 0 0 0.02  0 0 0 1.6 -0.35"/></filter>
  <linearGradient id="tone" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#6b3f22"/><stop offset="0.5" stop-color="#8a5530"/><stop offset="1" stop-color="#5e361d"/></linearGradient>
</defs>
<rect width="800" height="560" fill="url(#tone)"/>
<rect width="800" height="560" filter="url(#grain)" opacity="0.9"/>`),

    "bg-red-pedal": svg(800, 560, `
<defs>
  <linearGradient id="paint" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#d23a2c"/><stop offset="1" stop-color="#8f1d15"/></linearGradient>
  <filter id="powder"><feTurbulence type="fractalNoise" baseFrequency="1.2" numOctaves="2" seed="3"/><feColorMatrix values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 0.08 0"/></filter>
  <radialGradient id="screw" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#f0f0f0"/><stop offset="1" stop-color="#7a7a7a"/></radialGradient>
</defs>
<rect width="800" height="560" rx="36" fill="url(#paint)"/>
<rect width="800" height="560" rx="36" filter="url(#powder)"/>
<rect x="3" y="3" width="794" height="554" rx="34" fill="none" stroke="#ffffff" stroke-opacity="0.18" stroke-width="4"/>
${[[40, 40], [760, 40], [40, 520], [760, 520]].map(([x, y]) => `<circle cx="${x}" cy="${y}" r="11" fill="url(#screw)"/><path d="M${x - 6} ${y}h12M${x} ${y - 6}v12" stroke="#555" stroke-width="2"/>`).join("")}`),

    "bg-tweed": svg(800, 560, `
<defs>
  <pattern id="weave" width="12" height="12" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
    <rect width="12" height="12" fill="#d9c48f"/>
    <rect width="6" height="12" fill="#b99c5e"/>
    <path d="M0 3h12M0 9h12" stroke="#6e5328" stroke-opacity="0.45" stroke-width="1.4"/>
  </pattern>
  <filter id="fleck"><feTurbulence type="fractalNoise" baseFrequency="0.7" numOctaves="2" seed="5"/><feColorMatrix values="0 0 0 0 0.25  0 0 0 0 0.18  0 0 0 0 0.08  0 0 0 0.25 0"/></filter>
</defs>
<rect width="800" height="560" fill="url(#weave)"/>
<rect width="800" height="560" filter="url(#fleck)"/>`),
  },

  switch: (() => {
    const bat = (up) => svg(128, 180, `
<defs>
  <radialGradient id="nut" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#f5f6f8"/><stop offset="1" stop-color="#80848b"/></radialGradient>
  <linearGradient id="lever" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#8a8e95"/><stop offset="0.45" stop-color="#ffffff"/><stop offset="1" stop-color="#6c7077"/></linearGradient>
</defs>
<polygon points="64,52 98,72 98,108 64,128 30,108 30,72" fill="url(#nut)" stroke="#5c6067" stroke-width="2"/>
<circle cx="64" cy="90" r="16" fill="#3b3e44"/>
<g transform="rotate(${up ? 0 : 180} 64 90)">
  <path d="M58 90 L54 20 Q64 10 74 20 L70 90 Z" fill="url(#lever)" stroke="#55595f" stroke-width="1.5"/>
  <ellipse cx="64" cy="20" rx="10" ry="7" fill="url(#lever)" stroke="#55595f" stroke-width="1.5"/>
</g>`);
    const footswitch = (on) => svg(160, 200, `
<defs>
  <radialGradient id="cap" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#ffffff"/><stop offset="0.5" stop-color="#c3c7cd"/><stop offset="1" stop-color="#6e737a"/></radialGradient>
  <radialGradient id="led" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="${on ? "#ffd0c8" : "#6b2a24"}"/><stop offset="1" stop-color="${on ? "#ff2a12" : "#3a1310"}"/></radialGradient>
  <filter id="glow" x="-100%" y="-100%" width="300%" height="300%"><feGaussianBlur stdDeviation="${on ? 5 : 0}"/></filter>
</defs>
${on ? `<circle cx="80" cy="28" r="14" fill="#ff3a1e" opacity="0.6" filter="url(#glow)"/>` : ""}
<circle cx="80" cy="28" r="10" fill="url(#led)" stroke="#1c1c1c" stroke-width="2"/>
<circle cx="80" cy="124" r="54" fill="#2b2d31" stroke="#16171a" stroke-width="2"/>
<circle cx="80" cy="${on ? 127 : 124}" r="40" fill="url(#cap)" stroke="#5e6269" stroke-width="2"/>
<path d="${knurl(80, on ? 127 : 124, 30, 38, 28)}" stroke="#4d5157" stroke-opacity="0.5" stroke-width="2"/>`);
    const led = (on) => svg(96, 96, `
<defs>
  <radialGradient id="lens" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="${on ? "#ffe0d6" : "#7a2c25"}"/><stop offset="0.6" stop-color="${on ? "#ff3418" : "#4a1814"}"/><stop offset="1" stop-color="${on ? "#a8130a" : "#2a0d0b"}"/></radialGradient>
  <radialGradient id="bezel" cx="40%" cy="35%" r="70%"><stop offset="0" stop-color="#f0f1f3"/><stop offset="1" stop-color="#6f737a"/></radialGradient>
  <filter id="glow" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="8"/></filter>
</defs>
${on ? `<circle cx="48" cy="48" r="34" fill="#ff3a1e" opacity="0.55" filter="url(#glow)"/>` : ""}
<circle cx="48" cy="48" r="30" fill="url(#bezel)" stroke="#4f535a" stroke-width="2"/>
<circle cx="48" cy="48" r="21" fill="url(#lens)"/>
<ellipse cx="41" cy="40" rx="7" ry="5" fill="#fff" opacity="${on ? 0.7 : 0.25}"/>`);
    const rocker = (on) => svg(96, 160, `
<defs>
  <linearGradient id="up" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a3b3f"/><stop offset="1" stop-color="#1a1b1d"/></linearGradient>
  <linearGradient id="down" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#121214"/><stop offset="1" stop-color="#2c2d31"/></linearGradient>
</defs>
<rect x="8" y="8" width="80" height="144" rx="10" fill="#0c0c0d" stroke="#3a3a3e" stroke-width="2"/>
<rect x="16" y="16" width="64" height="64" rx="6" fill="url(#${on ? "down" : "up"})"/>
<rect x="16" y="80" width="64" height="64" rx="6" fill="url(#${on ? "up" : "down"})"/>
<path d="M48 34 V62" stroke="${on ? "#f2f2f2" : "#8a8a8a"}" stroke-width="5" stroke-linecap="round"/>
<circle cx="48" cy="112" r="13" fill="none" stroke="${on ? "#8a8a8a" : "#f2f2f2"}" stroke-width="5"/>`);
    return {
      "switch-bat-up": bat(true),
      "switch-bat-down": bat(false),
      "switch-footswitch-on": footswitch(true),
      "switch-footswitch-off": footswitch(false),
      "switch-led-on": led(true),
      "switch-led-off": led(false),
      "switch-rocker-on": rocker(true),
      "switch-rocker-off": rocker(false),
    };
  })(),
};

let written = 0;
for (const [category, files] of Object.entries(images)) {
  const dir = path.join(root, category);
  fs.mkdirSync(dir, { recursive: true });
  for (const [name, content] of Object.entries(files)) {
    fs.writeFileSync(path.join(dir, `${name}.svg`), content);
    written++;
  }
}
console.log(`Wrote ${written} factory layout images under ${path.relative(process.cwd(), root)}`);
