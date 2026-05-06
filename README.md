# vizzz.di

ESP32 WiFi -> DMX512 firmware node. It outputs DMX512 through a MAX485
transceiver, accepts Art-Net and sACN input, broadcasts Art-Net for node sync,
and exposes a black/cyan browser console plus a machine-readable node manifest.

## Features

| Feature | Detail |
|---|---|
| **3 output modes** | `WEB_ONLY`, `ARTNET_ONLY`, `MERGE_HTP` |
| **Console UI** | Routes: `/`, `/control`, `/patch`, `/scenes`, `/network`, `/system`, `/performance`, `/vj` |
| **DMX output** | 512 channels on `DMX_NUM_1`, GPIO25 TX, GPIO21 DIR |
| **Art-Net IN/OUT** | Configurable Net/Subnet/Universe, hold-last on loss, optional broadcast output |
| **sACN IN** | E1.31 listener on port `5568`, same universe as Art-Net |
| **Scenes** | 8 slots, save/recall with fade |
| **Master dimmer** | 0-255 applied on the output path |
| **WebSocket** | Live status push every ~400 ms at `ws://10.0.0.1/ws` |
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
3. Open `http://10.0.0.1`.
4. Use `/vj` or `/performance` for the performance deck.
5. Use `/system` or `/node/manifest` to inspect the firmware-node contract.

Default AP password: `Poghka888$`.

## Build

PlatformIO is installed at `/home/nnn/.platformio/penv/bin/pio` in this
workspace. The project sets `name = vizzz.di` and `core_dir = .platformio-core`
so PlatformIO writes project-local cache/lock files instead of the read-only
user PlatformIO home.

```bash
# Build firmware
/home/nnn/.platformio/penv/bin/pio run -e esp32dev

# Upload to ESP32
sg dialout -c "/home/nnn/.platformio/penv/bin/pio run -e esp32dev --target upload"

# Native unit tests
/home/nnn/.platformio/penv/bin/pio test -e native
```

## Multi-Node Sync

One device can act as a controller node and broadcast the final output as
Art-Net to other nodes on the same network.

- Controller: enable Art-Net OUT in the console or call `/artout/set?en=1`
- Receiver nodes: set mode to `ARTNET_ONLY` and use the same universe
- Broadcast target: STA subnet broadcast when joined to a router, otherwise
  `10.0.0.255:6454`

## HTTP API

| Method | Path | Query | Description |
|---|---|---|---|
| GET | `/set` | `ch=1-512&v=0-255` | Set one web-layer channel |
| GET | `/blackout` | - | Set all web-layer channels to 0 |
| GET | `/full` | - | Set all web-layer channels to 255 |
| GET | `/master` | `v=0-255` | Set master dimmer |
| GET | `/mode/set` | `m=0\|1\|2` | WEB / ARTNET / HTP |
| GET | `/netmode/set` | `m=0\|1\|2` | AP_STA / STA_ONLY / AP_ONLY, then reboot |
| GET | `/artout/set` | `en=0\|1` | Enable Art-Net OUT |
| GET | `/artnet/set` | `net=N&subnet=S&uni=U` | Configure Art-Net/sACN universe |
| GET | `/scene/save` | `n=0-7` | Save current web layer |
| GET | `/scene/recall` | `n=0-7&fade=ms` | Recall scene with fade |
| GET | `/wifi/scan` | - | JSON SSID scan |
| GET | `/wifi/set` | `ssid=X&pass=Y` | Connect STA |
| GET | `/wifi/forget` | - | Clear STA credentials |
| GET | `/node/set` | `name=X&ap_ssid=Y&ap_pass=Z` | Update node identity |
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

## Dependencies

- AsyncTCP
- ESPAsyncWebServer
- Direct ArtDMX parser over Arduino WiFiUDP
- esp_dmx, vendored in `lib/esp_dmx/`
- Arduino WiFi / WiFiUDP / Preferences
