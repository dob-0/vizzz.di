#!/usr/bin/env node
// Audit 2026-10-15 2-M3: the console puts Wi-Fi names (from any access point
// in range) and peer addresses inside onclick="...'X'..." attributes. The
// browser HTML-decodes the attribute, then runs it as JavaScript, so the value
// must survive both. This extracts the console's own escHtml/escJsSq/escAttrJs
// from src/main.cpp and checks, for hostile names, that (1) the attribute
// cannot be closed early and (2) after HTML decoding the JS string literal
// evaluates back to the exact name. Usage: node tools/check_console_escape.js
'use strict';
const fs = require('fs');
const path = require('path');
const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'main.cpp'), 'utf8');
const grab = (name) => {
  const m = src.match(new RegExp('^function ' + name + '\\(s\\)\\{.*\\}$', 'm'));
  if (!m) throw new Error('console function not found: ' + name);
  return m[0];
};
const { escAttrJs } = new Function(
  [grab('escHtml'), grab('escJsSq'), grab('escAttrJs'), 'return {escAttrJs};'].join('\n'))();
// Each template that renders a name into onclick must use escAttrJs, never escJsSq alone.
const bare = (src.match(/onclick="[^"\n]*\$\{escJsSq\(/g) || []).length;
// HTML attribute-value decoding (WHATWG HTML §13.2.5.38): named and numeric
// character references the console's escaper can emit, plus the &#NN; forms
// an attacker would use.
const htmlDecode = (v) => v
  .replace(/&#x([0-9a-f]+);?/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
  .replace(/&#([0-9]+);?/g, (_, d) => String.fromCodePoint(parseInt(d, 10)))
  .replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const hostile = [
  'x" onfocus="alert(1)" autofocus "', "x'+alert(1)+'", 'a&#39;+alert(1)+&#39;',
  'a\\\'+alert(1)//', '</button><img src=x onerror=alert(1)>', 'line\nbreak\rcr',
  'amp &amp; &quot; &#x27;', 'plain-ssid', '', 'back\\slash\\',
];
let fail = bare ? 1 : 0;
if (bare) console.log(`FAIL ${bare} onclick template(s) still use escJsSq without escHtml`);
for (const name of hostile) {
  const attr = escAttrJs(name);
  const closes = attr.includes('"');
  let back;
  try { back = new Function("return '" + htmlDecode(attr) + "';")(); } catch (e) { back = e; }
  const ok = !closes && back === name;
  if (!ok) { fail++; console.log('FAIL', JSON.stringify(name), '->', JSON.stringify(attr), String(back)); }
}
console.log(fail ? `console escape: ${fail} FAIL` : `console escape: PASS (${hostile.length} names)`);
process.exit(fail ? 1 : 0);
