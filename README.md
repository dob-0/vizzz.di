# vizzz.di

ESP32 WiFi -> DMX512 firmware node. It outputs DMX512 through a MAX485
transceiver, accepts Art-Net and sACN input, broadcasts Art-Net for node sync,
and serves di.iiii's own light desk UI from flash, plus a machine-readable node manifest.

## Features

| Feature | Detail |
|---|---|
| **3 output modes** | `WEB_ONLY`, `ARTNET_ONLY`, `MERGE_HTP` |
| **Desk UI** | di.iiii's desk files from flash at `/` (see [Desk UI in flash](#desk-ui-in-flash-build-step-v1)); the old console pages 301 to it |
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

To work on the UI side without an ESP32, `tools/simulate_node.py` serves the same desk files
the same way the firmware does (gzip, ETag, 304, the 301s) against an in-memory model of the firmware.
It needs the locked files in `tools/.desk_ui_cache/` and refuses to start without them:

```bash
python3 tools/fetch_desk_ui.py --from ../di.iiii --cache tools/.desk_ui_cache   # or without --from: GitHub at the locked commit
python3 tools/simulate_node.py --port 8088   # then open http://127.0.0.1:8088
```

## Desk UI in flash (build step V1)

di.iiii's desk UI files (`docs/DI_DESK_UI_PLAN.md`) are pinned, never edited here. `desk_ui.lock` names the di.iiii
commit, the files, and the sha256 of each raw file and of its `gzip -9` output (mtime 0, so reruns are byte-identical).
`include/desk_ui_gz.h` is generated from it and committed; the firmware serves it (V2, below).

```bash
python3 tools/fetch_desk_ui.py                      # fetch at the locked commit, verify sha256, write the header
python3 tools/fetch_desk_ui.py --from ../di.iiii    # same, from a local checkout (its files must match the lock)
python3 tools/fetch_desk_ui.py --check              # fail when the committed header differs from a fresh one
python3 tools/fetch_desk_ui.py --update --commit <sha> [--branch <name>]   # bump the pin (a deliberate PR)
python3 tools/flash_gate.py esp32dev                # used/free of the app slot; exit 1 under 128 KB free
```

### How the node serves it (V2)

- `/` is `index.html`; `/app.js`, `/style.css`, `/qr.js`, `/from.js`, `/show.js`, `/caps.js` are their files. Each is sent
  from PROGMEM (`beginResponse_P`, no heap copy), already gzipped: `Content-Encoding: gzip`, `Content-Type` from the table in
  `desk_ui_gz.h`, `ETag: "<first 16 hex of the raw file's sha256>"`, `Cache-Control: no-cache`.
- **304 rule:** a request whose `If-None-Match` lists that ETag (or `*`; a `W/` prefix is accepted) gets `304` with the ETag and
  no body. `no-cache` means the browser always asks, and the answer costs a few hundred bytes instead of the file.
- The old console is gone (`APP_HTML` removed). `/control` answers 301 to `/#control`; `/patch`, `/scenes`, `/vj` answer 301 to
  `/` (di's pages are `setup`, `control`, `touch`, `fader`, `midi`: no page of those names). `/network`, `/system` and
  `/performance` served the same page and answer 301 to `/`. Until the node-settings section in di's desk exists, Wi-Fi setup
  is available only through the console POST routes below.
- Until the `/api` bridge (plan step V4) exists, di's desk loads but cannot reach `api/state` and says it is offline.

`pio run -e esp32dev` runs the flash gate after linking (`MIN_FREE_BYTES` at the top of `tools/flash_gate.py`;
slot = smallest `app` partition of `board_build.partitions`, default two OTA slots of 1,310,720 B).
The native tests check the header's sizes and etags against the lock.

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

`/status` (and the `/ws` push) also carry `heap_free`, `heap_max_block` (largest allocatable block), `ws_clients` and `sse_clients` (live `/api/dmx/stream` clients); the manifest repeats them under `state`.

`tools/heap_probe.mjs --node http://<ip> [--clients 3] [--sse 3] [--seconds 30]` samples those keys at idle, with N extra `/ws` clients and/or N `/api/dmx/stream` readers, and after they close (min/max/last, largest-block drop and recovery); it also counts the events each stream reader received. Node ≥ 22, no dependencies (Node has no `EventSource`: the probe reads the stream with `fetch`).
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
| GET | - | - | `/` `/app.js` `/style.css` `/qr.js` `/from.js` `/show.js` `/caps.js` | - | di's desk UI files from flash (gzip, ETag, 304) |
| GET | - | - | `/control` `/patch` `/scenes` `/vj` `/network` `/system` `/performance` | - | 301 to the desk (`/#control` or `/`) |
| WS | - | - | `/ws` | - | Status push every ~400 ms; refuses a foreign `Origin` |

### Desk API (`/api/*`, build step V4)

**Testing rule:** di's `test-http.js --class a` adds and REMOVES fixtures and groups; run it against `tools/simulate_node.py`. On a real node only with `curl http://<node>/api/state > patch-before.json` first and a restore afterwards (measured 2026-10-15: two runs left the node with 0 fixtures).

di's desk page at `/` talks to these. They are di's own desk API, held to di's
`serverXR/src/lighting/api-contract.json` (vendored as
`test/fixtures/api-contract.v1.json`, commit 091a1a6): **23 class-a routes really
work, 28 class-b answer static shapes or `{error:"not on this node"}`, 8 class-c
answer `404 {error}`**. Mapping rules and decisions: `docs/DESK_BRIDGE_CORE.md`.

- **Gate.** GET routes: same origin, no `Access-Control-Allow-Origin`. POST routes
  need `Content-Type: application/json` (else `415`). That is a non-simple request:
  a cross-site page cannot send it without a CORS preflight, and every `/api/*`
  preflight answers `403` without ACAO (OWASP CSRF Prevention Cheat Sheet). di's
  `post()` sends exactly that header and nothing custom. A method a path lacks
  answers `405` with `Allow`. No login yet (owner's decision, as for the operator routes).
- **Bodies** are JSON objects up to 2048 bytes (`413` above, `400 {error}` when not
  valid JSON or not an object), parsed with ArduinoJson 7.4.2.
- **Replies** are JSON, streamed chunked from one per-request copy of the facts
  (`Cache-Control: no-store`). Under 32 KB of largest free heap block the node
  answers `503 {"error":"node busy"}` instead of allocating; `/status` counts
  `api_replies` and `api_busy`. A busy lock answers `503 {"error":"busy"}`.
- Ids are the node's: fixtures `f1`…`f12` (slot + 1), scenes `s1`…`s8`, groups `g1`…`g8`;
  universe is always 0.

```bash
curl http://192.168.4.1/api/state
curl -X POST -H 'content-type: application/json' -d '{"master":200}' http://192.168.4.1/api/master
```

| Method | Class | Path | What the node does |
|---|---|---|---|
| GET | a | `/api/state` | Whole desk state, streamed from one frozen copy: fixtures (values read from the web layer through each profile), saved scenes (read from their snapshots through today's patch), groups, chase, fx, output, status |
| GET | a | `/api/dmx` | `outVals`, the 512 bytes on the cable (merged, mastered, slew-limited), as `dmx["0"]` |
| GET | a | `/api/dmx/stream` | The same bytes pushed as server-sent events (V5, below); not in contract v1, advertised by `caps.stream` |
| GET/POST | a | `/api/input` | GET: the source mode in di's words (input off = WEB, line desk `follow` = ARTNET, `htp` = HTP; loss `release`/`hold` = fallback). POST changes mode/fallback (saved); refuses `artnet:false`/`sacn:false`, another universe, LTP, a different name |
| POST | a | `/api/input/release` | Lets go of a held network frame until a source is live again |
| POST | a | `/api/master` | `master` = master dimmer; `blackout:true` = `/blackout` (web layer to 0, cue list and FX stop); `blackout:false` changes nothing (no blackout flag on the node) |
| POST | a | `/api/raw` | `channel`/`channels[]` write web-layer channels (universe 0 only); `clear` and null values change nothing (one layer, nothing held) |
| POST | a | `/api/fx` | The node's FX engine (`mode` from `fxModes`, `bpm` 20–240, `depth`, `enabled`); refuses unknown modes, `spatial` other than `patch`, a non-empty `exclude` |
| POST | a | `/api/chase` | The cue runner: `sceneIds` become the steps, `holdMs`/`fadeMs` each step's dwell/fade, `enabled` runs it; refuses `sync:"beat"` |
| POST | a | `/api/output` | Art-Net OUT: `enabled` (saved), `mode`/`targets` = broadcast or one unicast peer; refuses other drivers, a serial port, a refresh other than 43 Hz, two targets |
| POST | a | `/api/fixtures/add` | Next free slot, profile by name from the 13 built-in, `address` from the body or the next free one; 409 on a taken address without `force`; never writes the web layer |
| POST | a | `/api/fixture` | `values` written through the profile into the web layer (cancels a running fade), `name`, `x`/`y`; `on:false` refused (no mute mask yet) |
| POST | a | `/api/fixtures/all` | `values` to every listed fixture; `on:false` refused |
| POST | a | `/api/fixtures/move` | Stage position (`x`, `y` 0..1 kept as 0..255) |
| POST | a | `/api/fixtures/readdress` | New start address (universe 0); atomic, 409 on overlap without `force` |
| POST | a | `/api/fixtures/identify` | Level and colour roles to full for `seconds` (1–10, default 2), then the saved values are written back; refused while a fade runs; `off` ends it |
| POST | a | `/api/groups/add` | One channel range = the selection; refused when another fixture lies inside that range |
| POST | a | `/api/scenes/save` | Web layer into the first free of the 8 NVS slots, with `name` (`sN.name`) and `fadeMs` (`sN.fade`); 409 when all 8 are used |
| POST | a | `/api/scenes/update` | `name`, `fadeMs`, `restage` (re-capture the web layer); refuses `fx`/`lfos`/`audioCfg` |
| POST | a | `/api/scenes/recall` | Fades to the slot over `fadeMs` or the scene's own fade; stops a running chase |
| POST | a | `/api/scenes/remove` | Clears the slot and its NVS keys, and the chase steps that used it |
| POST | a | `/api/groups/remove` | Disables the group slot |
| POST | a | `/api/fixtures/remove` | Unpatches the listed fixtures |
| GET | b | `/api/cues` | An empty cue list (`n:0` hides the cue strip) |
| POST | b | `/api/cues/go` | `{error:"not on this node", cues:<empty list>}` |
| POST | b | `/api/cues/back` | `{error:"not on this node", cues:<empty list>}` |
| POST | b | `/api/cues/stop` | `{error:"not on this node", cues:<empty list>}` |
| POST | b | `/api/cues/loop` | `{error:"not on this node", cues:<empty list>}` |
| GET | b | `/api/show` | One show, this node's, `live` = Art-Net OUT on |
| GET | b | `/api/looks` | `{looks:[]}` |
| POST | b | `/api/looks/add` | `{error:"not on this node"}` (200) |
| POST | b | `/api/looks/remove` | `{error:"not on this node"}` (200) |
| POST | b | `/api/looks/capture` | `{error:"not on this node"}` (200) |
| POST | b | `/api/looks/fire` | `{error:"not on this node"}` (200) |
| POST | b | `/api/layer` | `{error:"not on this node"}` (200) |
| POST | b | `/api/layers/add` | `{error:"not on this node"}` (200) |
| POST | b | `/api/layers/remove` | `{error:"not on this node"}` (200) |
| POST | b | `/api/lfos` | `{error:"not on this node"}` (200) |
| POST | b | `/api/sets` | `{error:"not on this node"}` (200) |
| POST | b | `/api/fan` | `{error:"not on this node"}` (200) |
| POST | b | `/api/fixtures/limits` | `{error:"not on this node"}` (200) |
| POST | b | `/api/profiles/add` | `{error:"not on this node"}` (200) |
| POST | b | `/api/profiles/remove` | `{error:"not on this node"}` (200) |
| POST | b | `/api/output/send` | `{error:"not on this node"}` (200) |
| POST | b | `/api/output/send/remove` | `{error:"not on this node"}` (200) |
| POST | b | `/api/nodes/add` | `{error:"not on this node"}` (200) |
| POST | b | `/api/nodes/remove` | `{error:"not on this node"}` (200) |
| POST | b | `/api/discover` | `{error:"not on this node"}` (200) |
| POST | b | `/api/serial/identify` | `{error:"not on this node"}` (200) |
| POST | b | `/api/audio` | `{error:"not on this node"}` (200) |
| POST | b | `/api/audiocfg` | `{error:"not on this node"}` (200) |
| GET | c | `/api/library` | 404 `{error:"not on this node"}` |
| GET | c | `/api/library/manufacturer` | 404 `{error:"not on this node"}` |
| GET | c | `/api/library/fixture` | 404 `{error:"not on this node"}` |
| POST | c | `/api/library/import` | 404 `{error:"not on this node"}` |
| GET/POST | c | `/api/midi` | 404 `{error:"not on this node"}` |
| POST | c | `/api/show/open` | 404 `{error:"not on this node"}` |
| POST | c | `/api/show/copy-machine` | 404 `{error:"not on this node"}` |

### DMX stream (`GET /api/dmx/stream`, build step V5)

`outVals` (what `/api/dmx` shows: on the cable) pushed as server-sent events, in the shape of di's `dmxstream.js`
(`serverXR/src/lighting/dmxstream.js`, read by `useLightingMirror.js`):

```text
retry: 1000
event: frame
data: {"s":1,"t":48210,"k":1,"d":[[0,0,[0,0,...512 values]]],"m":{"master":255,"blackout":false,"looks":[],"cues":null}}

event: frame
data: {"s":2,"t":48260,"k":0,"d":[[0,63,[180,0,12]]]}
```

- `k:1` is a key frame (all 512 slots as one run, with `m`); `k:0` a delta: only the runs of slots that changed since the
  frame before, runs closer than 6 slots joined (`dmxstream.js` `deltaRuns`). `m` rides on a delta only when it changed.
- Rate: at most one event per 50 ms (20 Hz, `vizzz::desk::stream::kPeriodMs`), sent only when `outVals` or the master
  changed. **Keepalive:** when nothing changed for 1 s the frame is sent again as a key frame (instead of di's 15 s comment line),
  which also heals a client whose queue overflowed. A client that joins makes the next tick a key frame for everybody.
- Differences from di's own stream, on purpose: one frame for every client, so `s` counts the node's events, not one client's;
  `t` is the node's uptime in ms (no wall clock; only a look's `since` uses it and the node has no looks); no `?u=` filter
  (one universe, 0).
- **Client cap: 4** (`kMaxClients`). The 5th gets `503 {"error":"stream full"}` (a client still being accepted counts).
  Under 32 KB of largest free block every `/api` route, this one included, answers `503 {"error":"node busy"}`.
- The route is a plain GET (no `Accept` needed), same-origin, no CORS. `/api/state` says `caps.stream: true`; the vendored
  contract (v1) does not list the path, so the route sits in `vizzz::desk::kDeskExtra` and `tools/check_routes.py` holds it apart.
- Memory: the event JSON is written into one static 2,304 B buffer from `loop()` (`streamTick`), after copying the frame under
  `gLock` and releasing it; the library then makes one shared copy of the event for all clients (its only allocation on this path).
  `SSE_MAX_QUEUED_MESSAGES=8` (library default 32) bounds what a stalled client can hold. `tools/heap_probe.mjs --sse N` measures it.

```bash
curl -N http://192.168.4.1/api/dmx/stream
```

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

- AsyncTCP 3.3.2
- ESPAsyncWebServer 3.6.0
- ArduinoJson 7.4.2 (`/api/*` POST bodies only)
- Direct ArtDMX parser over Arduino WiFiUDP
- esp_dmx, vendored in `lib/esp_dmx/`
- Arduino WiFi / WiFiUDP / Preferences
