'use strict';
// Feeds the frames stream_harness.cpp wrote to di's own createDmxStream (dmxstream.js) and checks that the
// node's events carry the same k, d and m, in the same order. s must count 1, 2, ... like di's per client.
// Usage: node stream_side.js <di lighting dir> <harness output>
const fs = require('fs');
const path = require('path');
const { createDmxStream } = require(path.join(process.argv[2], 'dmxstream.js'));
const lines = fs.readFileSync(process.argv[3], 'utf8').trim().split('\n').map((l) => JSON.parse(l));

let clock = 0;
const out = [];
const res = {
  writableLength: 0,
  writeHead() {}, flushHeaders() {}, end() {},
  write(text) {
    const m = /^event: frame\ndata: (.*)\n\n$/s.exec(text);
    if (m) out.push(JSON.parse(m[1]));
  },
  on() {},
};
const stream = createDmxStream({ now: () => clock });
stream.subscribe({ socket: { setNoDelay() {} }, on() {} }, res, new URLSearchParams());
let master = 255;
const mine = [];
for (const [i, row] of lines.entries()) {
  clock = 1000 + 50 * (i + 1);
  master = row.master;
  stream.frame(new Map([[0, Buffer.from(row.frame)]]), () => ({ master, blackout: false, looks: [], cues: [] }));
  if (row.event) mine.push(row.event);
}
stream.close();
let bad = 0;
const fail = (m) => { bad++; console.error('FAIL', m); };
if (out.length !== mine.length) fail(`di sent ${out.length} events, the node ${mine.length}`);
for (let i = 0; i < Math.min(out.length, mine.length); i++) {
  const a = out[i], b = mine[i];
  if (a.s !== b.s) fail(`event ${i}: s ${a.s} vs ${b.s}`);
  if (a.k !== b.k) fail(`event ${i}: k ${a.k} vs ${b.k}`);
  if (JSON.stringify(a.d) !== JSON.stringify(b.d)) fail(`event ${i}: d differs`);
  if ((a.m === undefined) !== (b.m === undefined)) fail(`event ${i}: m present in only one`);
  if (a.m && b.m && (a.m.master !== b.m.master || a.m.blackout !== b.m.blackout)) fail(`event ${i}: m differs`);
}
console.log(`dmx stream vs di dmxstream.js: ${Math.min(out.length, mine.length)} events compared, ${bad ? 'FAIL' : 'PASS'}`);
process.exit(bad ? 1 : 0);
