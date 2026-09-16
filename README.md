# vizzz.di

ESP32 WiFi -> DMX512 firmware node. It outputs DMX512 through a MAX485
transceiver, accepts Art-Net and sACN input, broadcasts Art-Net for node sync,
and exposes a black/cyan browser console plus a machine-readable node manifest.

## Features

| Feature | Detail |
|---|---|
| **3 output modes** | `WEB_ONLY`, `ARTNET_ONLY`, `MERGE_HTP` |
| **Console UI** | Routes: `/`, `/control`, `/patch`, `/scenes`, `/network`, `/system`, `/vj` (`/performance` alias) |
| **DMX output** | 512 channels on `DMX_NUM_1`, GPIO25 TX, GPIO21 DIR |
| **Art-Net IN/OUT** | Configurable Net/Subnet/Universe, hold-last on loss, optional broadcast output |
| **sACN IN** | E1.31 listener on port `5568`, same universe as Art-Net |
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

- Controller: enable Art-Net OUT in the console or call `/artout/set?en=1`
- Receiver nodes: set mode to `ARTNET_ONLY` and use the same universe
- Broadcast target: STA subnet broadcast when joined to a router, otherwise
  `192.168.4.255:6454`
- Nodes advertise themselves with UDP beacons on port `47777`; `GET /peers`
  returns the live peer table.
- The performance page includes fleet controls backed by `/net/blackout`,
  `/net/full`, `/net/master`, and `/net/scene/recall`.

## HTTP API

Mutating routes support both `GET` and `POST` for transition compatibility.

| Method | Path | Query | Description |
|---|---|---|---|
| GET/POST | `/set` | `ch=1-512&v=0-255` | Set one web-layer channel |
| GET/POST | `/blackout` | - | Set all web-layer channels to 0 |
| GET/POST | `/full` | - | Set all web-layer channels to 255 |
| GET/POST | `/master` | `v=0-255` | Set master dimmer |
| GET/POST | `/mode/set` | `m=0\|1\|2` | WEB / ARTNET / HTP |
| GET/POST | `/mode/fallback` | `en=0\|1` | In ARTNET mode, fall back to web layer when network input is inactive |
| GET/POST | `/netmode/set` | `m=0\|1\|2` | AP_STA / STA_ONLY / AP_ONLY, then reboot |
| GET/POST | `/artout/set` | `en=0\|1` | Enable Art-Net OUT |
| GET | `/artout/peer` | `ip=X` | Use one peer as Art-Net OUT unicast target |
| GET/POST | `/artnet/set` | `net=N&subnet=S&uni=U` | Configure Art-Net/sACN universe |
| GET/POST | `/scene/save` | `n=0-7` | Save current web layer |
| GET/POST | `/scene/recall` | `n=0-7&fade=ms` | Recall scene with fade |
| GET/POST | `/net/blackout` | - | Blackout this node and all discovered peers |
| GET/POST | `/net/full` | - | Full on this node and all discovered peers |
| GET/POST | `/net/master` | `v=0-255` | Set master on this node and all discovered peers |
| GET/POST | `/net/scene/recall` | `n=0-7&fade=ms` | Recall scene on this node and all discovered peers |
| GET | `/wifi/scan` | - | JSON SSID scan |
| GET/POST | `/wifi/set` | `ssid=X&pass=Y` | Connect STA |
| GET/POST | `/wifi/forget` | - | Clear STA credentials |
| GET/POST | `/node/set` | `name=X&ap_ssid=Y&ap_pass=Z` | Update node identity |
| GET | `/discover` | - | Return node identity and send a discovery beacon |
| GET | `/peers` | - | Live discovered peer table |
| GET | `/peer/cmd` | `ip=X&path=/blackout\|/full...` | Forward an allowed command to one known peer |
| GET/POST | `/group/set` | `g=0-7&name=X&start=A&end=B&en=0\|1` | Configure one group |
| GET/POST | `/group/apply` | `g=0-7&v=0-255` | Apply value to one group range |
| GET/POST | `/cue/count` | `c=1-16` | Set active cue step count |
| GET/POST | `/cue/set` | `i=0-15&scene=0-7&dwell=ms&fade=ms` | Configure one cue step |
| GET/POST | `/cue/run` | `en=0\|1` | Start/stop cue runner |
| GET/POST | `/cue/next` | - | Advance cue runner one step |
| GET/POST | `/fx/set` | `mode=...&en=0\|1&bpm=20-240&depth=0-255` | Configure FX engine |
| GET/POST | `/fx/tap` | - | BPM tap tempo input |
| GET/POST | `/color/set` | `en=0\|1&r=0-255&g=0-255&b=0-255` | Configure color wash |
| GET/POST | `/safety/set` | `burn=0\|1` | Toggle burn-safe mode |
| GET | `/safety/status` | - | Burn-safe/effective safety parameters |
| GET | `/status` | - | Live status JSON |
| GET | `/page` | `i=0-15` | 32-channel page snapshot |
| GET | `/monitor` | - | First 64 output channels |
| GET | `/node/manifest` | - | Firmware node manifest |
| GET | `/manifest.json` | - | Same firmware node manifest |
| WS | `/ws` | - | Status push every ~400 ms |

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
