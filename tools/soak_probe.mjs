#!/usr/bin/env node
// soak_probe — watch a node for --minutes: /status every --every s. Records heap_free, heap_max_block,
// api_replies, rst, uptime/boot changes (a reboot = rst changes or fw timestamp/uptime resets), empty or
// failed replies, and the slowest reply. Prints a table at the end and a verdict: rebooted? wdt? failures?
//   node tools/soak_probe.mjs --node http://<ip> [--minutes 60] [--every 30] [--out file.json]
const a = Object.fromEntries(process.argv.slice(2).map((x, i, v) => x.startsWith('--') ? [x.slice(2), v[i + 1] && !v[i + 1].startsWith('--') ? v[i + 1] : true] : []).filter(Boolean));
const base = String(a.node || '').replace(/\/$/, ''); if (!base) { console.error('usage: --node http://<ip>'); process.exit(2); }
const minutes = Number(a.minutes ?? 60), every = Number(a.every ?? 30);
const rows = [], fails = []; let slowest = 0, firstRst = null, firstUp = null, reboots = 0;
const t0 = Date.now();
while (Date.now() - t0 < minutes * 60000) {
  const ts = new Date().toISOString().slice(11, 19); const s = Date.now();
  try {
    const ac = new AbortController(); const t = setTimeout(() => ac.abort(), 5000);
    const r = await fetch(base + '/status', { signal: ac.signal }); const txt = await r.text(); clearTimeout(t);
    const ms = Date.now() - s; slowest = Math.max(slowest, ms);
    if (!txt) { fails.push({ ts, why: 'empty reply', ms }); }
    else {
      const d = JSON.parse(txt);
      if (firstRst === null) { firstRst = d.rst; firstUp = d.uptime ?? d.up ?? null; }
      if (d.rst !== firstRst) { reboots++; fails.push({ ts, why: 'rst changed to ' + d.rst }); firstRst = d.rst; }
      if (firstUp !== null && (d.uptime ?? d.up) < firstUp) { reboots++; fails.push({ ts, why: 'uptime reset' }); }
      firstUp = d.uptime ?? d.up ?? firstUp;
      rows.push({ ts, ms, heap_free: d.heap_free, heap_max_block: d.heap_max_block, api_replies: d.api_replies, ws: d.ws_clients, sse: d.sse_clients, usb: d.usb_active, rst: d.rst });
    }
  } catch (e) { fails.push({ ts, why: String(e.message || e).slice(0, 60), ms: Date.now() - s }); }
  await new Promise((r) => setTimeout(r, every * 1000));
}
const min = (k) => Math.min(...rows.map((r) => r[k]).filter((v) => typeof v === 'number'));
const max = (k) => Math.max(...rows.map((r) => r[k]).filter((v) => typeof v === 'number'));
const out = { node: base, minutes, every, samples: rows.length, failures: fails.length, reboots, slowest_ms: slowest,
  heap_free: { min: min('heap_free'), max: max('heap_free'), first: rows[0]?.heap_free, last: rows.at(-1)?.heap_free },
  heap_max_block: { min: min('heap_max_block'), max: max('heap_max_block'), first: rows[0]?.heap_max_block, last: rows.at(-1)?.heap_max_block },
  api_replies: { first: rows[0]?.api_replies, last: rows.at(-1)?.api_replies }, rst: rows.at(-1)?.rst, fails };
if (a.out) (await import('node:fs')).writeFileSync(String(a.out), JSON.stringify({ ...out, rows }, null, 1));
console.log(`soak ${minutes} min on ${base}: ${rows.length} samples, ${fails.length} failures, ${reboots} reboots, slowest ${slowest} ms, rst ${out.rst}`);
console.log(`heap_free ${out.heap_free.min}-${out.heap_free.max} B (first ${out.heap_free.first}, last ${out.heap_free.last}); heap_max_block ${out.heap_max_block.min}-${out.heap_max_block.max} B (first ${out.heap_max_block.first}, last ${out.heap_max_block.last}); api_replies ${out.api_replies.first} -> ${out.api_replies.last}`);
for (const f of fails.slice(0, 10)) console.log(`  fail ${f.ts} ${f.why}${f.ms ? ' ' + f.ms + ' ms' : ''}`);
process.exit(reboots || fails.length > 2 ? 1 : 0);
