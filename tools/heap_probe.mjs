#!/usr/bin/env node
// heap_probe — read heap_free / heap_max_block / ws_clients (/ sse_clients) from a node's /status
// at idle and under a load of N WebSocket clients (what N phones on /vj hold open) and/or N
// /api/dmx/stream readers (what N desk pages hold open), polling /status every --every ms for
// --seconds. Prints min/max/last per key and a verdict line. Node >= 22 (global WebSocket), no
// dependencies. Node has no EventSource: the stream is read with fetch and a TextDecoder.
//
//   node tools/heap_probe.mjs --node http://<ip> [--clients 3] [--sse 3] [--seconds 30] [--every 2000] [--json]
//
// --sse N opens N stream readers for the loaded phase and reports, per reader, the events it got
// (key frames / deltas), events per second, bytes per second and the HTTP status (a reader over the
// node's cap gets 503 {"error":"stream full"} and is reported as refused, not as a failure).
//
// Plan step V0/V1/V5 (docs/DI_DESK_UI_PLAN.md): MAX_FIXTURES is decided by heap_max_block; the
// stream's client cap by heap_max_block with 1, 3 and 5 readers.
const args = Object.fromEntries(process.argv.slice(2).map((a, i, v) => a.startsWith('--') ? [a.slice(2), v[i + 1] && !v[i + 1].startsWith('--') ? v[i + 1] : true] : []).filter(Boolean));
const base = String(args.node || '').replace(/\/$/, '');
if (!base) { console.error('usage: heap_probe.mjs --node http://<ip> [--clients N] [--sse N] [--seconds S] [--every MS]'); process.exit(2); }
const clients = Number(args.clients ?? 3), seconds = Number(args.seconds ?? 30), every = Number(args.every ?? 2000);
const sseN = Number(args.sse ?? 0);
const KEYS = ['heap_free', 'heap_max_block', 'ws_clients'];
const OPTIONAL = ['sse_clients'];   // absent on a node older than V5

async function status() {
  const ac = new AbortController(); const t = setTimeout(() => ac.abort(), 3000);
  try { const r = await fetch(base + '/status', { signal: ac.signal }); return await r.json(); }
  finally { clearTimeout(t); }
}
async function sample(label, n) {
  const stats = Object.fromEntries([...KEYS, ...OPTIONAL].map((k) => [k, { min: Infinity, max: -Infinity, last: null }]));
  let polls = 0, failed = 0;
  const until = Date.now() + seconds * 1000;
  while (Date.now() < until) {
    try {
      const s = await status(); polls++;
      for (const k of [...KEYS, ...OPTIONAL]) { const v = s[k]; if (typeof v !== 'number') { if (KEYS.includes(k)) failed++; continue; }
        const st = stats[k]; st.min = Math.min(st.min, v); st.max = Math.max(st.max, v); st.last = v; }
    } catch { failed++; }
    await new Promise((r) => setTimeout(r, every));
  }
  return { label, clients: n, polls, failed, ...Object.fromEntries([...KEYS, ...OPTIONAL].map((k) => [k, stats[k]])) };
}
function openClients(n) {
  const wsUrl = base.replace(/^http/, 'ws') + '/ws';
  return Promise.all(Array.from({ length: n }, () => new Promise((res, rej) => {
    const w = new WebSocket(wsUrl); let msgs = 0;
    w.addEventListener('open', () => res(w)); w.addEventListener('error', (e) => rej(new Error('ws open failed: ' + (e.message || 'error'))));
    w.addEventListener('message', () => { msgs++; }); w.msgs = () => msgs;
  })));
}
// One /api/dmx/stream reader: fetch + TextDecoder (no EventSource in Node). Parses the SSE lines
// itself: "event:", "data:", a blank line ends an event; ":" lines are comments; "retry:" is noted.
function openSse(n) {
  return Array.from({ length: n }, (_, i) => {
    const r = { i: i + 1, status: 0, error: null, events: 0, keys: 0, deltas: 0, bad: 0, bytes: 0, retry: null, lastSeq: 0, seqGaps: 0, t0: 0, ac: new AbortController() };
    r.done = (async () => {
      try {
        const res = await fetch(base + '/api/dmx/stream', { signal: r.ac.signal, headers: { accept: 'text/event-stream' } });
        r.status = res.status;
        if (!res.ok) { r.error = (await res.text()).slice(0, 80); return; }
        r.t0 = Date.now();
        const dec = new TextDecoder(); let buf = '', name = '', data = '';
        for await (const chunk of res.body) {
          r.bytes += chunk.length; buf += dec.decode(chunk, { stream: true });
          let nl;
          while ((nl = buf.indexOf('\n')) >= 0) {
            const line = buf.slice(0, nl).replace(/\r$/, ''); buf = buf.slice(nl + 1);
            if (line === '') {
              if (name === 'frame' && data) {
                try { const e = JSON.parse(data); r.events++; e.k ? r.keys++ : r.deltas++;
                  if (r.lastSeq && e.s !== r.lastSeq + 1) r.seqGaps++; r.lastSeq = e.s;
                  if (!Array.isArray(e.d)) r.bad++; } catch { r.bad++; }
              }
              name = ''; data = '';
            } else if (line.startsWith('event:')) name = line.slice(6).trim();
            else if (line.startsWith('data:')) data += line.slice(5).trim();
            else if (line.startsWith('retry:')) r.retry = Number(line.slice(6));
          }
        }
      } catch (e) { if (e.name !== 'AbortError') r.error = String(e.message || e); }
    })();
    return r;
  });
}

const idle = await sample('idle', 0);
let socks = [];
try { socks = await openClients(clients); } catch (e) { console.error(String(e.message)); }
const streams = openSse(sseN);
const loadStart = Date.now();
const loaded = await sample('loaded', socks.length);
const loadedMs = Date.now() - loadStart;
const pushes = socks.map((w) => w.msgs());
for (const w of socks) w.close();
for (const r of streams) r.ac.abort();
await Promise.all(streams.map((r) => r.done));
await new Promise((r) => setTimeout(r, 1500));
const after = await sample('after', 0);
const rows = [idle, loaded, after];
const sseRows = streams.map((r) => ({ reader: r.i, status: r.status, refused: r.status === 503, error: r.error, events: r.events, keys: r.keys, deltas: r.deltas, bad: r.bad, seqGaps: r.seqGaps,
  eventsPerSec: r.t0 ? +(r.events / ((loadStart + loadedMs - r.t0) / 1000)).toFixed(2) : 0, bytesPerSec: r.t0 ? Math.round(r.bytes / ((loadStart + loadedMs - r.t0) / 1000)) : 0, retry: r.retry }));
if (args.json) { console.log(JSON.stringify({ node: base, every, seconds, pushes, rows, sse: sseRows }, null, 1)); }
else {
  console.log(`node ${base}  ${seconds}s per phase, /status every ${every} ms, ${socks.length} ws clients and ${sseN} stream readers in the loaded phase (ws pushes received: ${pushes.join('/') || '-'})`);
  console.log('phase   clients polls fail | heap_free min/max/last   | heap_max_block min/max/last | ws_clients max | sse_clients max');
  for (const r of rows) console.log(`${r.label.padEnd(7)} ${String(r.clients).padStart(7)} ${String(r.polls).padStart(5)} ${String(r.failed).padStart(4)} | ${r.heap_free.min}/${r.heap_free.max}/${r.heap_free.last} | ${r.heap_max_block.min}/${r.heap_max_block.max}/${r.heap_max_block.last} | ${r.ws_clients.max} | ${Number.isFinite(r.sse_clients.max) ? r.sse_clients.max : 'n/a'}`);
  for (const r of sseRows) console.log(r.refused ? `stream ${r.reader}: refused ${r.status} ${r.error}` : r.error && !r.events ? `stream ${r.reader}: ${r.status || 'no response'} ${r.error}` : `stream ${r.reader}: ${r.events} events (${r.keys} key, ${r.deltas} delta, ${r.bad} bad, ${r.seqGaps} seq gaps) = ${r.eventsPerSec}/s, ${r.bytesPerSec} B/s, retry ${r.retry}`);
  const drop = idle.heap_max_block.min - loaded.heap_max_block.min;
  console.log(`largest block: idle ${idle.heap_max_block.min} B, loaded ${loaded.heap_max_block.min} B (drop ${drop} B), after ${after.heap_max_block.min} B${after.heap_max_block.min < idle.heap_max_block.min - 2048 ? '  <- did not recover: possible leak/fragmentation' : ''}`);
}
process.exit(0);
