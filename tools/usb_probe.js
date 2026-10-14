#!/usr/bin/env node
'use strict';
// usb_probe.js — flash-and-verify proof for the node's USB widget input.
//
// The node speaks the ENTTEC DMX USB PRO widget protocol ("DMX USB PRO API",
// ENTTEC) on its USB serial port. This probe drives it with di.iiii's OWN driver
// (serverXR/src/lighting/enttec.js, required from --di), constructed and called
// exactly as the light desk does (desk.js: new Enttec({ port, maxHz }), then
// send(universe 0, 512-byte buffer) every 1000/maxHz ms), so a PASS here is
// di's driver working against the node, not a re-implementation of it.
//
// Zero dependencies. Never run it against a port the desk has open.
//
//   node tools/usb_probe.js --di ~/work/di.iiii --port /dev/ttyUSB0 \
//        --node http://192.168.88.66 --ch 64 --v 180 --seconds 5
//   node tools/usb_probe.js params --di ~/work/di.iiii --port /dev/ttyUSB0
//
// Default run: status before -> send ch=v for N s (status polled every second:
// the node must stay reachable) -> /status usb_active true, usb_frames grew,
// usb_baud == driver baud -> /page out at ch == expected for mode, master,
// ceiling -> zero frames 1.5 s -> out at ch back to the web layer -> close.
// `params`: sends label 3 (Get Widget Parameters) and label 10 (Get Widget
// Serial Number) through the driver's own frame() and prints the decoded replies.
// Exit code 0 only when every check passed.

const fs = require('fs');
const path = require('path');
const http = require('http');

function args(argv) {
  const o = { cmd: 'run', di: path.join(process.env.HOME || '', 'work/di.iiii'), port: '/dev/ttyUSB0',
              node: 'http://192.168.88.66', ch: 64, v: 180, seconds: 5, hz: 40 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === 'params' || a === 'run') { o.cmd = a; continue; }
    const k = a.replace(/^--/, '');
    if (!(k in o) || i + 1 >= argv.length) { console.error(`unknown or empty option ${a}`); process.exit(2); }
    const val = argv[++i];
    o[k] = typeof o[k] === 'number' ? Number(val) : val;
  }
  if (!(o.ch >= 1 && o.ch <= 512) || !(o.v >= 0 && o.v <= 255)) { console.error('--ch 1..512, --v 0..255'); process.exit(2); }
  return o;
}

const opt = args(process.argv.slice(2));
const { Enttec, frame } = require(path.join(opt.di, 'serverXR/src/lighting/enttec.js'));

let passed = 0, failed = 0;
function check(name, ok, detail) {
  if (ok) passed++; else failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  — ' + detail : ''}`);
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

function getJSON(url) {
  return new Promise((resolve) => {
    const req = http.get(url, { timeout: 2000 }, (res) => {
      let body = '';
      res.on('data', (c) => { body += c; });
      res.on('end', () => { try { resolve(JSON.parse(body)); } catch (e) { resolve(null); } });
    });
    req.on('timeout', () => req.destroy());
    req.on('error', () => resolve(null));
  });
}

// Same arithmetic as the firmware (include/vizzz_core.h applyMaster, clampLevel, merge::forMode).
function expectedOut(st, web, usb) {
  let v;
  if (st.mode === 0) v = web;                 // WEB_ONLY ignores USB
  else if (st.mode === 1) v = usb;            // ARTNET_ONLY: network sources alone
  else v = Math.max(web, usb);                // MERGE_HTP
  if (st.dim < 255) v = (v * st.dim) >> 8;
  return Math.min(v, st.safe_ceiling);
}

async function pageAt(ch) {
  const i = Math.floor((ch - 1) / 32);
  const p = await getJSON(`${opt.node}/page?i=${i}`);
  if (!p) return null;
  const k = ch - p.base_ch;
  return { web: p.web[k], out: p.out[k] };
}

function openDriver() {
  const e = new Enttec({ port: opt.port, maxHz: opt.hz });
  if (!e.connected) { console.log(`FAIL  open ${opt.port}  — ${e.lastError}`); process.exit(1); }
  console.log(`opened ${opt.port} at ${e.baud} baud with di's enttec.js`);
  return e;
}

async function run() {
  const before = await getJSON(`${opt.node}/status`);
  check('node /status reachable before', !!before, opt.node);
  if (!before) return;
  check('firmware has the USB widget (usb_baud in /status)', before.usb_baud != null, `fw ${before.fw}`);

  const e = openDriver();
  check('host baud == node usb_baud', e.baud === before.usb_baud, `host ${e.baud}, node ${before.usb_baud}`);

  const buf = Buffer.alloc(512);
  buf[opt.ch - 1] = opt.v;
  const tick = Math.round(1000 / opt.hz);
  const timer = setInterval(() => e.send(0, buf), tick);   // the desk's pushFrame loop
  let unreachable = 0;
  for (let s = 0; s < opt.seconds; s++) { await wait(1000); if (!(await getJSON(`${opt.node}/status`))) unreachable++; }
  check('node stayed reachable while the port was open (no DTR/RTS reset)', unreachable === 0, `${unreachable} missed polls`);

  const during = await getJSON(`${opt.node}/status`);
  const sent = e.status().packetsSent;
  check('usb_active true while sending', !!during && during.usb_active === true);
  const grew = during ? during.usb_frames - before.usb_frames : 0;
  check('usb_frames grew', grew > 0, `+${grew} on the node, ${sent} sent by the driver (${e.status().dropped} dropped by it)`);
  if (during) {
    const pg = await pageAt(opt.ch);
    const want = pg ? expectedOut(during, pg.web, opt.v) : null;
    check(`out at ch ${opt.ch} == expected`, !!pg && pg.out === want,
          pg ? `out ${pg.out}, want ${want} (v ${opt.v}, web ${pg.web}, mode ${during.mode_name}, master ${during.dim}, ceiling ${during.safe_ceiling})` : 'no /page');
  }

  buf[opt.ch - 1] = 0;
  await wait(1500);                                      // zero frames, slew settles
  const pg0 = await pageAt(opt.ch);
  const st0 = await getJSON(`${opt.node}/status`);
  const want0 = pg0 && st0 ? expectedOut(st0, pg0.web, 0) : null;
  check(`released: out at ch ${opt.ch} back to the web layer`, !!pg0 && pg0.out === want0,
        pg0 ? `out ${pg0.out}, want ${want0}` : 'no /page');

  clearInterval(timer);
  e.close();
}

// Widget-protocol reader for the replies; text log bytes between frames are skipped.
function decodeFrames(bytes) {
  const out = [];
  let skipped = 0;
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== 0x7e || i + 4 > bytes.length) { skipped++; continue; }
    const len = bytes[i + 2] | (bytes[i + 3] << 8);
    const end = i + 4 + len;
    if (len > 600 || end >= bytes.length || bytes[end] !== 0xe7) { skipped++; continue; }
    out.push({ label: bytes[i + 1], data: bytes.subarray(i + 4, end) });
    i = end;
  }
  return { frames: out, skipped };
}

async function readFor(e, ms) {
  const chunks = [];
  const tmp = Buffer.alloc(1024);
  const until = Date.now() + ms;
  while (Date.now() < until) {
    let n = 0;
    try { n = fs.readSync(e.fd, tmp, 0, tmp.length, null); } catch (err) { if (err.code !== 'EAGAIN') throw err; }
    if (n > 0) chunks.push(Buffer.from(tmp.subarray(0, n))); else await wait(10);
  }
  return Buffer.concat(chunks);
}

async function params() {
  const e = openDriver();
  await readFor(e, 200);                                  // drain any log text already queued
  fs.writeSync(e.fd, frame(3, Buffer.from([0, 0])));     // exactly what enttec.js identify() sends
  fs.writeSync(e.fd, frame(10, Buffer.alloc(0)));
  const { frames, skipped } = decodeFrames(await readFor(e, 1000));
  console.log(`read ${frames.length} frame(s), ${skipped} non-frame byte(s) (log text)`);
  const p = frames.find((f) => f.label === 3);
  check('label 3 Get Widget Parameters replied', !!p && p.data.length >= 5);
  if (p) {
    const d = p.data;
    console.log(`  firmware ${d[1]}.${d[0]}  break ${d[2]} x10.67us (${(d[2] * 10.67).toFixed(0)} us)  ` +
                `MAB ${d[3]} x10.67us (${(d[3] * 10.67).toFixed(0)} us)  rate ${d[4]} pkt/s  user ${d.length - 5} B`);
    check('label 3 values in API range', d[2] >= 9 && d[2] <= 127 && d[3] >= 1 && d[3] <= 127 && d[4] >= 1 && d[4] <= 40);
  }
  const s = frames.find((f) => f.label === 10);
  check('label 10 Get Widget Serial Number replied (4 BCD bytes)', !!s && s.data.length === 4);
  if (s) {
    const digits = [...s.data].reverse().map((b) => (b >> 4).toString() + (b & 15).toString()).join('');
    console.log(`  serial ${digits}`);
    check('serial is valid BCD', /^\d{8}$/.test(digits));
  }
  e.close();
}

(opt.cmd === 'params' ? params() : run())
  .catch((err) => { failed++; console.log(`FAIL  ${err.message}`); })
  .then(() => {
    console.log(`${passed} passed, ${failed} failed`);
    process.exit(failed ? 1 : 0);
  });
