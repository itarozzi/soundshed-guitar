#!/usr/bin/env node
/**
 * Simple HTML assembler for ui-components.
 * Replaces markers like:
 *   <!--#include:header-icon-bar.html-->
 *   <!--#include:ui-components/control-bar.html-->
 *   <!--#include:main-content/visualizer.html-->
 *
 * Usage:
 *   node scripts/assemble-html.js
 *
 * It reads index.template.html if present, else index.html as input.
 * Writes the result to index.html (in-place for the ui root, matching WebView expectations).
 *
 * Run automatically as part of `npm run build`.
 *
 * It also fills in the Content-Security-Policy: the template's
 * {{CSP_INLINE_SCRIPT_HASHES}} placeholder becomes a 'sha256-…' source for every
 * inline <script> that follows the policy, so an inline script can be edited
 * without touching the policy by hand.
 */

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const CSP_HASHES_PLACEHOLDER = '{{CSP_INLINE_SCRIPT_HASHES}}';

const ROOT = __dirname.replace(/[\\/]scripts$/, '');
const COMPONENTS_DIR = path.join(ROOT, 'ui-components');
const TEMPLATE_CANDIDATES = [
  path.join(ROOT, 'index.template.html'),
  path.join(ROOT, 'index.html'),
];
const OUTPUT = path.join(ROOT, 'index.html');

function findTemplate() {
  for (const p of TEMPLATE_CANDIDATES) {
    if (fs.existsSync(p)) return p;
  }
  throw new Error('No index.template.html or index.html found');
}

function resolveInclude(includePath) {
  // Allow both "foo.html" and "ui-components/foo.html" or "main-content/bar.html"
  const candidates = [
    path.join(COMPONENTS_DIR, includePath),
    path.join(COMPONENTS_DIR, path.basename(includePath)),
    path.join(ROOT, includePath),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  return null;
}

function processIncludes(html, seen = new Set()) {
  const re = /<!--\s*#include:\s*([^\s>]+?)\s*-->/g;
  return html.replace(re, (match, includeSpec) => {
    const filePath = resolveInclude(includeSpec);
    if (!filePath) {
      console.warn(`[assemble-html] Warning: include not found: ${includeSpec}`);
      return match;
    }
    const abs = path.resolve(filePath);
    if (seen.has(abs)) {
      console.warn(`[assemble-html] Warning: circular include detected for ${includeSpec}`);
      return '';
    }
    seen.add(abs);
    let content = fs.readFileSync(filePath, 'utf8');
    // Recurse for nested includes
    content = processIncludes(content, seen);
    seen.delete(abs);
    return content;
  });
}

/**
 * The CSP hash source for an inline script's text. The browser hashes the text as
 * the HTML parser leaves it, which has turned every CRLF into LF; the working tree
 * is CRLF on Windows, so hashing the raw file would never match.
 */
function inlineScriptHash(scriptText) {
  const normalized = scriptText.replace(/\r\n?/g, '\n');
  return `'sha256-${crypto.createHash('sha256').update(normalized, 'utf8').digest('base64')}'`;
}

/** Replaces the policy's placeholder with the hashes of the inline scripts after it. */
function applyCspScriptHashes(html) {
  const at = html.indexOf(CSP_HASHES_PLACEHOLDER);
  if (at < 0) {
    return html;
  }
  const hashes = [];
  const inlineScript = /<script(\s[^>]*)?>([\s\S]*?)<\/script>/gi;
  inlineScript.lastIndex = at;
  for (let match = inlineScript.exec(html); match; match = inlineScript.exec(html)) {
    const attributes = match[1] || '';
    if (!/\ssrc\s*=/i.test(attributes)) {
      hashes.push(inlineScriptHash(match[2]));
    }
  }
  return html.slice(0, at) + hashes.join(' ') + html.slice(at + CSP_HASHES_PLACEHOLDER.length);
}

/** The finished index.html text, from the template. */
function assembleHtml() {
  const inputPath = findTemplate();
  const template = fs.readFileSync(inputPath, 'utf8');
  return { inputPath, template, html: applyCspScriptHashes(processIncludes(template)) };
}

function main() {
  const { inputPath, template, html } = assembleHtml();

  // If this was the template, or we did work, write output
  fs.writeFileSync(OUTPUT, html, 'utf8');

  console.log(`[assemble-html] Assembled ${path.basename(inputPath)} -> ${path.relative(ROOT, OUTPUT)} (${template.length} -> ${html.length} bytes)`);
}

module.exports = { assembleHtml, applyCspScriptHashes, inlineScriptHash, CSP_HASHES_PLACEHOLDER };

if (require.main === module) {
  main();
}
