#!/usr/bin/env node
/**
 * Copies third-party browser bundles into dist/ so the WebView can load them
 * offline. Runs automatically as `npm run postbuild`.
 */

const fs = require('fs');
const path = require('path');

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const NODE_MODULES = path.join(ROOT, 'node_modules');
const DIST = path.join(ROOT, 'dist');

const FILES = [
  ['jszip/dist/jszip.min.js', 'jszip.min.js'],
  ['alpinejs/dist/cdn.min.js', 'alpine.min.js'],
];

let copied = 0;
const missing = [];

for (const [source, target] of FILES) {
  const from = path.join(NODE_MODULES, source);
  const to = path.join(DIST, target);
  if (!fs.existsSync(from)) {
    missing.push(source);
    continue;
  }
  fs.mkdirSync(path.dirname(to), { recursive: true });
  fs.copyFileSync(from, to);
  copied += 1;
}

console.log(`[copy-vendor] copied ${copied}/${FILES.length} vendor files into dist/`);

if (missing.length > 0) {
  console.error(`[copy-vendor] missing dependencies (run npm install): ${missing.join(', ')}`);
  process.exitCode = 1;
}
