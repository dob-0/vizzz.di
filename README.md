# vi_di_li

ESP32 WiFi → DMX512 controller. Outputs DMX512 via MAX485 with a browser-based editor UI, VJ performance UI, Art-Net input/output, multi-device sync, and WebSocket live monitoring.

## Features

| Feature | Detail |
|---|---|
| **3 output modes** | `WEB_ONLY`, `ARTNET_ONLY`, `MERGE_HTP` (highest-takes-precedence) |
| **Editor UI** (`/`) | 512-channel sliders, page view (32 ch/page × 16 pages), blackout/full |
| **VJ UI** (`/vj`) | Dark touch-optimised, master dimmer, 8 scene buttons, fade time, Art-Net OUT toggle |
| **Art-Net IN** | Configurable Net/Subnet/Universe, 3 s hold-last on loss |
| **Art-Net OUT** | Broadcast current output as Art-Net to all devices on AP subnet (10.0.0.255) for multi-unit sync |
| **Scenes** | 8 slots, save/recall with crossfade |
| **Master dimmer** | 0–255 applied to all channels on every output path |
| **WebSocket** | Live status push every 400 ms at `ws://10.0.0.1/ws` |
| **WiFi** | AP always on + optional STA (scan & connect via UI) |
| **Persistent config** | All settings written to ESP32 flash (Preferences NVS) |
| **Thread-safe** | FreeRTOS mutex guards shared channel state between web (core 0) and DMX (core 1) |

## Hardware

Board: **ESP32 DevKit** (38-pin)

| Signal | GPIO | Notes |
|---|---|---|
| DMX TX → MAX485 DI | **25** | |
| DMX DIR → MAX485 DE+RE | **21** | Both pins tied together |
| MAX485 A/B | — | To XLR pin 3/2, shield to pin 1 |

> **Do not use DMX_NUM_0** (UART0) — it conflicts with the USB serial port.

## Wiring

```
ESP32 3.3 V ──── MAX485 VCC
ESP32 GND   ──── MAX485 GND
GPIO 25     ──── MAX485 DI
GPIO 21     ──── MAX485 DE + RE (bridge)
MAX485 A    ──── XLR pin 3  (DMX+)
MAX485 B    ──── XLR pin 2  (DMX−)
GND         ──── XLR pin 1  (shield)
```

## Quick Start

1. Build & flash (see Build section below)
2. Connect to WiFi AP **`vi_di_li`** — password `Poghka888$`
3. Open **`http://10.0.0.1`** — editor UI  
   Open **`http://10.0.0.1/vj`** — VJ / performance UI
4. To join an existing network: Settings → Network in the editor UI

## Build

PlatformIO is required. The binary is at `~/.platformio/penv/bin/pio` after install.

```bash
# Build firmware
~/.platformio/penv/bin/pio run -e esp32dev

# Upload (requires dialout group access on Linux)
sg dialout -c "~/.platformio/penv/bin/pio run --target upload"

# Run unit tests (native, no hardware needed)
~/.platformio/penv/bin/pio test -e native
```

VS Code tasks are pre-configured in `.vscode/tasks.json` (Build / Upload / Test).

## Multi-Device Sync (Art-Net OUT)

One device acts as **master**, the rest as **slaves**.

- On the master: enable **Art-Net OUT** in the VJ UI (or `GET /artout/set?en=1`)
- On each slave: set mode to `ARTNET_ONLY` and configure matching Net/Subnet/Universe
- The master broadcasts its output to `10.0.0.255:6454` every DMX cycle

All devices must be on the same AP (connect slaves to the master's AP, or put all devices on a shared router).

## HTTP API

| Method | Path | Query / Body | Description |
|---|---|---|---|
| GET | `/set` | `ch=N&v=V` | Set channel N (1–512) to value V (0–255) |
| GET | `/blackout` | — | All channels → 0 |
| GET | `/full` | — | All channels → 255 |
| GET | `/master` | `v=N` | Set master dimmer (0–255) |
| GET | `/mode/set` | `mode=0\|1\|2` | WEB / ARTNET / HTP |
| GET | `/artout/set` | `en=0\|1` | Enable Art-Net OUT broadcast |
| GET | `/artnet/set` | `net=N&sub=S&uni=U` | Configure Art-Net universe |
| GET | `/scene/save` | `slot=0-7` | Save current channels to scene slot |
| GET | `/scene/recall` | `slot=0-7&fade=ms` | Recall scene with fade |
| GET | `/wifi/scan` | — | JSON list of nearby SSIDs |
| GET | `/wifi/set` | `ssid=X&pass=Y` | Connect to STA network |
| GET | `/wifi/forget` | — | Clear STA credentials |
| GET | `/node/set` | `name=X` | Set node name |
| GET | `/reboot` | — | Reboot device |
| GET | `/status` | — | JSON: mode, artnet, master, IPs, uptime |
| GET | `/page` | `p=0-15` | JSON: 32-channel page snapshot |
| GET | `/monitor` | — | JSON: full 512-ch output snapshot |
| WS  | `/ws` | — | Push JSON status every 400 ms |

All mutating routes hold the FreeRTOS mutex; they return HTTP 503 if the system is busy.

## Resource Usage

| Resource | Used | Total |
|---|---|---|
| RAM | ~52 KB (15.9%) | 327 KB |
| Flash | ~877 KB (66.9%) | 1310 KB |

## Dependencies

- [AsyncTCP](https://github.com/me-no-dev/AsyncTCP)
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- [ArtnetWifi](https://github.com/rstephan/ArtnetWifi)
- [esp_dmx](https://github.com/someweisguy/esp_dmx) (local copy in `lib/esp_dmx/`)
