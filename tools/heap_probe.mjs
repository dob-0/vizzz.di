#!/usr/bin/env node
// heap_probe — read heap_free / heap_max_block / ws_clients from a node's /status
// at idle and under a load of N WebSocket clients (what N phones on /vj hold open),
// polling /status every --every ms for --seconds. Prints min/max/last per key and
// a verdict line. Node >= 22 (global WebSocket), no dependencies.
//
//   node tools/heap_probe.mjs --node http://<ip> [--clients 3] [--seconds 30] [--every 2000] [--json]
//
// Plan step V0/V1 (docs/DI_DESK_UI_PLAN.md): MAX_FIXTURES is decided by heap_max_block.
const args = Object.fromEntries(process.argv.slice(2).map((a, i, v) => a.startsWith('--') ? [a.slice(2), v[i + 1] && !v[i + 1].startsWith('--') ? v[i + 1] : true] : []).filter(Boolean));
const base = String(args.node || '').replace(/\/$/, '');
if (!base) { console.error('usage: heap_probe.mjs --node http://<ip> [--clients N] [--seconds S] [--every MS]'); process.exit(2); }
const clients = Number(args.clients ?? 3), seconds = Number(args.seconds ?? 30), every = Number(args.every ?? 2000);
const KEYS = ['heap_free', 'heap_max_block', 'ws_clients'];

async function status() {
  const ac = new AbortController(); const t = setTimeout(() => ac.abort(), 3000);
  try { const r = await fetch(base + '/status', { signal: ac.signal }); return await r.json(); }
  finally { clearTimeout(t); }
}
async function sample(label, n) {
  const stats = Object.fromEntries(KEYS.map((k) => [k, { min: Infinity, max: -Infinity, last: null }]));
  let polls = 0, failed = 0;
  const until = Date.now() + seconds * 1000;
  while (Date.now() < until) {
    try {
      const s = await status(); polls++;
      for (const k of KEYS) { const v = s[k]; if (typeof v !== 'number') { failed++; continue; }
        const st = stats[k]; st.min = Math.min(st.min, v); st.max = Math.max(st.max, v); st.last = v; }
    } catch { failed++; }
    await new Promise((r) => setTimeout(r, every));
  }
  return { label, clients: n, polls, failed, ...Object.fromEntries(KEYS.map((k) => [k, stats[k]])) };
}
function openClients(n) {
  const wsUrl = base.replace(/^http/, 'ws') + '/ws';
  return Promise.all(Array.from({ length: n }, () => new Promise((res, rej) => {
    const w = new WebSocket(wsUrl); let msgs = 0;
    w.addEventListener('open', () => res(w)); w.addEventListener('error', (e) => rej(new Error('ws open failed: ' + (e.message || 'error'))));
    w.addEventListener('message', () => { msgs++; }); w.msgs = () => msgs;
  })));
}
const idle = await sample('idle', 0);
let socks = [];
try { socks = await openClients(clients); } catch (e) { console.error(String(e.message)); }
const loaded = await sample('loaded', socks.length);
const pushes = socks.map((w) => w.msgs());
for (const w of socks) w.close();
await new Promise((r) => setTimeout(r, 1500));
const after = await sample('after', 0);
const rows = [idle, loaded, after];
if (args.json) { console.log(JSON.stringify({ node: base, every, seconds, pushes, rows }, null, 1)); }
else {
  console.log(`node ${base}  ${seconds}s per phase, /status every ${every} ms, ${socks.length} ws clients in the loaded phase (pushes received: ${pushes.join('/') || '-'})`);
  console.log('phase   clients polls fail | heap_free min/max/last   | heap_max_block min/max/last | ws_clients max');
  for (const r of rows) console.log(`${r.label.padEnd(7)} ${String(r.clients).padStart(7)} ${String(r.polls).padStart(5)} ${String(r.failed).padStart(4)} | ${r.heap_free.min}/${r.heap_free.max}/${r.heap_free.last} | ${r.heap_max_block.min}/${r.heap_max_block.max}/${r.heap_max_block.last} | ${r.ws_clients.max}`);
  const drop = idle.heap_max_block.min - loaded.heap_max_block.min;
  console.log(`largest block: idle ${idle.heap_max_block.min} B, loaded ${loaded.heap_max_block.min} B (drop ${drop} B), after ${after.heap_max_block.min} B${after.heap_max_block.min < idle.heap_max_block.min - 2048 ? '  <- did not recover: possible leak/fragmentation' : ''}`);
}
process.exit(0);
