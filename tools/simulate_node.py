#!/usr/bin/env python3
"""Run the vizzz.di browser console on this machine, with no ESP32.

Serves the real APP_HTML extracted from src/main.cpp against an in-memory model
of the firmware: web layer, master dimmer, safety ceiling and slew, scenes, FX,
colour, cues, groups and a fake peer table. DMX output is computed the same way
the firmware computes it, so /monitor and /page show believable numbers.

    python3 tools/simulate_node.py --port 8088

    python3 tools/simulate_node.py --udp      # also Art-Net 6454, sACN 5568, OSC 9000

This is a UI and API harness, not an emulator. It does not run the C++. With
--udp it mirrors the firmware's network inputs (include/vizzz_core.h): ArtPoll
-> 239-byte ArtPollReply, two-source Art-Net HTP merge (10 s, third ignored),
the Art OUT tag rule, sACN on Port-Address + 1 with preview / terminated /
priority / sequence handling, and OSC int/float/bundle input. One deviation:
the ArtPollReply goes to the poll's source address and port (the firmware
always uses port 6454, which is the same thing for di and tools/net_probe.py).
"""
import argparse, base64, hashlib, json, math, os, re, socket, struct, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "src", "main.cpp")
MAX_CH, SCENE_COUNT, PAGE_SIZE, PAGE_COUNT, MAX_FIXTURES = 512, 8, 32, 16, 12
SAFE_MAX_LEVEL, SAFE_SLEW_STEP = 245, 18
BURN_SAFE_MAX_LEVEL, BURN_SAFE_SLEW_STEP = 96, 8
PAGE_ROUTES = ("/", "/control", "/patch", "/scenes", "/network", "/system",
               "/vj", "/performance")


def extract_app_html(path=MAIN):
    src = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r'APP_HTML\[\]\s*PROGMEM\s*=\s*R"HTML\((.*?)\)HTML";', src, re.S)
    if not m:
        raise SystemExit("APP_HTML literal not found in %s" % path)
    return m.group(1)


class Node:
    def __init__(self):
        self.lock = threading.Lock()
        self.web = [0] * MAX_CH
        self.net_layer = [0] * MAX_CH
        self.out = [0] * MAX_CH
        self.scenes = [[0] * MAX_CH for _ in range(SCENE_COUNT)]
        self.master = 255
        self.mode = 2                      # MERGE_HTP
        self.net_mode = 0                  # AP_STA
        self.art = {"net": 0, "subnet": 0, "uni": 0}
        self.art_out = False
        self.art_out_target = ""
        self.web_enabled = True
        self.burn_safe = False
        self.artnet_fallback = True
        self.name = "vizzz.di-sim"
        self.ap_ssid = "vizzz.di_SIMLAB"
        self.mac = "24:6F:28:AA:BB:CC"
        self.fade = None                   # (start, dur_ms, from[], to[])
        self.fx = {"en": False, "mode": 0, "bpm": 120, "depth": 128}
        self.color = {"en": False, "r": 255, "g": 255, "b": 255}
        self.cue = {"run": False, "count": 4, "index": 0,
                    "steps": [{"scene": i % SCENE_COUNT, "dwell": 2000, "fade": 500}
                              for i in range(16)]}
        self.groups = [{"name": "GRP %d" % (i + 1), "start": i * 8 + 1,
                        "end": i * 8 + 8, "en": i < 2} for i in range(8)]
        self.fixtures = [  # firmware initFixtureDefaults(), main.cpp:221
            {"name": "F%d" % (i + 1), "start": st, "end": en, "x": x, "y": y,
             "en": i < 4}
            for i, (st, en, x, y) in enumerate(zip(
                [1, 4, 7, 10, 64, 67, 70, 73, 128, 131, 134, 137],
                [3, 6, 9, 12, 66, 69, 72, 75, 130, 133, 136, 139],
                [32, 96, 160, 224, 32, 96, 160, 224, 32, 96, 160, 224],
                [32, 32, 32, 32, 128, 128, 128, 128, 224, 224, 224, 224]))]
        self.fixture_count = 4
        self.peers = [
            {"ip": "10.0.0.51", "name": "vizzz.di-u2", "mac": "24:6F:28:11:22:33",
             "uni": 1, "age": 1200},
            {"ip": "10.0.0.52", "name": "vizzz.di-u3", "mac": "24:6F:28:44:55:66",
             "uni": 2, "age": 2400},
        ]
        self.tap = []
        self.boot = time.time()

    # ---- output pipeline, mirroring the firmware ----
    def uni15(self):
        return (self.art["net"] << 8) | (self.art["subnet"] << 4) | self.art["uni"]

    def net_active(self):
        return NET.art_active() or NET.sacn_active()

    def ceiling(self):
        return BURN_SAFE_MAX_LEVEL if self.burn_safe else SAFE_MAX_LEVEL

    def slew_step(self):
        return BURN_SAFE_SLEW_STEP if self.burn_safe else SAFE_SLEW_STEP

    def tick(self):
        with self.lock:
            self._fade_tick()
            self._cue_tick()
            src = self._merged()
            cap, step = self.ceiling(), self.slew_step()
            for i in range(MAX_CH):
                t = min(src[i] * self.master // 255, cap)
                cur = self.out[i]
                if t > cur:
                    cur = min(t, cur + step)
                elif t < cur:
                    cur = max(t, cur - step)
                self.out[i] = cur

    def _merged(self):
        net = self.net_layer if self.net_active() else None
        if self.mode == 0:
            base = list(self.web)
        elif self.mode == 1:
            base = list(net) if net else (list(self.web) if self.artnet_fallback else list(self.out))
        else:
            base = [max(a, b) for a, b in zip(self.web, net)] if net else list(self.web)
        if self.fx["en"]:
            phase = (time.time() * self.fx["bpm"] / 60.0) % 1.0
            depth = self.fx["depth"] / 255.0
            for i in range(MAX_CH):
                if self.fx["mode"] == 1:      # chase
                    on = (int(time.time() * self.fx["bpm"] / 60.0) % 8) == (i % 8)
                    k = 1.0 if on else 1.0 - depth
                else:                          # sine pulse
                    k = 1.0 - depth * (0.5 - 0.5 * __import__("math").cos(2 * 3.14159 * phase))
                base[i] = int(base[i] * k)
        return base

    def _fade_tick(self):
        if not self.fade:
            return
        t0, dur, a, b = self.fade
        p = 1.0 if dur <= 0 else min(1.0, (time.time() - t0) * 1000.0 / dur)
        for i in range(MAX_CH):
            self.web[i] = int(a[i] + (b[i] - a[i]) * p)
        if p >= 1.0:
            self.fade = None

    def _cue_tick(self):
        if not self.cue["run"]:
            return
        st = self.cue["steps"][self.cue["index"] % self.cue["count"]]
        if not hasattr(self, "_cue_at"):
            self._cue_at = 0
        if (time.time() - self._cue_at) * 1000 >= st["dwell"]:
            self._cue_at = time.time()
            self.cue["index"] = (self.cue["index"] + 1) % self.cue["count"]
            nxt = self.cue["steps"][self.cue["index"]]
            self.fade = (time.time(), nxt["fade"], list(self.web),
                         list(self.scenes[nxt["scene"]]))

    def status(self):
        return {
            "ap_ip": "192.168.4.1", "sta_ip": "192.168.88.140", "sta": True,
            "sta_ssid": "sim-net", "wl_status": 3, "ssid": self.ap_ssid,
            "name": self.name, "mdns": self.name.lower(),
            "net_mode": self.net_mode,
            "net_mode_name": ["AP_STA", "STA_ONLY", "AP_ONLY"][self.net_mode],
            "net": self.art["net"], "subnet": self.art["subnet"],
            "uni": self.art["uni"],
            "uni15": (self.art["net"] << 8) | (self.art["subnet"] << 4) | self.art["uni"],
            "mode": self.mode,
            "mode_name": ["WEB_ONLY", "ARTNET_ONLY", "MERGE_HTP"][self.mode],
            "artnet_active": NET.art_active(), "sacn_active": NET.sacn_active(),
            "ao": self.art_out, "web": self.web_enabled, "dim": self.master,
            "ao_target": self.art_out_target or "broadcast",
            "ap_clients": 1, "sta_rssi": -54, "mac": self.mac,
            "peer_count": len(self.peers), "burn_safe": self.burn_safe,
            "artnet_fallback_to_web": self.artnet_fallback,
            "safe_ceiling": self.ceiling(), "safe_slew_step": self.slew_step(),
        }

    def manifest(self):
        return {
            "schema": "vizzz.di.node.manifest.v1", "kind": "firmware-node",
            "product": "vizzz.di", "name": self.name, "fw": "simulator",
            "hardware": {"board": "ESP32 DevKit (simulated)", "dmx_uart": "DMX_NUM_1",
                         "dmx_tx_gpio": 25, "dmx_dir_gpio": 21,
                         "max_channels": MAX_CH, "dmx_period_ms": 23},
            "protocols": [
                {"id": "artnet", "role": "input-output", "port": 6454,
                 "universe": self.uni15(), "output_enabled": self.art_out},
                {"id": "sacn", "role": "input", "port": 5568,
                 "universe": self.uni15() + 1},          # E1.31 §6.2.7: never 0
                {"id": "osc", "role": "input", "transport": "udp", "port": 9000},
                {"id": "dmx512", "role": "physical-output", "channels": MAX_CH}],
            "simulated": True,
        }


NODE = Node()
# ── Network inputs, same rules as include/vizzz_core.h ──────────────────────
ART_ID = b"Art-Net\0"
ACN_PID = bytes([0x41, 0x53, 0x43, 0x2D, 0x45, 0x31, 0x2E, 0x31, 0x37, 0, 0, 0])
VIZZZ_PHYSICAL_TAG = 0x76


def seq_stale(last, now):
    """E1.31-2018 §6.7.2 window, used for Art-Net too (0 = sequencing off)."""
    diff = (now - last) & 0xFF
    diff = diff - 256 if diff > 127 else diff
    return -20 < diff <= 0


def build_poll_reply(ip, mac, net, sub, uni, short, long_, report, count, merging, sacn_src):
    """Art-Net 4 ArtPollReply, 239 bytes, same fields as vizzz::artnet::buildPollReply."""
    o = bytearray(239)
    o[0:8] = ART_ID
    o[8:10] = struct.pack("<H", 0x2100)
    o[10:14] = bytes(ip)
    o[14:16] = struct.pack("<H", 6454)
    o[16], o[17] = 0, 1
    o[18], o[19] = net & 0x7F, sub & 0x0F
    o[20], o[21], o[23] = 0x00, 0xFF, 0xE0
    o[24], o[25] = 0xF0, 0x7F
    o[26:26 + 17] = short.encode("latin1", "replace")[:17].ljust(17, b"\0")
    o[44:44 + 63] = long_.encode("latin1", "replace")[:63].ljust(63, b"\0")
    rep = ("#0001 [%04d] %s" % (count % 10000, report)).encode("latin1", "replace")[:63]
    o[108:108 + len(rep)] = rep
    o[172], o[173], o[174], o[178] = 0, 1, 0x80, 0x08
    o[182] = 0x80 | (0x08 if merging else 0) | (0x01 if sacn_src else 0)
    o[186] = o[190] = uni & 0x0F
    o[201:207] = bytes(mac)
    o[207:211] = bytes(ip)
    o[211], o[212], o[213], o[217], o[227] = 1, 0x0F, 0x80, 0x40, 43
    return bytes(o)


class NetInputs:
    def __init__(self):
        self.lock = threading.Lock()
        self.art = {}           # ip -> [last_s, seq, data]   (two slots)
        self.sacn = {}          # cid -> [last_s, seq, prio, data]
        self.last_art = 0.0
        self.replies = 0

    def art_active(self):
        return time.time() - self.last_art < 3.0

    def sacn_active(self):
        with self.lock:
            return bool(self.sacn)

    def expire(self):
        now = time.time()
        with self.lock:
            self.art = {k: v for k, v in self.art.items() if now - v[0] <= 10.0}
            if not any(now - v[0] < 3.0 for v in self.art.values()):
                self.art = {}
            self.sacn = {k: v for k, v in self.sacn.items() if now - v[0] <= 2.5}
            art = [max(c) for c in zip(*[v[2] for v in self.art.values()])] if self.art else None
            top = max((v[2] for v in self.sacn.values()), default=None)
            sa = ([max(c) for c in zip(*[v[3] for v in self.sacn.values() if v[2] == top])]
                  if top is not None else None)
        layers = [l for l in (art, sa) if l]
        NODE.net_layer = [max(c) for c in zip(*layers)] if layers else [0] * MAX_CH

    def on_artnet(self, pkt, addr, sock):
        if len(pkt) < 14 or pkt[:8] != ART_ID:
            return
        op = struct.unpack_from("<H", pkt, 8)[0]
        if op == 0x2000:
            self.replies += 1
            here = sock.getsockname()[0]
            ip = [int(x) for x in (here if here != "0.0.0.0" else "127.0.0.1").split(".")]
            mac = [int(x, 16) for x in NODE.mac.split(":")]
            reply = build_poll_reply(ip, mac, NODE.art["net"], NODE.art["subnet"], NODE.art["uni"],
                                     NODE.name, "vizzz.di " + NODE.name, "simulator", self.replies,
                                     len(self.art) > 1, self.sacn_active() and not self.art_active())
            sock.sendto(reply, addr)
            return
        if op != 0x5000 or len(pkt) < 18 or ((pkt[10] << 8) | pkt[11]) < 14:
            return
        length = (pkt[16] << 8) | pkt[17]
        if length == 0 or length > 512 or 18 + length > len(pkt):
            return
        if (pkt[14] | ((pkt[15] & 0x7F) << 8)) != NODE.uni15():
            return
        if NODE.art_out and pkt[13] == VIZZZ_PHYSICAL_TAG:
            return                                       # two-node latch rule (1-C1)
        seq, now, ip = pkt[12], time.time(), addr[0]
        with self.lock:
            slot = self.art.get(ip)
            if slot is None and len(self.art) >= 2:
                return                                   # third source ignored
            if slot and seq_stale(slot[1], seq) and seq and slot[1] and now - slot[0] < 1.0:
                return
            self.art[ip] = [now, seq, list(pkt[18:18 + length]) + [0] * (512 - length)]
            self.last_art = now

    def on_sacn(self, pkt):
        if len(pkt) < 126 or pkt[0:2] != b"\x00\x10" or pkt[4:16] != ACN_PID:
            return
        if pkt[18:22] != b"\0\0\0\x04" or pkt[40:44] != b"\0\0\0\x02" or pkt[117] != 2:
            return
        if pkt[118] != 0xA1 or pkt[119:123] != b"\0\0\0\x01":
            return
        count = (pkt[123] << 8) | pkt[124]
        uni = (pkt[113] << 8) | pkt[114]
        if count < 1 or count > 513 or 125 + count > len(pkt) or uni != NODE.uni15() + 1:
            return
        opts, prio, seq, cid = pkt[112], min(pkt[108], 200), pkt[111], bytes(pkt[22:38])
        if opts & 0x80:
            return                                       # preview, §6.2.6
        now = time.time()
        with self.lock:
            src = self.sacn.get(cid)
            if src and seq_stale(src[1], seq):
                return
            if opts & 0x40:
                self.sacn.pop(cid, None)                 # terminated, §6.2.6
                return
            if pkt[125] != 0:
                return
            if src is None and len(self.sacn) >= 2:
                low = min(self.sacn, key=lambda k: self.sacn[k][2])
                if prio <= self.sacn[low][2]:
                    return
                del self.sacn[low]
            data = list(pkt[126:125 + count])
            self.sacn[cid] = [now, seq, prio, data + [0] * (512 - len(data))]

    def on_osc(self, pkt):
        if pkt.startswith(b"#bundle\0") and len(pkt) >= 16:
            off = 16
            while off + 4 <= len(pkt):
                size = struct.unpack_from(">i", pkt, off)[0]
                off += 4
                if size <= 0 or size % 4 or off + size > len(pkt):
                    break
                if not pkt[off:].startswith(b"#bundle"):
                    self.on_osc(pkt[off:off + size])
                off += size
            return
        try:
            end = pkt.index(b"\0")
            addr = pkt[:end].decode()
            toff = (end + 4) & ~3
            tend = pkt.index(b"\0", toff)
            tags = pkt[toff:tend].decode()
            aoff = (tend + 4) & ~3
            if tags[:2] == ",i":
                v = lv = struct.unpack_from(">i", pkt, aoff)[0]
            elif tags[:2] == ",f":
                f = struct.unpack_from(">f", pkt, aoff)[0]
                lv = (0 if not math.isfinite(f) or f <= 0 else round(f * 255) if f <= 1
                      else min(255, round(f)))
            else:
                return
        except (ValueError, UnicodeDecodeError, struct.error):
            return
        with NODE.lock:
            if addr.startswith("/ch/") and addr[4:].isdigit() and 1 <= int(addr[4:]) <= MAX_CH:
                NODE.web[int(addr[4:]) - 1] = max(0, min(255, lv))
            elif addr == "/master":
                NODE.master = max(0, min(255, lv))


NET = NetInputs()


def udp_loop(host, port, handler, multicast_uni=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    if multicast_uni is not None and host in ("0.0.0.0", ""):
        group = socket.inet_aton("239.255.%d.%d" % (multicast_uni >> 8, multicast_uni & 0xFF))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, group + socket.inet_aton("0.0.0.0"))
    while True:
        pkt, addr = s.recvfrom(2048)
        handler(pkt, addr, s)



def ival(q, k, d=0):
    try:
        return int(q.get(k, [d])[0])
    except (ValueError, TypeError):
        return d


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "vizzz.di-sim"

    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", ctype="text/plain"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, obj):
        self._send(200, json.dumps(obj), "application/json")

    def do_POST(self):
        self.do_GET()

    def do_GET(self):
        u = urlparse(self.path)
        p, q = u.path, parse_qs(u.query)

        if p == "/ws":
            return self._websocket()
        if p in PAGE_ROUTES:
            return self._send(200, APP_HTML, "text/html; charset=utf-8")

        n = NODE
        if p == "/status":
            return self._json(n.status())
        if p == "/node/manifest" or p == "/manifest.json":
            return self._json(n.manifest())
        if p == "/safety/status":
            return self._json({"burn_safe": n.burn_safe, "ceiling": n.ceiling(),
                               "slew": n.slew_step()})
        if p == "/monitor":
            return self._json({"out": n.out[:64]})
        if p == "/page":
            i = clamp(ival(q, "i", 0), 0, PAGE_COUNT - 1)
            s = i * PAGE_SIZE
            return self._json({"i": i, "web": n.web[s:s + PAGE_SIZE],
                               "out": n.out[s:s + PAGE_SIZE]})
        if p == "/peers":
            return self._json({"peers": n.peers, "count": len(n.peers)})
        if p == "/discover":
            return self._json({"name": n.name, "mac": n.mac, "ip": "192.168.4.1",
                               "uni": n.status()["uni15"]})
        if p == "/groups":
            return self._json({"groups": n.groups})
        if p == "/fixtures":
            return self._json({"fixtures": n.fixtures[:n.fixture_count], "count": n.fixture_count, "max": MAX_FIXTURES})
        if p == "/cue/status":
            return self._json({"run": n.cue["run"], "count": n.cue["count"],
                               "index": n.cue["index"], "steps": n.cue["steps"][:n.cue["count"]]})
        if p == "/fx/status":
            return self._json(dict(n.fx, **{"color": n.color}))
        if p == "/wifi/scan":
            return self._json({"scanning": False, "networks": [
                {"ssid": "sim-net", "rssi": -54, "enc": 3},
                {"ssid": "venue-wifi", "rssi": -71, "enc": 3},
                {"ssid": "open-guest", "rssi": -80, "enc": 0}]})

        with n.lock:
            if p == "/set":
                if "ch" not in q or "v" not in q:
                    return self._send(400, "missing arg")
                n.web[clamp(ival(q, "ch", 1), 1, MAX_CH) - 1] = clamp(ival(q, "v"), 0, 255)
            elif p == "/blackout" or p == "/net/blackout":
                n.web = [0] * MAX_CH; n.fade = None
            elif p == "/full" or p == "/net/full":
                n.web = [255] * MAX_CH; n.fade = None
            elif p in ("/master", "/net/master"):
                n.master = clamp(ival(q, "v", n.master), 0, 255)
            elif p == "/mode/set":
                n.mode = clamp(ival(q, "m", n.mode), 0, 2)
            elif p == "/mode/fallback":
                n.artnet_fallback = bool(ival(q, "en", 1))
            elif p == "/netmode/set":
                n.net_mode = clamp(ival(q, "m", n.net_mode), 0, 2)
            elif p == "/web/set":
                n.web_enabled = bool(ival(q, "en", 1))
            elif p == "/safety/set":
                n.burn_safe = bool(ival(q, "burn", 0))
            elif p == "/artout/set":
                n.art_out = bool(ival(q, "en", 0))
            elif p == "/artout/peer":
                n.art_out_target = q.get("ip", [""])[0]
            elif p == "/artnet/set":
                n.art["net"] = clamp(ival(q, "net", n.art["net"]), 0, 127)
                n.art["subnet"] = clamp(ival(q, "subnet", n.art["subnet"]), 0, 15)
                n.art["uni"] = clamp(ival(q, "uni", n.art["uni"]), 0, 15)
            elif p == "/scene/save":
                n.scenes[clamp(ival(q, "n"), 0, SCENE_COUNT - 1)] = list(n.web)
            elif p in ("/scene/recall", "/net/scene/recall"):
                s = n.scenes[clamp(ival(q, "n"), 0, SCENE_COUNT - 1)]
                f = max(0, ival(q, "fade", 0))
                if f == 0:
                    n.web = list(s); n.fade = None
                else:
                    n.fade = (time.time(), f, list(n.web), list(s))
            elif p == "/group/set":
                g = n.groups[clamp(ival(q, "g"), 0, 7)]
                if "name" in q: g["name"] = q["name"][0]
                if "start" in q: g["start"] = clamp(ival(q, "start", 1), 1, MAX_CH)
                if "end" in q: g["end"] = clamp(ival(q, "end", 1), 1, MAX_CH)
                if "en" in q: g["en"] = bool(ival(q, "en", 1))
            elif p == "/group/apply":
                g = n.groups[clamp(ival(q, "g"), 0, 7)]
                v = clamp(ival(q, "v"), 0, 255)
                for c in range(g["start"] - 1, min(g["end"], MAX_CH)):
                    n.web[c] = v
            elif p == "/cue/count":
                n.cue["count"] = clamp(ival(q, "c", 1), 1, 16)
            elif p == "/cue/set":
                i = clamp(ival(q, "i"), 0, 15)
                st = n.cue["steps"][i]
                if "scene" in q: st["scene"] = clamp(ival(q, "scene"), 0, SCENE_COUNT - 1)
                if "dwell" in q: st["dwell"] = max(0, ival(q, "dwell", 1000))
                if "fade" in q: st["fade"] = max(0, ival(q, "fade", 0))
            elif p == "/cue/run":
                n.cue["run"] = bool(ival(q, "en", 0)); n._cue_at = time.time()
            elif p == "/cue/next":
                n.cue["index"] = (n.cue["index"] + 1) % n.cue["count"]
            elif p == "/fx/set":
                if "mode" in q: n.fx["mode"] = ival(q, "mode", 0)
                if "en" in q: n.fx["en"] = bool(ival(q, "en", 0))
                if "bpm" in q: n.fx["bpm"] = clamp(ival(q, "bpm", 120), 20, 240)
                if "depth" in q: n.fx["depth"] = clamp(ival(q, "depth", 128), 0, 255)
            elif p == "/fx/tap":
                now = time.time(); n.tap = [t for t in n.tap if now - t < 3] + [now]
                if len(n.tap) >= 2:
                    gaps = [b - a for a, b in zip(n.tap, n.tap[1:])]
                    n.fx["bpm"] = clamp(int(60.0 / (sum(gaps) / len(gaps))), 20, 240)
            elif p == "/color/set":
                if "en" in q: n.color["en"] = bool(ival(q, "en", 0))
                for k in "rgb":
                    if k in q: n.color[k] = clamp(ival(q, k, 255), 0, 255)
            elif p == "/node/set":
                if "name" in q: n.name = q["name"][0]
                if "ap_ssid" in q: n.ap_ssid = q["ap_ssid"][0]
            elif p in ("/wifi/set", "/wifi/forget", "/reboot", "/factory-reset",
                       "/peer/cmd"):
                pass
            else:
                return self._send(404, "no route: " + p)
        return self._send(204)

    # ---- minimal RFC6455 server: handshake, then text frames out ----
    def _websocket(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            return self._send(400, "not a websocket request")
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write((
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        self.wfile.flush()
        try:
            while True:
                self.wfile.write(self._frame(json.dumps(NODE.status())))
                self.wfile.flush()
                time.sleep(0.4)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    @staticmethod
    def _frame(text):
        p = text.encode()
        h = bytearray([0x81])
        if len(p) < 126:
            h.append(len(p))
        elif len(p) < (1 << 16):
            h.append(126); h += struct.pack(">H", len(p))
        else:
            h.append(127); h += struct.pack(">Q", len(p))
        return bytes(h) + p


def ticker():
    while True:
        NET.expire()
        NODE.tick()
        time.sleep(0.023)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--udp", action="store_true", help="also listen for Art-Net, sACN and OSC")
    ap.add_argument("--artnet-port", type=int, default=6454)
    ap.add_argument("--sacn-port", type=int, default=5568)
    ap.add_argument("--osc-port", type=int, default=9000)
    a = ap.parse_args()
    if a.udp:
        for port, h, mc in ((a.artnet_port, lambda p, ad, s: NET.on_artnet(p, ad, s), None),
                            (a.sacn_port, lambda p, ad, s: NET.on_sacn(p), NODE.uni15() + 1),
                            (a.osc_port, lambda p, ad, s: NET.on_osc(p), None)):
            threading.Thread(target=udp_loop, args=(a.host, port, h, mc), daemon=True).start()
    APP_HTML = extract_app_html()
    threading.Thread(target=ticker, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print("vizzz.di simulator on http://%s:%d  (%d bytes of console UI)"
          % (a.host, a.port, len(APP_HTML)))
    srv.serve_forever()
