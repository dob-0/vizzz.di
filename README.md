# vizzz.di

ESP32 WiFi -> DMX512 firmware node. It outputs DMX512 through a MAX485
transceiver, accepts Art-Net and sACN input, broadcasts Art-Net for node sync,
and exposes a browser console in di.iiii's light desk look plus a machine-readable node manifest.

## Features

| Feature | Detail |
|---|---|
| **3 output modes** | `WEB_ONLY`, `ARTNET_ONLY`, `MERGE_HTP` |
| **Console UI** | Routes: `/`, `/control`, `/patch`, `/scenes`, `/network`, `/system`, `/vj` (`/performance` alias) |
| **DMX output** | 512 channels on `DMX_NUM_1`, GPIO25 TX, GPIO21 DIR |
| **Art-Net IN/OUT** | Configurable Net/Subnet/Universe, hold-last on loss, optional broadcast output |
| **sACN IN** | E1.31 listener on port `5568`, same universe as Art-Net |
| **USB DMX IN** | ENTTEC DMX USB PRO widget protocol on the USB cable (UART0, 250000 baud): di.iiii's `enttec` driver drives the node directly; HTP with Art-Net/sACN (see [USB](#usb-enttec-dmx-usb-pro-widget)) |
| **Peer / fleet control** | Peer discovery plus network-wide blackout, full, master, and scene recall |
| **VJ deck** | Mobile-first scene launcher, smoother FX pads, color engine, cue runner, group mixer, and fleet controls |
| **Scenes** | 8 slots, save/recall with fade |
| **Master dimmer** | 0-255 applied on the output path |
| **Burn-safe mode** | Optional stress profile with reduced output ceiling/slew (`/safety/set?burn=1`) |
| **WebSocket** | Live status push every ~400 ms at `ws://192.168.4.1/ws` |
| **Node manifest** | `GET /node/manifest` and `GET /manifest.json` |
| **WiFi** | AP always recoverable, optional STA client mode |
| **Persistent config** | ESP32 Preferences NVS |

## Hardware

Board: **ESP32 DevKit**.

| Signal | GPIO | Notes |
|---|---|---|
| DMX TX -> MAX485 DI | **25** | UART2 / `DMX_NUM_1` |
| DMX DIR -> MAX485 DE+RE | **21** | DE and RE bridged |
| MAX485 A/B | - | XLR pin 3/2, shield to pin 1 |

Do not use `DMX_NUM_0`; UART0 shares the USB serial pins and can silently break
DMX output.

## Quick Start

1. Build and flash with PlatformIO.
2. Connect to AP `vizzz.di` or the generated `vizzz.di_XXXXXX` AP.
3. Open `http://192.168.4.1`.
4. Use `/vj` for the VJ controller.
5. Use `/system` or `/node/manifest` to inspect the firmware-node contract.

Default AP password: whatever you set in `src/secrets.h`. That file is
gitignored so it never reaches the repository — copy the template and edit it:

```bash
cp src/secrets.example.h src/secrets.h
```

Without it the firmware still builds, falls back to the placeholder
`changeme123`, and says so on the serial console at boot. WPA2 needs at least
eight characters.

## Build

PlatformIO lives in a project-local virtualenv at `.venv-pio/`. The project sets
`name = vizzz.di` and `core_dir = .platformio-core` so PlatformIO writes
project-local cache/lock files instead of the read-only user PlatformIO home.

```bash
# One-time setup
python3 -m venv .venv-pio && .venv-pio/bin/pip install platformio

# Build firmware
.venv-pio/bin/pio run -e esp32dev

# Upload to ESP32
sg dialout -c ".venv-pio/bin/pio run -e esp32dev --target upload"

# Native unit tests
.venv-pio/bin/pio test -e native
```

The console follows di.iiii's light desk tokens (serverXR/src/lighting/ui/style.css): `--bg #0c0c0d`, `--bar #060607`, `--pane #121213`, `--pane2 #1a1a1c`, `--field #060607`, `--cell #161617`, `--line #26262a`, `--text #d8dadb`, `--muted #878d92`, `--accent #35d2dd` (live/selected only), `--danger #e0524a` (Blackout, Panic); mono 11px for data, system sans for UI, 1 px lines, square corners. Top bar: vizzz.di mark, page tabs, Blackout; below it a status row of chips (mode, network, Art-Net, sACN, out, web, STA, peers). Pictures: `docs/console/`.

To work on the console UI without an ESP32, `tools/simulate_node.py` serves the
real `APP_HTML` from `src/main.cpp` against an in-memory model of the firmware:

```bash
python3 tools/simulate_node.py --port 8088   # then open http://127.0.0.1:8088
```

## Device Onboarding

For multiple ESP nodes, handle one physical board at a time and track it by MAC.
`onboard_device.py` can read the connected ESP MAC, optionally erase all flash,
upload the current firmware, then configure the node name and Art-Net/sACN
universe once the node is reachable over HTTP.

```bash
# Clean erase + flash the ESP on /dev/ttyUSB0
python3 onboard_device.py --erase

# After connecting to its AP or STA address, assign a name and universe
python3 onboard_device.py --skip-serial --skip-upload --host 192.168.4.1 --name vizzz.di-u2 --universe 2 --mode artnet

# Configure an existing reachable node and set channels 1-4 full as a test
python3 onboard_device.py --skip-serial --skip-upload --host 192.168.88.127 --universe 18 --mode web --test 4
```

`--universe` is the absolute 15-bit Art-Net universe number. The helper splits it
into the firmware's `net/subnet/uni` values automatically.

## Multi-Node Sync

One device can act as a controller node and broadcast the final output as
Art-Net to other nodes on the same network.

- Controller: enable Art-Net OUT in the console or POST `/artout/set?en=1` with `X-Vizzz-Console: 1`
- Receiver nodes: set mode to `ARTNET_ONLY` and use the same universe
- Broadcast target: STA subnet broadcast when joined to a router, otherwise
  `192.168.4.255:6454`
- Nodes advertise themselves with UDP beacons on port `47777`; `GET /peers`
  returns the live peer table.
- The performance page includes fleet controls backed by `/net/blackout`,
  `/net/full`, `/net/master`, and `/net/scene/recall`.

## USB (ENTTEC DMX USB PRO widget)

The node speaks the widget protocol of the ENTTEC DMX USB PRO on its USB serial
port, so a host that drives a DMX USB PRO drives the node over the same USB cable
used to flash it. Method: the public **"ENTTEC DMX USB PRO API"** document
(widget protocol: `0x7E, label, length LSB, length MSB, data, 0xE7`).

| Label | Name (API) | What the node does |
|---|---|---|
| 3 | Get Widget Parameters | Replies: firmware `1.0` (vizzz widget layer, not an ENTTEC number), the node's real DMX break and MAB (from its esp_dmx driver, 10.67 µs units), rate 40 pkt/s (the API ceiling; the node refreshes every 23 ms), then the requested user-config bytes as zeros (≤ 508) |
| 4 | Set Widget Parameters | Accepted; nothing is stored or applied — the node's DMX timing is its own driver's |
| 6 | Output Only Send DMX Packet | Start code `0x00` only; channels 1..N go into the USB layer; channels above N keep their value (short frames, ≥ 24 ch per the API, are fine) |
| 10 | Get Widget Serial Number | Replies 4 BCD bytes, least significant digit pair first: the low 24 bits of the Wi-Fi MAC as 8 decimal digits (stable per board) |
| other | — | Ignored. A frame longer than 600 data bytes or with a wrong end byte is dropped and the bytes after its start are scanned again, so noise never costs later frames; a frame stalled for 100 ms is dropped the same way |

**Cable and port.** The DevKit's own USB socket (CH340/CP210x on UART0). On Linux
the port is `/dev/ttyUSB0` (or `/dev/serial/by-id/…`), on macOS `/dev/cu.usbserial*`,
on Windows `COMn`.

**Baud: 250000, set by the host.** The widget protocol has no baud negotiation, so
both ends must agree. The node runs `Serial` at `USB_WIDGET_BAUD = 250000`
(`src/main.cpp`), the baud di.iiii's `serverXR/src/lighting/enttec.js` sets first
(`stty … 250000` on the open descriptor; `mode COMn: BAUD=250000` on Windows). It
falls back to 115200 only if the port refuses 250000 — then the node hears garbage
and `usb_active` stays false. The CH340 on aylmo accepts 250000 (measured with
`stty` 2026-10-15). Any other host must set 250000 8N1, raw, no flow control.

**What di.iiii must set.** In the light desk's output settings: `output.driver`
= `'enttec'` and `output.serialPort` = the node's port (e.g. `/dev/ttyUSB0`).
di sends label 6 every tick (40 Hz) on universe 0; the node takes it regardless of
its Art-Net universe (a USB cable is one universe). Only one program may hold the
port: close `pio device monitor` before the desk opens it.

**Merge.** USB is a third network-class source next to Art-Net and sACN; the three
merge HTP among themselves, then the output mode decides:

| Mode | With USB frames arriving | When USB stops (2.5 s) |
|---|---|---|
| `WEB_ONLY` | ignored — web/VJ layer only | — |
| `ARTNET_ONLY` | network layer (HTP of USB, Art-Net, sACN) alone | holds the last output, or falls back to web if `artnet_fallback_to_web` |
| `MERGE_HTP` (default) | max(web, network) per channel | web layer only |

Master, safe ceiling, FX and slew apply after the merge, as for Art-Net. Panic
switches to `WEB_ONLY`, so it silences USB too. The 2.5 s loss window is
E1.31-2018 §6.7.1's: the widget API has no timeout (a real widget holds its last
frame for ever), but di transmits continuously, so silence means the desk is gone,
and a hold-for-ever source would latch the rig on in `MERGE_HTP` after the desk
closes. `ARTNET_ONLY` still offers hold-last through its existing fallback rule.

**The log shares the UART.** The text boot/serial log goes out on the same UART0
TX at 250000. A host reading the port sees log text, and binary widget replies
only after it sent label 3 or 10. di never reads the port. The ROM bootloader's
first lines (before the firmware starts) are at 115200 and look garbled at 250000.

`/status` adds `usb_active`, `usb_frames` (label-6 frames applied since boot) and
`usb_baud`; the manifest has a `usb` protocol entry with the same facts.
Proof on hardware: `tools/usb_probe.js` (see HANDOVER "USB flash-and-verify");
without hardware: `tools/widget_pty_harness.cpp` runs the firmware's parser on a
pseudo-terminal that di's driver opens.

## HTTP API

One route table, `vizzz::kRoutes` in `include/vizzz_core.h`, decides every
route's methods, header and CORS; `setupWeb()` registers only from it and the
manifest is generated from it. `tools/check_routes.py` fails if this README,
`main.cpp` or di.iiii's client disagree with it (audit 2026-10-15 2-H2, 2-M4, 2-M7).

- **Operator routes** (method GET/POST, no header): the show controls di.iiii's
  DMX Out and phones send as plain `no-cors` GETs. Any web page in the
  operator's browser can also fire these. That is a known, accepted limit until
  the owner decides on a login (see `docs/AUDIT_2026_10_15.md` 2-H2).
- **Console routes** (method POST, header `X-Vizzz-Console: 1` required):
  everything that changes the network, identity, patch or stored state. GET
  answers `405` with `Allow: POST`; POST without the header answers `403`; a
  CORS preflight answers `403` without `Access-Control-Allow-Origin`, so a
  cross-site page cannot send them (OWASP CSRF Prevention Cheat Sheet,
  "custom request headers"). The built-in console sends the header.
- **Reads** (GET): `CORS yes` = `Access-Control-Allow-Origin: *`. No other
  route, and no error answer, carries it.
- Numbers are decimal digits inside the range shown; anything else is `400`
  with `{"error","arg","min","max"}`. Commands answer `204` (or `202 queued`,
  `503 busy`).

```bash
curl -X POST -H 'X-Vizzz-Console: 1' 'http://192.168.4.1/mode/set?m=2'      # console route
curl -X POST -H 'X-Vizzz-Console: 1' --data-urlencode ssid=Net --data-urlencode pass=secret123 http://192.168.4.1/wifi/set
curl 'http://192.168.4.1/set?ch=1&v=255'                                     # operator route
```

| Method | Header | CORS | Path | Arguments | Description |
|---|---|---|---|---|---|
| GET/POST | - | - | `/set` | `ch=1-512&v=0-255` | Set one web-layer channel (di.iiii DMX Out) |
| GET/POST | - | - | `/blackout` | - | Web layer to 0, cue list and FX stopped (di.iiii DMX Out) |
| GET/POST | - | - | `/full` | - | Set all web-layer channels to 255 |
| GET/POST | - | - | `/master` | `v=0-255` | Set master dimmer (di.iiii DMX Out) |
| GET/POST | - | - | `/scene/recall` | `n=0-7&fade=0-600000` | Recall scene with fade (ms) |
| GET/POST | - | - | `/cue/run` | `en=0\|1` | Start/stop cue runner |
| GET/POST | - | - | `/cue/next` | - | Advance cue runner one step |
| GET/POST | - | - | `/fx/tap` | - | BPM tap tempo input |
| POST | yes | - | `/panic` | - | Kill everything: master 0, blackout, FX and cues off, WEB_ONLY in RAM |
| POST | yes | - | `/mode/set` | `m=0\|1\|2` | WEB / ARTNET / HTP |
| POST | yes | - | `/mode/fallback` | `en=0\|1` | In ARTNET mode, fall back to web layer when network input is inactive |
| POST | yes | - | `/netmode/set` | `m=0\|1\|2` | AP_STA / STA_ONLY / AP_ONLY, then reboot |
| POST | yes | - | `/web/set` | `en=0\|1` | Web console on/off, then reboot |
| POST | yes | - | `/artout/set` | `en=0\|1` | Enable Art-Net OUT |
| POST | yes | - | `/artout/peer` | `ip=a.b.c.d (empty = broadcast)` | Use one peer as Art-Net OUT unicast target |
| POST | yes | - | `/artnet/set` | `net=0-127&subnet=0-15&uni=0-15` | Configure Art-Net port-address (sACN = port-address + 1) |
| POST | yes | - | `/safety/set` | `burn=0\|1` | Toggle burn-safe mode |
| POST | yes | - | `/scene/save` | `n=0-7` | Save current web layer |
| POST | yes | - | `/fixture/set` | `i=0-11&name&start=1-512&end=1-512&x&y=0-255&en&count=1-12` | Configure one fixture |
| POST | yes | - | `/group/set` | `g=0-7&name=X&start=1-512&end=1-512&en=0\|1` | Configure one group |
| POST | yes | - | `/group/apply` | `g=0-7&v=0-255` | Apply value to one group range |
| POST | yes | - | `/cue/count` | `c=1-16` | Set active cue step count |
| POST | yes | - | `/cue/set` | `i=0-15&scene=0-7&dwell=ms&fade=ms` | Configure one cue step |
| POST | yes | - | `/fx/set` | `mode=none\|strobe\|...\|0-9&en=0\|1&bpm=20-240&depth=0-255` | Configure FX engine |
| POST | yes | - | `/color/set` | `en=0\|1&r=0-255&g=0-255&b=0-255` | Configure color wash |
| POST | yes | - | `/net/blackout` | - | Blackout this node and all discovered peers |
| POST | yes | - | `/net/full` | - | Full on this node and all discovered peers |
| POST | yes | - | `/net/master` | `v=0-255` | Set master on this node and all discovered peers |
| POST | yes | - | `/net/scene/recall` | `n=0-7&fade=ms` | Recall scene on this node and all discovered peers |
| POST | yes | - | `/peer/cmd` | `ip=X&path=/blackout\|/full\|/master?v=\|/scene/recall?n=` | Forward an allowed command to one known peer |
| POST | yes | - | `/announce` | - | Send one discovery beacon now (else every 30 s) |
| POST | yes | - | `/wifi/set` | `ssid=1-32 B&pass=empty or 8-63 (form body)` | Connect STA |
| POST | yes | - | `/wifi/forget` | - | Clear STA credentials |
| POST | yes | - | `/node/set` | `name=X&ap_ssid=Y&ap_pass=8-63 (form body)` | Update node identity |
| POST | yes | - | `/reboot` | - | Reboot |
| POST | yes | - | `/factory-reset` | - | Erase NVS and reboot with defaults |
| GET | - | yes | `/status` | - | Live status JSON (di.iiii reads it cross-origin) |
| GET | - | yes | `/page` | `i=0-15` | 32-channel page snapshot |
| GET | - | yes | `/monitor` | - | First 64 output channels |
| GET | - | yes | `/safety/status` | - | Burn-safe/effective safety parameters |
| GET | - | yes | `/discover` | - | Node identity (no side effect) |
| GET | - | yes | `/peers` | - | Live discovered peer table |
| GET | - | yes | `/groups` | - | Group table |
| GET | - | yes | `/fixtures` | - | Fixture table |
| GET | - | yes | `/cue/status` | - | Cue list state |
| GET | - | yes | `/fx/status` | - | FX engine state |
| GET | - | yes | `/node/manifest` | - | Firmware node manifest (routes generated from the route table) |
| GET | - | yes | `/manifest.json` | - | Same firmware node manifest |
| GET | - | - | `/wifi/scan` | - | JSON SSID scan (same origin only) |
| GET | - | - | `/` `/control` `/patch` `/scenes` `/network` `/system` `/performance` `/vj` | - | Console pages |
| WS | - | - | `/ws` | - | Status push every ~400 ms; refuses a foreign `Origin` |

## Node Manifest

The product name is `vizzz.di`. The manifest schema remains
`vizzz.di.node.manifest.v1` for compatibility. It exposes identity, firmware
tag, network state, hardware pins, DMX constraints, supported protocols, API
routes, and the source-control policy:

```json
{
  "schema": "vizzz.di.node.manifest.v1",
  "kind": "firmware-node",
  "product": "vizzz.di"
}
```

## AI And Git Workflow

Future AI/code changes should follow this flow:

1. Inspect `CURRENT.md` and `AGENTS.md`.
2. Keep firmware edits scoped; never touch `lib/esp_dmx/`.
3. Run:
   - `/home/nnn/.platformio/penv/bin/pio test -e native`
   - `/home/nnn/.platformio/penv/bin/pio run -e esp32dev`
4. Commit verified changes with a clear message.
5. Push `main` to `origin/main` only when validation passes and the worktree
   contains no unrelated edits.

## Crash Test Tools

Software stress tools live in `tools/`.

```bash
# HTTP stress/fuzz (safe defaults: burn-safe on, dangerous routes off)
python3 tools/crash_http.py --host 192.168.4.1 --seconds 300 --workers 10

# UDP parser flood (Art-Net/sACN/OSC + malformed packets)
python3 tools/crash_udp.py --host 192.168.4.1 --seconds 180 --pps 300
```

Recommended sequence:

1. Run software-only stress with no fixture bus connected.
2. Enable hardware and keep burn-safe mode on.
3. Increase duration/load in stages and watch thermals/fixture behavior.

## Dependencies

- AsyncTCP
- ESPAsyncWebServer
- Direct ArtDMX parser over Arduino WiFiUDP
- esp_dmx, vendored in `lib/esp_dmx/`
- Arduino WiFi / WiFiUDP / Preferences
