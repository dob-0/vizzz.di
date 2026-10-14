#!/usr/bin/env python3
"""Check a vizzz.di node's network inputs from a computer on its network.

Python 3.8+ standard library only (no third-party packages to pin).

    python3 tools/net_probe.py poll   --node 192.168.88.66
    python3 tools/net_probe.py verify --node 192.168.88.66 --ch 64

`poll` sends one ArtPoll and decodes every ArtPollReply (Art-Net 4 field table).
`verify` runs the scripted acceptance check for audit 2026-10-15 part 2 (H1, H3,
M1/M2, L1, L2) and 1-M7, reading the result back over HTTP (/status, /monitor,
/page). It drives one test channel (default 64; /monitor shows channels 1-64),
which must be at 0 in the node's web layer, and sets it back to 0 at the end.
Exit status 0 = every check passed.

Limits: one computer has one source IP, so the two-source Art-Net merge is not
proven on hardware (the native tests cover it); the Art OUT latch rule needs two
nodes with Art OUT on. Replies arrive on --reply-port (6454 by default, as the
firmware always answers on 6454); a desk bound to 6454 on the same computer may
take the reply instead, so close it while polling.
"""
import argparse, json, socket, struct, sys, time, urllib.request, uuid

ART_ID = b"Art-Net\0"
ACN_PID = bytes([0x41, 0x53, 0x43, 0x2D, 0x45, 0x31, 0x2E, 0x31, 0x37, 0, 0, 0])


def art_poll():
    return ART_ID + struct.pack("<H", 0x2000) + bytes([0, 14, 0, 0])


def art_dmx(port_address, seq, data):
    return (ART_ID + struct.pack("<H", 0x5000) + bytes([0, 14, seq & 0xFF, 0,
            port_address & 0xFF, (port_address >> 8) & 0x7F]) + struct.pack(">H", len(data)) + bytes(data))


def sacn_packet(cid, universe, data, priority=100, seq=0, options=0, name="net_probe"):
    """E1.31-2018 data packet (root, framing, DMP layers), as di's sacn.js builds it."""
    slots = bytes([0]) + bytes(data)
    total = 125 + len(slots)
    p = bytearray(total)
    p[0:2] = b"\x00\x10"; p[4:16] = ACN_PID
    struct.pack_into(">HI", p, 16, 0x7000 | (total - 16), 0x00000004)
    p[22:38] = cid
    struct.pack_into(">HI", p, 38, 0x7000 | (total - 38), 0x00000002)
    p[44:44 + len(name)] = name.encode()
    p[108] = priority; p[111] = seq & 0xFF; p[112] = options
    struct.pack_into(">H", p, 113, universe)
    struct.pack_into(">HBBHHH", p, 115, 0x7000 | (total - 115), 0x02, 0xA1, 0, 1, len(slots))
    p[125:] = slots
    return bytes(p)


def osc_msg(addr, value):
    def pad(b):
        return b + b"\0" * (4 - len(b) % 4)
    if isinstance(value, float):
        return pad(addr.encode()) + pad(b",f") + struct.pack(">f", value)
    return pad(addr.encode()) + pad(b",i") + struct.pack(">i", value)


def osc_bundle(*msgs):
    out = b"#bundle\0" + struct.pack(">Q", 1)
    for m in msgs:
        out += struct.pack(">i", len(m)) + m
    return out


def decode_reply(r):
    if len(r) < 207 or r[:8] != ART_ID or struct.unpack_from("<H", r, 8)[0] != 0x2100:
        return None
    cstr = lambda a, n: r[a:a + n].split(b"\0")[0].decode("latin1")
    return {
        "length": len(r), "ip": ".".join(map(str, r[10:14])), "port": struct.unpack_from("<H", r, 14)[0],
        "net": r[18], "sub": r[19], "short": cstr(26, 18), "long": cstr(44, 64), "report": cstr(108, 64),
        "num_ports": struct.unpack_from(">H", r, 172)[0], "port_type": r[174], "good_output": r[182],
        "sw_out": r[190], "style": r[200], "mac": ":".join("%02x" % b for b in r[201:207]),
        "bind_index": r[211] if len(r) > 211 else None, "status3": r[217] if len(r) > 217 else None,
    }


class Probe:
    def __init__(self, a):
        self.a = a
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.tx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            self.tx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.tx.bind((a.bind, a.reply_port))
        self.seq = 0
        self.cid_a, self.cid_b = uuid.uuid4().bytes, uuid.uuid4().bytes

    def http(self, path):
        with urllib.request.urlopen("http://%s:%d%s" % (self.a.node, self.a.http_port, path), timeout=3) as r:
            return json.loads(r.read().decode())

    def poll(self, wait=1.5):
        self.tx.sendto(art_poll(), (self.a.node, self.a.artnet_port))
        self.tx.settimeout(0.2)
        replies, end = [], time.time() + wait
        while time.time() < end:
            try:
                data, addr = self.tx.recvfrom(1024)
            except socket.timeout:
                continue
            d = decode_reply(data)
            if d:
                d["from"] = addr[0]
                replies.append(d)
        return replies

    def art(self, port_address, level, ch, seconds=1.0, seq=None):
        frame = [0] * 512
        frame[ch - 1] = level
        end = time.time() + seconds
        while True:
            if seq is None:
                self.seq = self.seq % 255 + 1
            self.tx.sendto(art_dmx(port_address, seq if seq is not None else self.seq, frame),
                           (self.a.node, self.a.artnet_port))
            if time.time() >= end:
                break
            time.sleep(0.025)

    def sacn(self, universe, level, ch, seconds=1.0, cid=None, priority=100, options=0, start_seq=1):
        frame = [0] * 512
        frame[ch - 1] = level
        end, seq = time.time() + seconds, start_seq
        while True:
            self.tx.sendto(sacn_packet(cid or self.cid_a, universe, frame, priority, seq, options),
                           (self.a.node, self.a.sacn_port))
            seq += 1
            if time.time() >= end:
                break
            time.sleep(0.025)
        return seq

    def osc(self, payload):
        self.tx.sendto(payload, (self.a.node, self.a.osc_port))

    def out(self, ch):
        time.sleep(0.6)                                  # node slews 18 levels per 23 ms frame
        return self.http("/monitor")["out"][ch - 1]

    def web(self, ch):
        page = (ch - 1) // 32
        return self.http("/page?i=%d" % page)["web"][(ch - 1) % 32]


def expected(level, st):
    m = st.get("dim", 255)
    v = level if m >= 255 else (level * m) >> 8
    return min(v, st.get("safe_ceiling", 255))


def verify(a):
    p, ch, results = Probe(a), a.ch, []

    def check(name, ok, detail):
        results.append(ok)
        print("%s  %-46s %s" % ("PASS" if ok else "FAIL", name, detail))

    st = p.http("/status")
    uni15 = st["uni15"]
    print("node %s  name=%s mode=%s uni15=%d dim=%d ceiling=%s" % (a.node, st["name"], st["mode_name"],
          uni15, st["dim"], st.get("safe_ceiling")))
    if st["mode_name"] == "WEB_ONLY":
        print("mode WEB_ONLY ignores the network; set MERGE_HTP or ARTNET_ONLY first")
        return 2
    if p.web(ch) != 0:
        print("channel %d is not 0 in the web layer; pick another --ch" % ch)
        return 2

    rs = [r for r in p.poll() if r["from"] == a.node]
    r = rs[0] if rs else {}
    check("2-H1 ArtPoll answered with ArtPollReply", bool(rs), "%d reply(ies)" % len(rs))
    check("2-H1 reply layout (239 B, port, ports, style)",
          r.get("length", 0) >= 239 and r.get("port") == 6454 and r.get("num_ports") == 1
          and r.get("port_type") == 0x80 and r.get("style") == 0 and r.get("bind_index") == 1,
          json.dumps({k: r.get(k) for k in ("length", "port", "num_ports", "port_type", "style", "bind_index")}))
    check("2-H1 reply address = /status net/sub/uni",
          (r.get("net"), r.get("sub"), r.get("sw_out")) == (st["net"], st["subnet"], st["uni"]),
          "net=%s sub=%s sw_out=%s" % (r.get("net"), r.get("sub"), r.get("sw_out")))
    check("2-H1 short name from node name", r.get("short") == st["name"][:17], repr(r.get("short")))

    lvl = 200
    p.art(uni15, lvl, ch)
    got = p.out(ch)
    check("ArtDmx on the node's port-address drives output", got == expected(lvl, st),
          "out=%d want=%d" % (got, expected(lvl, st)))
    p.art(uni15, 200, ch, seconds=0.1, seq=50)
    p.art(uni15, 20, ch, seconds=0.1, seq=49)
    got = p.out(ch)
    check("2-L1 out-of-order ArtDmx (seq 49 after 50) dropped", got == expected(200, st), "out=%d" % got)
    time.sleep(3.5)                                      # Art-Net loss timeout

    p.sacn(uni15 + 1, 150, ch)
    got = p.out(ch)
    check("2-H3 sACN on universe port-address + 1", got == expected(150, st),
          "universe=%d out=%d want=%d" % (uni15 + 1, got, expected(150, st)))
    p.sacn(uni15 + 1, 150, ch, seconds=0.1, options=0x40, start_seq=100)
    time.sleep(0.3)
    st2 = p.http("/status")
    check("2-M2 stream terminated drops the source at once", not st2["sacn_active"],
          "sacn_active=%s 0.3 s after terminate" % st2["sacn_active"])
    time.sleep(3.0)
    if uni15 == 0:
        p.sacn(0, 90, ch)
        got = p.out(ch)
        check("2-H3 reserved sACN universe 0 ignored", got == 0, "out=%d" % got)
    p.sacn(uni15 + 1, 90, ch, options=0x80)
    got = p.out(ch)
    check("2-M2 preview data never drives output", got == 0, "out=%d" % got)
    time.sleep(3.0)
    seq = p.sacn(uni15 + 1, 200, ch, seconds=0.5, cid=p.cid_a, priority=100)
    p.sacn(uni15 + 1, 30, ch, seconds=0.5, cid=p.cid_b, priority=150)
    p.sacn(uni15 + 1, 200, ch, seconds=0.3, cid=p.cid_a, priority=100, start_seq=seq)
    got = p.out(ch)
    check("2-M2 higher sACN priority wins (not HTP)", got == expected(30, st), "out=%d want=%d"
          % (got, expected(30, st)))
    time.sleep(3.0)

    p.osc(osc_msg("/ch/%d" % ch, 0.5))
    time.sleep(0.3)
    w = p.web(ch)
    check("1-M7 OSC float 0.5 -> level 128", w == 128, "web=%d" % w)
    p.osc(osc_bundle(osc_msg("/ch/%d" % ch, 7), osc_msg("/ch/%d" % ch, 9)))
    time.sleep(0.3)
    w = p.web(ch)
    check("1-L1 OSC bundle elements dispatched", w == 9, "web=%d" % w)
    p.osc(osc_msg("/ch/%d" % ch, 0))
    time.sleep(0.3)
    print("channel %d set back to 0 (web=%d)" % (ch, p.web(ch)))

    print("%d/%d checks passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["poll", "verify"])
    ap.add_argument("--node", required=True, help="node IP (or a broadcast address for poll)")
    ap.add_argument("--ch", type=int, default=64, help="test channel 1-64")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--reply-port", type=int, default=6454, help="local port; 0 = any")
    ap.add_argument("--artnet-port", type=int, default=6454)
    ap.add_argument("--sacn-port", type=int, default=5568)
    ap.add_argument("--osc-port", type=int, default=9000)
    ap.add_argument("--http-port", type=int, default=80)
    a = ap.parse_args()
    if not 1 <= a.ch <= 64:
        ap.error("--ch must be 1-64 (/monitor shows the first 64 channels)")
    if a.cmd == "poll":
        rs = Probe(a).poll()
        for r in rs:
            print(json.dumps(r))
        print("%d reply(ies)" % len(rs))
        return 0 if rs else 1
    return verify(a)


if __name__ == "__main__":
    sys.exit(main())
