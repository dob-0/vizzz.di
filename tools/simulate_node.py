#!/usr/bin/env python3
"""Run the vizzz.di browser console on this machine, with no ESP32.

Serves di.iiii's desk UI files the way the firmware does (gzip, ETag, 304, the
301s for the old console pages) against an in-memory model of the firmware: web layer, master dimmer, safety ceiling and slew, scenes, FX,
colour, cues, groups and a fake peer table. DMX output is computed the same way
the firmware computes it, so /monitor and /page show believable numbers.

    python3 tools/fetch_desk_ui.py --from <di.iiii checkout> --cache tools/.desk_ui_cache   # once
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
import argparse, base64, gzip, hashlib, io, json, math, os, re, select, socket, struct, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "src", "main.cpp")
# The firmware's USB widget baud, read from main.cpp so the two never disagree.
with open(MAIN, encoding="utf-8") as _f:
    USB_WIDGET_BAUD = int(re.search(r"USB_WIDGET_BAUD\s*=\s*(\d+)", _f.read()).group(1))
MAX_CH, SCENE_COUNT, PAGE_SIZE, PAGE_COUNT, MAX_FIXTURES = 512, 8, 32, 16, 12
SAFE_MAX_LEVEL, SAFE_SLEW_STEP = 245, 18
BURN_SAFE_MAX_LEVEL, BURN_SAFE_SLEW_STEP = 96, 8
# GET /api/dmx/stream (V5): the rate, keepalive, cap and gap are read from the firmware's header.
with open(os.path.join(ROOT, "include", "vizzz_stream.h"), encoding="utf-8") as _f:
    _st = _f.read()
STREAM = {k: int(re.search(r"constexpr \w+ %s\s*=\s*(\d+)" % k, _st).group(1))
          for k in ("kPeriodMs", "kKeepaliveMs", "kMaxClients", "kGap", "kRetryMs")}
DESK_CACHE = os.path.join(ROOT, "tools", ".desk_ui_cache")
LOCK = os.path.join(ROOT, "desk_ui.lock")
MIME = {".html": "text/html", ".js": "application/javascript", ".css": "text/css"}


def gz9(b):
    """Same stream as tools/fetch_desk_ui.py (level 9, mtime 0, no file name)."""
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, compresslevel=9, mtime=0) as g:
        g.write(b)
    return buf.getvalue()


def load_desk_ui(cache=DESK_CACHE, lock=LOCK):
    """{route path: (mime, gz bytes, etag)} from the locked raw files, each checked
    against desk_ui.lock. Exits with a clear message if a file is absent or differs."""
    with open(lock) as f:
        meta = json.load(f)
    files, missing = {}, []
    for e in meta["files"]:
        fp = os.path.join(cache, e["path"])
        if not os.path.isfile(fp):
            missing.append(e["path"])
            continue
        raw = open(fp, "rb").read()
        if hashlib.sha256(raw).hexdigest() != e["sha256"]:
            raise SystemExit("desk UI file %s in %s does not match desk_ui.lock (sha256); "
                             "re-run tools/fetch_desk_ui.py --cache" % (e["path"], cache))
        route = "/" if e["path"] == "index.html" else "/" + e["path"]
        files[route] = (MIME[os.path.splitext(e["path"])[1]], gz9(raw), e["sha256"][:16])
    if missing:
        raise SystemExit(
            "desk UI files missing in %s: %s\nfetch them first:\n"
            "  python3 tools/fetch_desk_ui.py --from <di.iiii checkout> --cache %s\n"
            "(or without --from to read GitHub at the locked commit)"
            % (cache, ", ".join(missing), cache))
    return files


def load_redirects(path=None):
    """vizzz::kDeskRedirects: the same list the firmware answers 301 from."""
    src = open(path or CORE, encoding="utf-8").read()
    return dict(re.findall(r'\{"(/[^"]*)",\s*"(/[^"]*)"\}', src[src.index("kDeskRedirects"):]))


def etag_matches(header, etag):
    """vizzz::etagMatches: "*", or any listed entry equal to "<etag>" (W/ allowed)."""
    for part in header.split(","):
        part = part.strip()
        if part == "*":
            return True
        if part.startswith("W/"):
            part = part[2:]
        if part == '"%s"' % etag:
            return True
    return False


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
            ident_tick(self)
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
        if net:
            DESK.hold_released = False          # a live source ends an api/input/release
        if self.mode == 0:
            base = list(self.web)
        elif self.mode == 1:
            fallback = self.artnet_fallback or DESK.hold_released
            base = list(net) if net else (list(self.web) if fallback else list(self.out))
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
            # USB widget input exists only on hardware (no serial in the simulator).
            "usb_active": False, "usb_frames": 0, "usb_baud": USB_WIDGET_BAUD,
            "api_replies": DESK.replies, "api_busy": DESK.busy,
            "heap_free": 180000, "heap_max_block": 110000, "ws_clients": 0,
            "sse_clients": DESK.sse,
        }

    def manifest(self):
        return {
            "schema": "vizzz.di.node.manifest.v1", "kind": "firmware-node",
            "product": "vizzz.di", "name": self.name, "fw": "simulator",
            "firmware": {"tag": "simulator", "git": "simulator", "framework": "python"},
            "hardware": {"board": "ESP32 DevKit (simulated)", "dmx_uart": "DMX_NUM_1",
                         "dmx_tx_gpio": 25, "dmx_dir_gpio": 21,
                         "max_channels": MAX_CH, "dmx_period_ms": 23},
            "protocols": [
                {"id": "http", "role": "control", "console_header": "X-Vizzz-Console: 1",
                 "routes": [r for r, k in ROUTES.items() if k != "DeskApi"],
                 "route_table": [{"path": r, "methods": ROUTE_METHODS[k], "console_header": k == "Console",
                                  "cors": k == "GetShared"} for r, k in ROUTES.items() if k != "DeskApi"]},
                {"id": "desk-api", "role": "control", "prefix": "/api/", "paths": len(DESK_API),
                 "contract_version": 1, "post_content_type": "application/json", "max_body": MAX_BODY},
                {"id": "websocket", "role": "live-status", "endpoint": "/ws", "push_ms": 400},
                {"id": "artnet", "role": "input-output", "port": 6454,
                 "universe": self.uni15(), "net": self.art["net"], "subnet": self.art["subnet"],
                 "uni": self.art["uni"], "output_enabled": self.art_out},
                {"id": "sacn", "role": "input", "port": 5568,
                 "universe": self.uni15() + 1,           # E1.31 §6.2.7: never 0
                 "universe_rule": "artnet port-address + 1"},
                {"id": "osc", "role": "input", "transport": "udp", "port": 9000},
                {"id": "usb", "role": "input", "transport": "serial", "uart": "UART0",
                 "protocol": "ENTTEC DMX USB PRO widget API", "baud": USB_WIDGET_BAUD,
                 "labels": [3, 4, 6, 10], "merge": "htp-with-artnet-sacn",
                 "timeout_ms": 2500, "active": False, "frames": 0},
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



CORE = os.path.join(ROOT, "include", "vizzz_core.h")


def load_route_table(path=CORE):
    """The firmware's own route table (vizzz::kRoutes): one source for both."""
    src = open(path, encoding="utf-8").read()
    table = dict(re.findall(r'\{"(/[^"]*)",\s*RouteKind::(\w+)\}', src))
    if not table:
        raise SystemExit("vizzz::kRoutes not found in %s" % path)
    return table


ROUTES = load_route_table()
REDIRECTS = load_redirects()
DESK_UI = load_desk_ui()   # exits with a message when the cached files are absent
ROUTE_METHODS = {"GetLocal": "GET", "GetShared": "GET", "Operator": "GET, POST", "Console": "POST",
                 "Static": "GET"}   # DeskApi: per path, from vizzz::desk::kDeskApi (DESK_API below)
FADE_MAX_MS, MAX_GROUPS, MAX_CUES = 600000, 8, 16
FX_NAMES = ["none", "strobe", "chase", "pulse", "sine", "sparkle", "comet", "bars", "glitch", "radar"]


# ── Desk API (/api/*, firmware plan step V4) ─────────────────────────────────
# The same paths, shapes and refusals as src/main.cpp + include/vizzz_desk.h,
# from this model's state. The route table, roles and profiles are read from
# vizzz_desk.h, so the simulator and the firmware have one source for them.
DESK_H = os.path.join(ROOT, "include", "vizzz_desk.h")


def load_desk_tables(path=DESK_H):
    src = open(path, encoding="utf-8").read()
    masks = {"kGet": ("GET",), "kPost": ("POST",), "kGet | kPost": ("GET", "POST")}
    api = {p: (masks[m], c.lower()) for p, m, c in
           re.findall(r'\{"(/api/[^"]*)",\s*(kGet \| kPost|kGet|kPost),\s*ApiClass::(\w)\}', src)}
    enum = re.search(r"enum class Role : uint8_t \{([^}]*)\}", src).group(1)
    enum = [e.strip() for e in enum.split(",")]
    roles = re.findall(r'\{"(\w+)", (\d+), Kind::(\w+)\}', src)
    kinds = {e: (name, kind.lower()) for e, (name, _d, kind) in zip(enum, roles)}
    profiles = {}
    for name, cat, _n, body in re.findall(r'\{"(\w+)",\s*"(_\w+)",\s*(\d+),\s*\{([^}]*)\}\}', src):
        profiles[name] = {"cat": cat, "roles": [kinds[r.strip().split("::")[-1]][0]
                                                 for r in body.split(",") if r.strip()]}
    role_kind = {name: kind for name, kind in kinds.values()}
    if not api or not profiles:
        raise SystemExit("desk tables not found in %s" % path)
    return api, profiles, role_kind, [kinds[e][0] for e in enum]


DESK_API, PROFILES, ROLE_KIND, ROLE_ORDER = load_desk_tables()
PROFILE_ORDER = list(PROFILES)
NOT_ON_NODE = "not on this node"
MAX_BODY, SCENE_NAME_MAX, FIXTURE_NAME_MAX = 2048, 23, 15
BY_WIDTH = {1: "dimmer", 2: "ds", 3: "rgb", 4: "drgb", 5: "drgbw", 6: "drgbwa", 9: "ppttdrgbw"}
EMPTY_CUES = {"project": None, "list": [], "loop": False, "autoplay": False, "index": -1, "running": False,
              "nextAt": None, "missing": [], "n": 0, "name": None, "nextInMs": None}
DEFAULT_LIMITS = {"dimMin": 0, "dimMax": 255, "panMin": 0, "panMax": 255, "tiltMin": 0, "tiltMax": 255,
                  "invertPan": False, "invertTilt": False, "swapPT": False}


class DeskRefusal(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code, self.msg = code, msg


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def truthy(v):
    if v is None or v is False:
        return False
    if is_num(v):
        return v != 0
    if isinstance(v, str):
        return v != ""
    return True


def parse_id(s, prefix, mx):
    """vizzz::desk::parseId: prefix + 1..max, digits only, no leading zero."""
    if not isinstance(s, str) or not re.fullmatch(prefix + r"[1-9][0-9]*", s):
        return -1
    v = int(s[1:])
    return v - 1 if v <= mx else -1


def clean_name(v, cap):
    """vizzz::desk::cleanName on UTF-8 bytes: drop controls, trim, at most cap-1 bytes."""
    if not isinstance(v, str):
        return ""
    b = "".join(c for c in v.lstrip(" \t") if ord(c) >= 0x20 and ord(c) != 0x7f).encode("utf-8")[:cap - 1]
    return b.decode("utf-8", "ignore").rstrip(" \t")


def level(v):
    return 0 if not v > 0 else 255 if v >= 255 else int(v + 0.5)


def clamp_fade(v):
    return 0 if not v > 0 else FADE_MAX_MS if v >= FADE_MAX_MS else int(v + 0.5)


def pos_to_milli(b):
    return (b * 1000 + 127) // 255


def unit_to_byte(v):
    if not v > 0:
        return 0
    if v >= 1:
        return 255
    return (min(1000, int(v * 1000 + 0.5)) * 255 + 500) // 1000


def unit255(b):
    m = pos_to_milli(b)
    return 1 if m >= 1000 else (0 if m == 0 else float("%d.%03d" % (0, m)))


class DeskState:
    """The desk-side facts the firmware keeps beside its old state (V4)."""
    def __init__(self):
        self.saved = set()                  # scene slots holding a scene
        self.scene_name = [""] * SCENE_COUNT
        self.scene_fade = [0] * SCENE_COUNT
        self.fix_prof = [None] * MAX_FIXTURES
        self.active = -1
        self.ident = None                   # {"start", "dur", "saved": {ch: v}, "slots": set}
        self.hold_released = False
        self.replies = 0
        self.busy = 0
        self.sse = 0                        # live /api/dmx/stream clients
        self.sse_lock = threading.Lock()


DESK = DeskState()


def fixture_view(n, i):
    """main.cpp deskFixture_locked(): profile from the slot, or picked by width."""
    f = n.fixtures[i]
    width = f["end"] - f["start"] + 1 if f["end"] >= f["start"] else 1
    p = DESK.fix_prof[i]
    if p is None or len(PROFILES[p]["roles"]) != width:
        p = BY_WIDTH.get(width, "dimmer")
    return {"slot": i, "name": f["name"][:FIXTURE_NAME_MAX], "start": f["start"], "profile": p,
            "width": len(PROFILES[p]["roles"]), "x": f["x"], "y": f["y"],
            "listed": f["en"] and i < n.fixture_count}


def views(n):
    return [fixture_view(n, i) for i in range(MAX_FIXTURES)]


def role_ch(fv, role):
    roles = PROFILES[fv["profile"]]["roles"]
    if role not in roles:
        return -1
    ch = fv["start"] - 1 + roles.index(role)
    return ch if ch < MAX_CH else -1


def values_from(fv, frame):
    out = {}
    for role in PROFILES[fv["profile"]]["roles"]:
        ch = role_ch(fv, role)
        if ch >= 0:
            out[role] = frame[ch]
    return out


def fixture_obj(fv, frame):
    return {"id": "f%d" % (fv["slot"] + 1), "index": fv["slot"] + 1, "name": fv["name"], "universe": 0,
            "address": fv["start"], "profile": fv["profile"], "on": True, "x": unit255(fv["x"]),
            "y": unit255(fv["y"]), "values": values_from(fv, frame), "limits": dict(DEFAULT_LIMITS)}


def end_of(fv):
    return fv["start"] + fv["width"] - 1


def in_range(fv, s, e):
    return fv["start"] >= s and end_of(fv) <= e


def group_listed(n, fvs, g):
    gr = n.groups[g]
    return gr["en"] and any(f["listed"] and in_range(f, gr["start"], gr["end"]) for f in fvs)


def group_obj(n, fvs, g):
    gr = n.groups[g]
    return {"id": "g%d" % (g + 1), "name": gr["name"][:15],
            "ids": ["f%d" % (f["slot"] + 1) for f in fvs if f["listed"] and in_range(f, gr["start"], gr["end"])]}


def scene_obj(n, fvs, s):
    listed = [f for f in fvs if f["listed"]]
    return {"id": "s%d" % (s + 1), "name": DESK.scene_name[s] or "Scene %d" % (s + 1),
            "fadeMs": DESK.scene_fade[s],
            "fixtures": [{"id": "f%d" % (f["slot"] + 1), "on": True, "values": values_from(f, n.scenes[s])}
                         for f in listed],
            "raw": {}, "live": len(listed), "missing": 0}


def chase_obj(n):
    steps = n.cue["steps"][:n.cue["count"]]
    return {"enabled": n.cue["run"], "sceneIds": ["s%d" % (st["scene"] + 1) for st in steps if st["scene"] in DESK.saved],
            "holdMs": steps[0]["dwell"] if steps else 0, "fadeMs": steps[0]["fade"] if steps else 0,
            "sync": "clock", "beats": 4}


def fx_obj(n):
    return {"mode": FX_NAMES[n.fx["mode"]], "bpm": n.fx["bpm"], "depth": n.fx["depth"], "enabled": n.fx["en"],
            "spatial": "patch", "exclude": []}


def output_obj(n):
    t = n.art_out_target
    return {"driver": "artnet", "serialPort": "", "mode": "unicast" if t else "broadcast",
            "targets": [t] if t else [], "enabled": n.art_out, "refreshHz": 1000 // 23, "extra": []}


SIM_IFACES = [{"iface": "ap", "address": "192.168.4.1", "netmask": "255.255.255.0"},
              {"iface": "sta", "address": "192.168.88.140", "netmask": "255.255.255.0"}]


def show_obj(n):
    return {"space": None, "label": n.name, "file": "", "saved": True, "live": n.art_out, "machine": None}


def state_obj(n):
    fvs = views(n)
    fixtures = [fixture_obj(f, n.web) for f in fvs if f["listed"]]
    profiles = {}
    for name in PROFILE_ORDER:
        roles = PROFILES[name]["roles"]
        profiles[name] = {"label": "%s (%d Channel%s)" % (name, len(roles), "" if len(roles) == 1 else "s"),
                          "channels": list(roles), "cat": PROFILES[name]["cat"], "custom": False}
    role_kinds = {k: [r for r in ROLE_ORDER if ROLE_KIND[r] == k]
                  for k in ("emitter", "level", "position", "fine", "control")}
    return {
        "master": n.master, "blackout": False, "fixtures": fixtures,
        "groups": [group_obj(n, fvs, g) for g in range(MAX_GROUPS) if group_listed(n, fvs, g)],
        "raw": {}, "scenes": [scene_obj(n, fvs, s) for s in range(SCENE_COUNT) if s in DESK.saved],
        "activeScene": "s%d" % (DESK.active + 1) if DESK.active in DESK.saved else None,
        "chase": chase_obj(n), "fx": fx_obj(n), "fxModes": list(FX_NAMES),
        "lfos": [], "layers": [], "looks": [], "sets": [], "customProfiles": [],
        "audioCfg": {"enabled": False, "mode": "level", "amount": 255, "release": 300, "useBeats": True},
        "output": output_obj(n), "profiles": profiles, "roleKinds": role_kinds,
        "caps": {"midi": False, "audio": False, "library": False, "looks": False, "layers": False, "lfos": False,
                 "sets": False, "cues": False, "profilesEdit": False, "multiUniverse": False, "stream": True,
                 "node": True},
        "show": show_obj(n),
        "status": {"universes": [0], "nodes": [], "interfaces": SIM_IFACES,
                   "broadcast": ["192.168.4.255", "192.168.88.255"], "packetsSent": 0, "lastError": None,
                   "serial": None, "identifying": [], "extra": [], "driver": "artnet",
                   "outputEnabled": n.art_out, "fading": False},
    }


def input_obj(n):
    """vizzz::desk::writeInput, from the simulator's network inputs."""
    enabled = n.mode != 0
    desk = "follow" if n.mode == 1 else "htp"
    pa = n.uni15()
    art, sacn = NET.art_active(), NET.sacn_active()
    art_age = int((time.time() - NET.last_art) * 1000) if NET.last_art else -1
    live = art or sacn
    held = n.mode == 1 and not n.artnet_fallback and not DESK.hold_released and not live and art_age >= 0
    state = "live" if live else "held" if held else "lost" if art_age >= 0 else "waiting"
    sources = []
    if art_age >= 0:
        sources.append({"key": "artnet", "protocol": "artnet", "name": "Art-Net", "ip": "", "priority": 100,
                        "fps": 0, "ageMs": art_age, "packets": 0, "outOfSequence": 0, "sources": len(NET.art),
                        "winning": art, "lost": not art, "lostWhy": None})
    if live:
        names = [x for x, on in (("Art-Net", art), ("sACN", sacn)) if on]
        summary = {"enabled": True, "text": "Following %s on 1 of 1 universe" % ", ".join(names),
                   "level": "ok", "live": 1, "universes": 1}
    elif enabled:
        summary = {"enabled": True, "text": {"held": "No signal — holding the last look",
                                             "lost": "No signal — the desk has the rig"}.get(state, "Listening — no signal yet"),
                   "level": "warn", "live": 0, "universes": 1}
    else:
        summary = {"enabled": False, "text": "Input off"}
    su = pa + 1
    return {
        "config": {"enabled": enabled, "artnet": True, "sacn": True,
                   "interfaces": [i["address"] for i in SIM_IFACES],
                   "universes": [{"universe": pa, "merge": "htp", "desk": desk}],
                   "loss": "release" if n.artnet_fallback else "hold", "name": n.name},
        "interfaces": [dict(i, internal=False) for i in SIM_IFACES],
        "listening": [{"protocol": "artnet", "address": "0.0.0.0", "port": 6454, "ok": True, "error": None,
                       "broadcast": True},
                      {"protocol": "sacn", "address": "0.0.0.0", "port": 5568, "ok": True, "error": None,
                       "groups": ["239.255.%d.%d" % (su >> 8, su & 255)], "groupErrors": []}],
        "lines": [{"universe": pa, "merge": "htp", "desk": desk,
                   "artnet": "%d:%d:%d" % ((pa >> 8) & 0x7f, (pa >> 4) & 15, pa & 15), "sacn": su,
                   "state": state, "lastAt": None, "ageMs": art_age if art_age >= 0 else None,
                   "sources": sources, "exceeded": None}],
        "unlistened": [], "stats": {}, "problems": [], "foreign": 0, "polls": None,
        "summary": summary, "lanAllowed": True,
    }


def overlap_slot(fvs, start, width, ignore=()):
    end = start + width - 1
    for f in fvs:
        if f["slot"] in ignore or not f["listed"]:
            continue
        if f["start"] <= end and end_of(f) >= start:
            return f["slot"]
    return -1


def next_free(fvs, width):
    a = 1
    while a + width - 1 <= MAX_CH:
        hit = overlap_slot(fvs, a, width)
        if hit < 0:
            return a
        a = end_of(fvs[hit]) + 1
    return 0


def id_set(lst, one=None):
    src = lst if isinstance(lst, list) else [one]
    return {s for s in (parse_id(v, "f", MAX_FIXTURES) for v in src) if s >= 0}


def role_values(values):
    if not isinstance(values, dict):
        return []
    return [(k, level(v)) for k, v in values.items() if k in ROLE_KIND and is_num(v)][:16]


def write_values(n, fv, vals):
    for role, v in vals:
        ch = role_ch(fv, role)
        if ch >= 0:
            n.web[ch] = v


def start_fade(n, target, ms):
    DESK.ident = None
    if ms <= 0:
        n.web = list(target)
        n.fade = None
    else:
        n.fade = (time.time(), ms, list(n.web), list(target))


def ident_tick(n):
    """loop(): identify ends -> the saved values are written back."""
    i = DESK.ident
    if i and (time.time() - i["start"]) * 1000 >= i["dur"]:
        for ch, v in i["saved"].items():
            n.web[ch] = v
        DESK.ident = None


def desk_api(n, method, p, b):
    """One /api request against the model. Returns (code, obj). Call with n.lock held."""
    cls = DESK_API[p][1]
    if cls == "c":
        raise DeskRefusal(404, NOT_ON_NODE)
    if p == "/api/cues" and method == "GET":
        return 200, {"cues": dict(EMPTY_CUES)}
    if p.startswith("/api/cues/"):
        return 200, {"error": NOT_ON_NODE, "cues": dict(EMPTY_CUES)}
    if p == "/api/show":
        return 200, show_obj(n)
    if p == "/api/looks":
        return 200, {"looks": []}
    if cls == "b":
        return 200, {"error": NOT_ON_NODE}
    if p == "/api/state":
        return 200, state_obj(n)
    if p == "/api/dmx":
        return 200, {"dmx": {"0": list(n.out)}, "master": n.master, "blackout": False, "looks": [], "cues": None}
    if p == "/api/input" and method == "GET":
        return 200, input_obj(n)
    return DESK_POST[p](n, b)


def p_master(n, b):
    m = b.get("master")
    if m is not None and not is_num(m):
        raise DeskRefusal(400, "master must be a number 0..255")
    if m is not None:
        n.master = level(m)
    if truthy(b.get("blackout")):          # applyBlackout_locked()
        DESK.ident = None
        n.web = [0] * MAX_CH; n.fade = None; n.cue["run"] = False; n.fx["en"] = False
        DESK.active = -1
    return 200, {"ok": True}


def refuse_off(b):
    if "on" in b and b["on"] is not None and not truthy(b["on"]):
        raise DeskRefusal(400, "on:false is not stored on this node (no mute mask yet): set the levels to 0")


def p_fixture(n, b):
    slot = parse_id(b.get("id"), "f", MAX_FIXTURES)
    if slot < 0:
        raise DeskRefusal(404, "no such fixture")
    refuse_off(b)
    if any(b.get(k) is not None for k in ("address", "universe", "profile", "index", "limits")):
        raise DeskRefusal(400, "api/fixture changes values, name and x/y here: re-patch with fixtures/readdress")
    if b.get("values") is not None and not isinstance(b["values"], dict):
        raise DeskRefusal(400, "values must be an object")
    fv = fixture_view(n, slot)
    if not fv["listed"]:
        raise DeskRefusal(404, "no such fixture")
    vals = role_values(b.get("values"))
    if vals:
        write_values(n, fv, vals); n.fade = None; DESK.active = -1
    name = clean_name(b.get("name"), FIXTURE_NAME_MAX + 1)
    if name:
        n.fixtures[slot]["name"] = name
    for k in ("x", "y"):
        if is_num(b.get(k)):
            n.fixtures[slot][k] = unit_to_byte(b[k])
    return 200, {"ok": True, "fixture": fixture_obj(fixture_view(n, slot), n.web)}


def p_fixtures_all(n, b):
    refuse_off(b)
    vals = role_values(b.get("values"))
    if vals:
        for fv in views(n):
            if fv["listed"]:
                write_values(n, fv, vals)
        n.fade = None; DESK.active = -1
    return 200, {"ok": True}


def universe_zero(v):
    return v is None or (is_num(v) and v == 0)


def p_raw(n, b):
    if truthy(b.get("clear")):
        return 200, {"ok": True, "raw": {}}
    sets = []

    def take(c):
        if not isinstance(c, dict) or not universe_zero(c.get("universe")):
            raise DeskRefusal(400, "only universe 0 on this node")
        ch, v = c.get("channel"), c.get("value")
        if not is_num(ch) or ch < 1 or ch > MAX_CH or ch != int(ch):
            raise DeskRefusal(400, "channel must be 1..512")
        if v is None or (is_num(v) and v < 0):
            return
        if not is_num(v):
            raise DeskRefusal(400, "value must be a number 0..255")
        if len(sets) >= 64:
            raise DeskRefusal(400, "at most 64 channels per request")
        sets.append((int(ch) - 1, level(v)))
    if isinstance(b.get("channels"), list):
        for c in b["channels"]:
            take(c)
    elif b.get("channel") is not None:
        take(b)
    for ch, v in sets:
        n.web[ch] = v
    if sets:
        n.fade = None; DESK.active = -1
    return 200, {"ok": True, "raw": {}}


def p_fixtures_add(n, b):
    prof = "rgb"
    if b.get("profile") is not None:
        prof = b["profile"] if isinstance(b["profile"], str) and b["profile"] in PROFILES else None
        if prof is None:
            raise DeskRefusal(400, "unknown profile: the node patches its built-in profiles only (state.profiles)")
    if not universe_zero(b.get("universe")):
        raise DeskRefusal(400, "only universe 0 on this node")
    width = len(PROFILES[prof]["roles"])
    count = 1
    if is_num(b.get("count")):
        count = 1 if b["count"] < 1 else int(b["count"])
    count = min(count, MAX_FIXTURES)
    address = 0
    a = b.get("address")
    if a is not None and not (is_num(a) and a == 0):
        if not is_num(a) or a < 1 or a > MAX_CH or a != int(a):
            raise DeskRefusal(400, "address must be 1..512")
        address = int(a)
    name = clean_name(b.get("name"), FIXTURE_NAME_MAX + 1) or prof
    fvs = views(n)
    if address and not truthy(b.get("force")):
        for i in range(count):
            start = address + i * width
            if start + width - 1 > MAX_CH:
                break
            if overlap_slot(fvs, start, width) >= 0:
                raise DeskRefusal(409, "those channels are taken by a patched fixture: patch somewhere else, or send force to stack them")
    added = []
    at = address or next_free(fvs, width)
    for _ in range(count):
        slot = next((f["slot"] for f in fvs if not f["listed"]), -1)
        if slot < 0 or at == 0 or at + width - 1 > MAX_CH:
            break
        n.fixtures[slot].update({"name": name, "start": at, "end": at + width - 1,
                                 "x": unit_to_byte(0.06 + (slot % 12) * 0.075),
                                 "y": unit_to_byte(0.28 + (slot // 12) * 0.16), "en": True})
        DESK.fix_prof[slot] = prof
        n.fixture_count = max(n.fixture_count, slot + 1)
        fvs[slot] = fixture_view(n, slot)
        added.append(slot)
        at = at + width if address else next_free(fvs, width)
    if not added:
        raise DeskRefusal(409, "no room: every fixture slot is used, or no free channels fit this profile")
    return 200, {"ok": True, "added": [fixture_obj(fvs[s], n.web) for s in sorted(added)]}


def p_fixtures_remove(n, b):
    if b.get("ids") is None and b.get("id") is None:
        raise DeskRefusal(400, "nothing to remove")
    removed = 0
    for s in id_set(b.get("ids"), b.get("id")):
        if fixture_view(n, s)["listed"]:
            n.fixtures[s]["en"] = False
            removed += 1
    return 200, {"ok": True, "removed": removed, "groupsRemoved": []}


def p_fixtures_move(n, b):
    for m in b.get("moves") if isinstance(b.get("moves"), list) else []:
        s = parse_id(m.get("id") if isinstance(m, dict) else None, "f", MAX_FIXTURES)
        if s < 0 or not fixture_view(n, s)["listed"]:
            continue
        for k in ("x", "y"):
            if is_num(m.get(k)):
                n.fixtures[s][k] = unit_to_byte(m[k])
    return 200, {"ok": True}


def p_fixtures_readdress(n, b):
    moves = b["moves"] if isinstance(b.get("moves"), list) else ([b] if b.get("id") is not None else [])
    plan = []
    for m in moves:
        s = parse_id(m.get("id") if isinstance(m, dict) else None, "f", MAX_FIXTURES)
        if s < 0:
            raise DeskRefusal(404, "no such fixture")
        if not universe_zero(m.get("universe")):
            raise DeskRefusal(400, "only universe 0 on this node")
        if m.get("address") is not None and not is_num(m["address"]):
            raise DeskRefusal(400, "address must be 1..512")
        plan.append((s, m.get("address")))
    if not plan:
        raise DeskRefusal(400, "nothing to move")
    fvs = views(n)
    to = {}
    for s, a in plan:
        f = fvs[s]
        if not f["listed"]:
            raise DeskRefusal(404, "no such fixture")
        addr = int(min(MAX_CH, max(1, f["start"] if a is None else a)))
        if addr + f["width"] - 1 > MAX_CH:
            raise DeskRefusal(409, "the fixture runs off the end of the universe there")
        to[s] = addr
    if not truthy(b.get("force")):
        planned = [dict(f, start=to.get(f["slot"], f["start"])) for f in fvs]
        for s in to:
            if overlap_slot(planned, planned[s]["start"], planned[s]["width"], ignore=(s,)) >= 0:
                raise DeskRefusal(409, "those channels are taken by a patched fixture: send force to stack them")
    for s, addr in to.items():
        n.fixtures[s]["start"], n.fixtures[s]["end"] = addr, addr + fvs[s]["width"] - 1
    DESK.active = -1
    return 200, {"ok": True, "moved": [{"id": "f%d" % (s + 1), "universe": 0, "address": to[s]} for s in sorted(to)]}


def p_fixtures_identify(n, b):
    off = truthy(b.get("off"))
    if off:
        if DESK.ident:
            DESK.ident["dur"] = 0
            ident_tick(n)
        return 200, {"ok": True, "identifying": []}
    slots = id_set(b.get("ids")) if isinstance(b.get("ids"), list) else set()
    if not slots:
        raise DeskRefusal(400, "select a fixture to find")
    seconds = 2
    if is_num(b.get("seconds")):
        seconds = 1 if b["seconds"] < 1 else 10 if b["seconds"] > 10 else int(b["seconds"] + 0.5)
    if n.fade:
        raise DeskRefusal(409, "a fade is running: identify once it has landed")
    i = DESK.ident or {"saved": {}, "slots": set()}
    for s in sorted(slots):
        fv = fixture_view(n, s)
        if not fv["listed"]:
            continue
        for role in PROFILES[fv["profile"]]["roles"]:
            if ROLE_KIND[role] not in ("level", "emitter"):
                continue
            ch = role_ch(fv, role)
            if ch < 0:
                continue
            i["saved"].setdefault(ch, n.web[ch])
            n.web[ch] = 255
        i["slots"].add(s)
    if not i["slots"]:
        raise DeskRefusal(400, "select a fixture to find")
    i["start"], i["dur"] = time.time(), seconds * 1000
    DESK.ident = i
    return 200, {"ok": True, "identifying": ["f%d" % (s + 1) for s in sorted(i["slots"])]}


def scene_meta(b):
    if any(b.get(k) is not None for k in ("fx", "lfos", "audioCfg")):
        raise DeskRefusal(400, "node scenes hold levels only (no fx, lfos or audio)")
    if b.get("fadeMs") is not None and not is_num(b["fadeMs"]):
        raise DeskRefusal(400, "fadeMs must be a number")
    return ("name" in b and b["name"] is not None, clean_name(b.get("name"), SCENE_NAME_MAX + 1),
            b.get("fadeMs") is not None, clamp_fade(b["fadeMs"]) if b.get("fadeMs") is not None else 0)


def p_scenes_save(n, b):
    _hn, name, _hf, fade = scene_meta(b)
    slot = next((s for s in range(SCENE_COUNT) if s not in DESK.saved), -1)
    if slot < 0:
        raise DeskRefusal(409, "all 8 scene slots are in use: remove one first")
    n.scenes[slot] = list(n.web)
    DESK.saved.add(slot)
    DESK.scene_name[slot], DESK.scene_fade[slot] = name, fade
    return 200, {"ok": True, "scene": scene_obj(n, views(n), slot)}


def p_scenes_update(n, b):
    slot = parse_id(b.get("id"), "s", SCENE_COUNT)
    if slot < 0:
        raise DeskRefusal(404, "no such scene")
    has_name, name, has_fade, fade = scene_meta(b)
    if slot not in DESK.saved:
        raise DeskRefusal(404, "no such scene")
    if truthy(b.get("restage")):
        n.scenes[slot] = list(n.web)
    if has_name:
        DESK.scene_name[slot] = name
    if has_fade:
        DESK.scene_fade[slot] = fade
    return 200, {"ok": True, "scene": scene_obj(n, views(n), slot)}


def p_scenes_recall(n, b):
    slot = parse_id(b.get("id"), "s", SCENE_COUNT)
    if slot < 0:
        raise DeskRefusal(404, "no such scene")
    if b.get("fadeMs") is not None and not is_num(b["fadeMs"]):
        raise DeskRefusal(400, "fadeMs must be a number")
    if slot not in DESK.saved:
        raise DeskRefusal(404, "no such scene")
    fade = clamp_fade(b["fadeMs"]) if is_num(b.get("fadeMs")) else DESK.scene_fade[slot]
    paused = n.cue["run"]
    n.cue["run"] = False
    start_fade(n, n.scenes[slot], fade)
    DESK.active = slot
    return 200, dict({"ok": True}, **({"chasePaused": True} if paused else {}))


def p_scenes_remove(n, b):
    slot = parse_id(b.get("id"), "s", SCENE_COUNT)
    if slot >= 0 and slot in DESK.saved:
        DESK.saved.discard(slot)
        n.scenes[slot] = [0] * MAX_CH
        DESK.scene_name[slot], DESK.scene_fade[slot] = "", 0
        if DESK.active == slot:
            DESK.active = -1
        keep = [st for st in n.cue["steps"][:n.cue["count"]] if st["scene"] != slot]
        n.cue["steps"][:len(keep)] = keep
        n.cue["count"] = len(keep)
        if n.cue["index"] >= n.cue["count"]:
            n.cue["index"] = 0
        if not keep:
            n.cue["run"] = False
    return 200, {"ok": True}


def p_groups_add(n, b):
    sel = id_set(b.get("ids")) if isinstance(b.get("ids"), list) else set()
    fvs = views(n)
    picked = [f for f in fvs if f["slot"] in sel and f["listed"]]
    if not picked:
        raise DeskRefusal(400, "select fixtures to group")
    lo, hi = min(f["start"] for f in picked), max(end_of(f) for f in picked)
    if any(f["listed"] and f["slot"] not in sel and in_range(f, lo, hi) for f in fvs):
        raise DeskRefusal(400, "a node group is one channel range, and another fixture sits inside this selection's range")
    slot = next((g for g in range(MAX_GROUPS) if not n.groups[g]["en"]), -1)
    if slot < 0:
        slot = next((g for g in range(MAX_GROUPS) if not group_listed(n, fvs, g)), -1)
    if slot < 0:
        raise DeskRefusal(400, "all 8 group slots are in use: remove one first")
    name = clean_name(b.get("name"), 16) or "G%d" % (slot + 1)
    n.groups[slot].update({"name": name, "start": lo, "end": hi, "en": True})
    return 200, {"ok": True, "group": group_obj(n, fvs, slot)}


def p_groups_remove(n, b):
    g = parse_id(b.get("id"), "g", MAX_GROUPS)
    if g < 0:
        raise DeskRefusal(404, "no such group")
    n.groups[g]["en"] = False
    return 200, {"ok": True}


def p_fx(n, b):
    m = -1
    if b.get("mode") is not None:
        m = FX_NAMES.index(b["mode"]) if b["mode"] in FX_NAMES else -2
        if m == -2:
            raise DeskRefusal(400, "unknown fx mode: the node offers state.fxModes")
    if b.get("spatial") is not None and b["spatial"] != "patch":
        raise DeskRefusal(400, "the node's fx follow the patch order only (spatial: patch)")
    if isinstance(b.get("exclude"), list) and b["exclude"]:
        raise DeskRefusal(400, "fx exclude is not on this node")
    for k in ("bpm", "depth"):
        if b.get(k) is not None and not is_num(b[k]):
            raise DeskRefusal(400, "bpm and depth must be numbers")
    if m >= 0:
        n.fx["mode"] = m
    if is_num(b.get("bpm")):
        n.fx["bpm"] = 20 if b["bpm"] < 20 else 240 if b["bpm"] > 240 else int(b["bpm"] + 0.5)
    if is_num(b.get("depth")):
        n.fx["depth"] = level(b["depth"])
    if b.get("enabled") is not None:
        n.fx["en"] = truthy(b["enabled"])
    elif m >= 0:
        n.fx["en"] = m != 0
    if n.fx["mode"] == 0:
        n.fx["en"] = False
    return 200, {"ok": True, "fx": fx_obj(n)}


def p_chase(n, b):
    if isinstance(b.get("sync"), str) and b["sync"] != "clock":
        raise DeskRefusal(400, "the node's cue runner has no beat clock: sync stays clock")
    for k in ("holdMs", "fadeMs"):
        if b.get(k) is not None and not is_num(b[k]):
            raise DeskRefusal(400, "holdMs and fadeMs must be numbers")
    cue = n.cue

    def saved_steps():
        return sum(1 for st in cue["steps"][:cue["count"]] if st["scene"] in DESK.saved)
    was_empty = saved_steps() == 0
    if isinstance(b.get("sceneIds"), list):
        ids = [s for s in (parse_id(v, "s", SCENE_COUNT) for v in b["sceneIds"]) if s >= 0][:MAX_CUES]
        d0 = cue["steps"][0]["dwell"] if cue["count"] else 1500
        f0 = cue["steps"][0]["fade"] if cue["count"] else 500
        keep = [{"scene": s, "dwell": d0, "fade": f0} for s in ids if s in DESK.saved]
        cue["steps"][:len(keep)] = keep
        cue["count"], cue["index"] = len(keep), 0
    for st in cue["steps"][:cue["count"]]:
        if is_num(b.get("holdMs")):
            st["dwell"] = max(0, min(FADE_MAX_MS, int(b["holdMs"])))
        if is_num(b.get("fadeMs")):
            st["fade"] = clamp_fade(b["fadeMs"])
        if st["fade"] + st["dwell"] < 50:
            st["dwell"] = 50 - st["fade"]
    want = cue["run"] if b.get("enabled") is None else truthy(b["enabled"])
    if saved_steps() == 0:
        want = False
    elif was_empty and want and b.get("enabled") is not True:
        want = False
    start = want and not cue["run"]
    cue["run"] = want
    if start:
        cue["index"] = 0
        n._cue_at = time.time()
        st = cue["steps"][0]
        start_fade(n, n.scenes[st["scene"]], st["fade"])
    return 200, {"ok": True, "chase": chase_obj(n)}


def p_output(n, b):
    if b.get("driver") is not None and b["driver"] != "artnet":
        raise DeskRefusal(400, "this node sends Art-Net; its DMX cable is always driven")
    if isinstance(b.get("serialPort"), str) and b["serialPort"]:
        raise DeskRefusal(400, "no serial output on this node")
    if b.get("refreshHz") is not None and not (is_num(b["refreshHz"]) and b["refreshHz"] == 1000 // 23):
        raise DeskRefusal(400, "the node refreshes at a fixed 43 Hz")
    mode = b.get("mode") if isinstance(b.get("mode"), str) else None
    if mode or isinstance(b.get("targets"), list):
        valid = [t for t in (b.get("targets") or []) if isinstance(t, str) and ipv4_ok(t)]
        if len(valid) > 1:
            raise DeskRefusal(400, "one unicast target on this node")
        peer = valid[0] if valid else ""
        if mode and mode != "unicast":
            peer = ""
        n.art_out_target = peer
    if b.get("enabled") is not None:
        n.art_out = truthy(b["enabled"])
    return 200, {"ok": True, "output": output_obj(n), "serial": None, "error": None}


def p_input(n, b):
    for k in ("artnet", "sacn"):
        if b.get(k) is not None and not truthy(b[k]):
            raise DeskRefusal(400, "the node always listens to Art-Net and sACN")
    if isinstance(b.get("name"), str) and b["name"] != n.name:
        raise DeskRefusal(400, "rename the node in its node settings")
    loss = None
    if b.get("loss") is not None:
        if b["loss"] not in ("release", "hold"):
            raise DeskRefusal(400, "loss is release or hold on this node")
        loss = b["loss"] == "release"
    desk = None
    for u in b.get("universes") if isinstance(b.get("universes"), list) else []:
        if not isinstance(u, dict) or not is_num(u.get("universe")) or u["universe"] != n.uni15():
            raise DeskRefusal(400, "the node listens on its own Art-Net port-address (sACN +1); change it in node settings")
        if u.get("merge") is not None and u["merge"] != "htp":
            raise DeskRefusal(400, "the node merges its sources HTP")
        if isinstance(u.get("desk"), str):
            desk = 1 if u["desk"] == "follow" else 2
    nxt = n.mode
    if b.get("enabled") is not None:
        nxt = (2 if n.mode == 0 else n.mode) if truthy(b["enabled"]) else 0
    if desk and nxt != 0:
        nxt = desk
    n.mode = nxt
    if loss is not None:
        n.artnet_fallback = loss
    return 200, input_obj(n)


def p_input_release(n, b):
    DESK.hold_released = True
    return 200, {"ok": True}


DESK_POST = {
    "/api/master": p_master, "/api/fixture": p_fixture, "/api/fixtures/all": p_fixtures_all, "/api/raw": p_raw,
    "/api/fixtures/add": p_fixtures_add, "/api/fixtures/remove": p_fixtures_remove,
    "/api/fixtures/move": p_fixtures_move, "/api/fixtures/readdress": p_fixtures_readdress,
    "/api/fixtures/identify": p_fixtures_identify, "/api/scenes/save": p_scenes_save,
    "/api/scenes/update": p_scenes_update, "/api/scenes/recall": p_scenes_recall,
    "/api/scenes/remove": p_scenes_remove, "/api/groups/add": p_groups_add, "/api/groups/remove": p_groups_remove,
    "/api/fx": p_fx, "/api/chase": p_chase, "/api/output": p_output, "/api/input": p_input,
    "/api/input/release": p_input_release,
}
assert {p for p, (m, c) in DESK_API.items() if c == "a" and "POST" in m} == set(DESK_POST), "class-a POSTs"


class BadArg(Exception):
    def __init__(self, arg, why, lo, hi):
        super().__init__(arg)
        self.body = json.dumps({"error": why, "arg": arg, "min": lo, "max": hi}, separators=(",", ":"))


def num(q, k, lo, hi, required=False):
    """vizzz::parseUintArg: 1-10 decimal digits inside [lo, hi], else 400."""
    if k not in q:
        if required:
            raise BadArg(k, "missing", lo, hi)
        return None
    s = q[k][0]
    if not re.fullmatch(r"[0-9]{1,10}", s) or not lo <= int(s) <= hi:
        raise BadArg(k, "bad number", lo, hi)
    return int(s)


def wpa_ok(s):
    return 8 <= len(s) <= 63 and all(0x20 <= ord(c) <= 0x7e for c in s)


def ipv4_ok(s):
    parts = s.split(".")
    return len(parts) == 4 and all(re.fullmatch(r"[0-9]{1,3}", p) and int(p) <= 255 for p in parts)


def stream_delta_runs(prev, nxt, gap=None):
    """dmxstream.js deltaRuns (and vizzz::desk::stream::formatDelta): the runs of changed slots,
    runs closer than the gap joined."""
    gap = STREAM["kGap"] if gap is None else gap
    runs, start, last = [], -1, -1
    for i in range(len(nxt)):
        if prev is not None and prev[i] == nxt[i]:
            continue
        if start >= 0 and i - last <= gap:
            last = i
            continue
        if start >= 0:
            runs.append((start, last))
        start = last = i
    if start >= 0:
        runs.append((start, last))
    return [[0, a, list(nxt[a:b + 1])] for a, b in runs]


def stream_event(seq, t, key, d, meta):
    """`event: frame` + `data: {s,t,k,d,m?}` + blank line, the key order of the firmware."""
    body = {"s": seq, "t": t, "k": 1 if key else 0, "d": d}
    if meta is not None:
        body["m"] = meta
    return ("event: frame\ndata: %s\n\n" % json.dumps(body, separators=(",", ":"))).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "vizzz.di-sim"

    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", ctype="text/plain", headers=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _desk_file(self, p):
        mime, gz, etag = DESK_UI[p]
        inm = self.headers.get("If-None-Match")
        not_modified = bool(inm) and etag_matches(inm, etag)
        self.send_response(304 if not_modified else 200)
        if not_modified:
            self.send_header("Content-Length", "0")
        else:
            self.send_header("Content-Type", mime)
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(gz)))
        self.send_header("ETag", '"%s"' % etag)
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if not not_modified:
            self.wfile.write(gz)

    def _json(self, obj):
        # Firmware sendJSON(): ACAO only on GetShared reads (audit 2-M7).
        acao = [("Access-Control-Allow-Origin", "*")] if ROUTES.get(self._p) == "GetShared" else []
        self._send(200, json.dumps(obj), "application/json", acao)

    def do_OPTIONS(self):
        p = urlparse(self.path).path
        kind = ROUTES.get(p)
        if kind is None:
            return self._send(404, "Not found")
        if kind == "GetShared":
            return self._send(204, headers=[("Access-Control-Allow-Origin", "*"),
                                            ("Access-Control-Allow-Methods", "GET"),
                                            ("Access-Control-Max-Age", "600")])
        return self._send(403, '{"error":"no cross-origin access"}', "application/json")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n > 0 else b""
        self._body = body
        extra = {}
        if body and (self.headers.get("Content-Type") or "").startswith("application/x-www-form-urlencoded"):
            extra = parse_qs(body.decode("utf-8", "replace"), keep_blank_values=True)
        self._route("POST", extra)

    def do_GET(self):
        self._route("GET", {})

    def _route(self, method, extra):
        u = urlparse(self.path)
        p, q = u.path, parse_qs(u.query, keep_blank_values=True)
        q.update(extra)
        self._p = p
        if p == "/ws":
            return self._websocket()
        kind = ROUTES.get(p)
        if kind is None:
            return self._send(404, "Not found")
        if kind == "DeskApi":
            return self._desk(method, p)
        # Firmware route() gate (vizzz::gateRequest, audit 2-H2).
        allowed = ("GET",) if kind in ("GetLocal", "GetShared", "Static") else \
                  ("GET", "POST") if kind == "Operator" else ("POST",)
        if method not in allowed:
            return self._send(405, json.dumps({"error": "method not allowed", "allow": ROUTE_METHODS[kind]}),
                              "application/json", [("Allow", ROUTE_METHODS[kind])])
        if kind == "Console" and self.headers.get("X-Vizzz-Console") != "1":
            return self._send(403, '{"error":"console route: send POST with X-Vizzz-Console: 1"}',
                              "application/json")
        try:
            return self._handle(p, q)
        except BadArg as e:
            return self._send(400, e.body, "application/json")

    def _stream(self):
        """GET /api/dmx/stream, the firmware's apiDmxStream + streamTick for one client: the cap
        answers 503 {"error":"stream full"}; the frame is outVals; an event only when the frame or the
        master changed, at most every kPeriodMs, a key frame every kKeepaliveMs."""
        with DESK.sse_lock:
            if DESK.sse >= STREAM["kMaxClients"]:
                full = True
            else:
                full = False
                DESK.sse += 1
        if full:
            return self._desk_json(503, {"error": "stream full"})
        DESK.replies += 1
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self.wfile.write(("retry: %d\n" % STREAM["kRetryMs"]).encode())
            self.wfile.flush()
            period, keep = STREAM["kPeriodMs"] / 1000.0, STREAM["kKeepaliveMs"] / 1000.0
            sent, sent_meta, seq, last_send = None, None, 0, 0.0
            while True:
                with NODE.lock:
                    frame, meta = list(NODE.out), {"master": NODE.master, "blackout": False,
                                                   "looks": [], "cues": None}
                now = time.time()
                changed = sent is None or frame != sent
                meta_changed = sent_meta is None or meta != sent_meta
                if sent is None or now - last_send >= keep:
                    key = True
                elif changed or meta_changed:
                    key = False
                else:
                    key = None
                if key is not None:
                    seq += 1
                    t = int((now - NODE.boot) * 1000)
                    if key:
                        d = [[0, 0, frame]]
                    else:
                        d = stream_delta_runs(sent, frame)
                    self.wfile.write(stream_event(seq, t, key, d, meta if (key or meta_changed) else None))
                    self.wfile.flush()
                    sent, sent_meta, last_send = frame, meta, now
                time.sleep(period)
                if select.select([self.connection], [], [], 0)[0] and not self.connection.recv(1, socket.MSG_PEEK):
                    break   # the reader closed: free the slot at once
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            pass
        finally:
            with DESK.sse_lock:
                DESK.sse -= 1
            self.close_connection = True

    def _desk_json(self, code, obj):
        DESK.replies += 1
        self._send(code, json.dumps(obj, ensure_ascii=False, separators=(",", ":")), "application/json")

    def _desk(self, method, p):
        """Firmware route() for RouteKind::DeskApi: vizzz::desk::gateDesk, then the body
        rules of deskParse(), then the handler (main.cpp api*)."""
        methods = DESK_API[p][0]
        if method not in methods:
            allow = ", ".join(methods)
            return self._send(405, json.dumps({"error": "method not allowed", "allow": allow}),
                              "application/json", [("Allow", allow)])
        if p == "/api/dmx/stream":
            return self._stream()
        b = {}
        if method == "POST":
            ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            if ctype != "application/json":
                return self._send(415, '{"error":"send the body as Content-Type: application/json"}',
                                  "application/json")
            raw = getattr(self, "_body", b"")
            if len(raw) > MAX_BODY:
                return self._desk_json(413, {"error": "body over 2048 bytes"})
            if not raw:
                return self._desk_json(400, {"error": "invalid JSON: empty body"})
            try:
                b = json.loads(raw.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                return self._desk_json(400, {"error": "invalid JSON"})
            if not isinstance(b, dict):
                return self._desk_json(400, {"error": "the body must be a JSON object"})
        try:
            with NODE.lock:
                code, obj = desk_api(NODE, method, p, b)
        except DeskRefusal as e:
            code, obj = e.code, {"error": e.msg}
        return self._desk_json(code, obj)

    def _handle(self, p, q):
        if p in REDIRECTS:   # firmware sendDeskRedirect()
            return self._send(301, headers=[("Location", REDIRECTS[p])])
        if p in DESK_UI:     # firmware sendDeskFile()
            return self._desk_file(p)

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
            i = num(q, "i", 0, PAGE_COUNT - 1) or 0
            s = i * PAGE_SIZE
            return self._json({"i": i, "web": n.web[s:s + PAGE_SIZE],
                               "out": n.out[s:s + PAGE_SIZE]})
        if p == "/peers":
            return self._json({"peers": n.peers, "count": len(n.peers)})
        if p == "/discover":   # firmware fields only, no side effect (2-L5)
            return self._json({"name": n.name, "ip": "192.168.4.1", "ap_ip": "192.168.4.1",
                               "ap_ssid": n.ap_ssid, "mdns": n.name.lower() + ".local",
                               "mac": n.mac, "product": "vizzz.di"})
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
                ch = num(q, "ch", 1, MAX_CH, True)
                v = num(q, "v", 0, 255, True)
                n.web[ch - 1] = v
            elif p == "/blackout" or p == "/net/blackout":
                n.web = [0] * MAX_CH; n.fade = None
                n.cue["run"] = False; n.fx["en"] = False
            elif p == "/panic":   # firmware loop() pendingPanic
                n.web = [0] * MAX_CH; n.fade = None; n.mode = 0; n.master = 255
                n.cue["run"] = False; n.fx["en"] = False; n.color["en"] = False
            elif p == "/full" or p == "/net/full":
                n.web = [255] * MAX_CH; n.fade = None
            elif p in ("/master", "/net/master"):
                v = num(q, "v", 0, 255)
                if v is not None:
                    n.master = v
            elif p == "/mode/set":
                n.mode = num(q, "m", 0, 2, True)
            elif p == "/mode/fallback":
                n.artnet_fallback = bool(num(q, "en", 0, 1, True))
            elif p == "/netmode/set":
                n.net_mode = num(q, "m", 0, 2, True)
            elif p == "/web/set":
                n.web_enabled = bool(num(q, "en", 0, 1, True))
            elif p == "/safety/set":
                n.burn_safe = bool(num(q, "burn", 0, 1, True))
            elif p == "/artout/set":
                en = num(q, "en", 0, 1)
                if en is not None:
                    n.art_out = bool(en)
            elif p == "/artout/peer":
                ip = q.get("ip", [""])[0].strip()
                if ip and not ipv4_ok(ip):
                    return self._send(400, "ip must be a dotted quad")
                n.art_out_target = ip
            elif p == "/artnet/set":
                vals = {k: num(q, k, 0, hi) for k, hi in (("net", 127), ("subnet", 15), ("uni", 15))}
                for k, v in vals.items():
                    if v is not None:
                        n.art[k] = v
            elif p == "/scene/save":
                slot = num(q, "n", 0, SCENE_COUNT - 1, True)
                n.scenes[slot] = list(n.web)
                DESK.saved.add(slot)                 # firmware saveScene() updates the cache
            elif p in ("/scene/recall", "/net/scene/recall"):
                s = n.scenes[num(q, "n", 0, SCENE_COUNT - 1, True)]
                f = num(q, "fade", 0, FADE_MAX_MS) or 0
                if f == 0:
                    n.web = list(s); n.fade = None
                else:
                    n.fade = (time.time(), f, list(n.web), list(s))
            elif p in ("/group/set", "/fixture/set"):
                fix = p == "/fixture/set"
                idx = num(q, "i" if fix else "g", 0, (MAX_FIXTURES if fix else MAX_GROUPS) - 1, True)
                start, end = num(q, "start", 1, MAX_CH), num(q, "end", 1, MAX_CH)
                if start is not None and end is not None and end < start:
                    raise BadArg("end", "end before start", start, MAX_CH)
                en = num(q, "en", 0, 1)
                if fix:
                    x, y, count = num(q, "x", 0, 255), num(q, "y", 0, 255), num(q, "count", 1, MAX_FIXTURES)
                    g = n.fixtures[idx]
                    if x is not None: g["x"] = x
                    if y is not None: g["y"] = y
                    if count is not None: n.fixture_count = count
                else:
                    g = n.groups[idx]
                if "name" in q: g["name"] = q["name"][0]
                if start is not None: g["start"] = start
                if end is not None: g["end"] = end
                g["end"] = max(g["end"], g["start"])   # 2-L6
                if en is not None: g["en"] = bool(en)
            elif p == "/group/apply":
                g = n.groups[num(q, "g", 0, MAX_GROUPS - 1, True)]
                v = num(q, "v", 0, 255, True)
                for c in range(g["start"] - 1, min(g["end"], MAX_CH)):
                    n.web[c] = v
            elif p == "/cue/count":
                n.cue["count"] = num(q, "c", 1, MAX_CUES, True)
            elif p == "/cue/set":
                st = n.cue["steps"][num(q, "i", 0, MAX_CUES - 1, True)]
                sc, dw, fa = (num(q, "scene", 0, SCENE_COUNT - 1), num(q, "dwell", 0, FADE_MAX_MS),
                              num(q, "fade", 0, FADE_MAX_MS))
                if sc is not None: st["scene"] = sc
                if dw is not None: st["dwell"] = dw
                if fa is not None: st["fade"] = fa
            elif p == "/cue/run":
                n.cue["run"] = bool(num(q, "en", 0, 1, True)); n._cue_at = time.time()
            elif p == "/cue/next":
                n.cue["index"] = (n.cue["index"] + 1) % n.cue["count"]
            elif p == "/fx/set":
                en, bpm, depth = num(q, "en", 0, 1), num(q, "bpm", 20, 240), num(q, "depth", 0, 255)
                if "mode" in q:
                    m = q["mode"][0]
                    if m in FX_NAMES: n.fx["mode"] = FX_NAMES.index(m)
                    elif re.fullmatch(r"[0-9]", m): n.fx["mode"] = int(m)
                    else: raise BadArg("mode", "unknown fx mode", 0, 9)
                if en is not None: n.fx["en"] = bool(en)
                if bpm is not None: n.fx["bpm"] = bpm
                if depth is not None: n.fx["depth"] = depth
            elif p == "/fx/tap":
                now = time.time(); n.tap = [t for t in n.tap if now - t < 3] + [now]
                if len(n.tap) >= 2:
                    gaps = [b - a for a, b in zip(n.tap, n.tap[1:])]
                    n.fx["bpm"] = max(20, min(240, int(60.0 / (sum(gaps) / len(gaps)))))
            elif p == "/color/set":
                vals = {k: num(q, k, 0, 255) for k in "rgb"}
                en = num(q, "en", 0, 1)
                if en is not None: n.color["en"] = bool(en)
                for k, v in vals.items():
                    if v is not None: n.color[k] = v
            elif p == "/node/set":
                if "ap_pass" in q and not wpa_ok(q["ap_pass"][0]):
                    return self._send(400, "ap_pass must be 8-63 printable ASCII characters")
                if "ap_ssid" in q and not q["ap_ssid"][0]:
                    return self._send(400, "ap_ssid must not be empty")
                if "name" in q: n.name = q["name"][0][:31]
                if "ap_ssid" in q: n.ap_ssid = q["ap_ssid"][0][:31]
            elif p == "/wifi/set":
                ssid = q.get("ssid", [None])[0]
                if ssid is None:
                    raise BadArg("ssid", "missing", 1, 32)
                if not 1 <= len(ssid.encode()) <= 32:
                    return self._send(400, '{"error":"ssid must be 1-32 bytes","arg":"ssid"}', "application/json")
                pw = q.get("pass", [""])[0]
                if pw and not wpa_ok(pw):
                    return self._send(400, '{"error":"pass must be empty or 8-63 printable ASCII","arg":"pass"}',
                                      "application/json")
            elif p in ("/wifi/forget", "/reboot", "/factory-reset", "/peer/cmd", "/announce"):
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
    threading.Thread(target=ticker, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print("vizzz.di simulator on http://%s:%d  (%d gz bytes of desk UI)"
          % (a.host, a.port, sum(len(v[1]) for v in DESK_UI.values())))
    srv.serve_forever()
