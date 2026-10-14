// di.iiii side of check_di_compat.sh. Uses di's own lighting modules, read-only.
//   node di_side.js gen  <lightingDir> <outDir>   -> poll.bin, artdmx.bin, sacn.bin
//   node di_side.js list <lightingDir> <reply.bin> -> di's nodeList() after one reply
const fs = require('fs');
const path = require('path');
const dgram = require('dgram');
const [mode, dir, arg] = process.argv.slice(2);
const artnet = require(path.join(dir, 'artnet.js'));
const sacn = require(path.join(dir, 'sacn.js'));

if (mode === 'gen') {
  fs.writeFileSync(path.join(arg, 'poll.bin'), artnet.buildPoll());
  const d = Buffer.alloc(512); d[0] = 11;
  fs.writeFileSync(path.join(arg, 'artdmx.bin'), artnet.buildDmx(0, 5, d));    // desk U1
  const s = Buffer.alloc(512); s[0] = 22;
  fs.writeFileSync(path.join(arg, 'sacn.bin'), sacn.buildPacket({
    cid: sacn.cidFor('compat'), sourceName: 'compat', universe: sacn.wireUniverse(0),
    priority: 100, sequence: 1, data: s,
  }));
  console.log('di packets written');
} else if (mode === 'list') {
  const node = new artnet.ArtNet({ bind: '127.0.0.1', bindPort: 0 });
  const reply = fs.readFileSync(arg);
  const tx = dgram.createSocket('udp4');
  const wait = setInterval(() => {
    if (!node.ready) return;
    clearInterval(wait);
    tx.send(reply, node.socket.address().port, '127.0.0.1', () => setTimeout(() => {
      const list = node.nodeList();
      console.log('di nodeList:', JSON.stringify(list.map(({ ip, shortName, longName, universes }) =>
        ({ ip, shortName, longName, universes }))));
      const ok = list.length === 1 && list[0].shortName === 'vizzz-test' && list[0].universes[0] === 3;
      tx.close(); node.socket.close();
      process.exit(ok ? 0 : 1);
    }, 200));
  }, 20);
} else {
  console.error('usage: node di_side.js gen|list <lightingDir> <arg>');
  process.exit(2);
}
