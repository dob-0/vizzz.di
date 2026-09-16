#!/usr/bin/env python3
"""Run the vizzz.di browser console on this machine, with no ESP32.

Serves the real APP_HTML extracted from src/main.cpp against an in-memory model
of the firmware: web layer, master dimmer, safety ceiling and slew, scenes, FX,
colour, cues, groups and a fake peer table. DMX output is computed the same way
the firmware computes it, so /monitor and /page show believable numbers.

    python3 tools/simulate_node.py --port 8088

This is a UI and API harness, not an emulator. It does not run the C++.
"""
import argparse, base64, hashlib, json, os, re, struct, threading, time
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
        if self.mode == 0:
            base = list(self.web)
        elif self.mode == 1:
            base = list(self.net_layer) if not self.artnet_fallback else list(self.web)
        else:
            base = [max(a, b) for a, b in zip(self.web, self.net_layer)]
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
            "artnet_active": False, "sacn_active": False,
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
            "protocols": ["artnet-in", "artnet-out", "sacn-in", "osc-in", "http", "ws"],
            "simulated": True,
        }


NODE = Node()


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
        NODE.tick()
        time.sleep(0.023)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    APP_HTML = extract_app_html()
    threading.Thread(target=ticker, daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print("vizzz.di simulator on http://%s:%d  (%d bytes of console UI)"
          % (a.host, a.port, len(APP_HTML)))
    srv.serve_forever()
