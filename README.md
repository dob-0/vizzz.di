# vi_di_li

ESP32 WiFi to DMX512 gateway. Receives Art-Net over WiFi and outputs DMX512 via MAX485. Includes a browser-based control interface.

## Features

- **3 output modes**: Web Only, Art-Net Only, HTP Merge (web + Art-Net)
- **Web UI**: 512-channel sliders, blackout/full buttons, page navigation
- **Art-Net**: configurable Net/Subnet/Universe, 3s timeout with hold-last
- **Scenes**: 8 scenes with adjustable fade time
- **WiFi**: AP mode always on + optional STA (client) mode with scan
- **mDNS**: accessible at `vi-di-li.local`
- **Persistent config**: all settings saved to flash (Preferences)

## Hardware

| Signal | GPIO |
|--------|------|
| DMX TX (MAX485 DI) | 25 |
| DMX DIR (MAX485 DE+RE) | 21 |

Board: ESP32 DevKit

## Wiring

```
ESP32 GPIO25 → MAX485 DI
ESP32 GPIO21 → MAX485 DE + RE (tied together)
MAX485 A/B   → DMX line
```

## Getting Started

1. Flash with PlatformIO (`pio run -t upload`)
2. Connect to WiFi AP **vi_di_li** (password: `Poghchka666#`)
3. Open `http://192.168.4.1` in a browser
4. Optionally connect the device to your network via the Network card in the UI

## Build

```bash
pio run
```

## Dependencies

- [AsyncTCP](https://github.com/me-no-dev/AsyncTCP)
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- [ArtnetWifi](https://github.com/rstephan/ArtnetWifi)
- [esp_dmx](https://github.com/someweisguy/esp_dmx)
