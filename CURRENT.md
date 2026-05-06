# Current State

Read this before changing the firmware. Keep it short and replace stale facts.

## Last verified baseline

`f2946cb` - `fix: defer WiFi.begin/disconnect to loop() task`
Branch: `main`

## What works

- ESP32 firmware builds with project-local PlatformIO core data.
- Visible project identity, default node name, and generated AP prefix use `vizzz.di`.
- Native Unity tests cover `vizzz_core.h` helpers.
- DMX output uses `DMX_NUM_1`, GPIO25 TX, GPIO21 DIR.
- Browser console exposes control, patch, scenes, network, system, and VJ routes.
- Embedded UI is intentionally minimal/mobile-first: hidden helper copy, compact cards, horizontal tabs, large touch targets, and WiFi first on `/network`.
- WiFi scan enables STA/AP+STA as needed and reports scan failures/timeouts to the UI instead of looping forever.
- Static RAM is optimized to 49,420 bytes (15.1%); short-lived scene/protocol/output scratch buffers use stack space instead of permanent globals.
- Flash is optimized to 837,901 bytes (63.9%) by disabling C++ exceptions, disabling Arduino core debug logging, and removing the ArtnetWifi dependency.
- Art-Net IN uses the local `pollArtNet()` WiFiUDP ArtDMX parser; keep it small/non-blocking.
- WebSocket `/ws` pushes status roughly every 400 ms using a stack JSON buffer to avoid periodic `String` heap churn.
- Art-Net and sACN input share the configured universe.
- Art-Net OUT can broadcast the final output for receiver nodes.
- Firmware node manifest is served at `/node/manifest` and `/manifest.json`.

## Hard rules

- Do not edit `lib/esp_dmx/`.
- Do not change DMX pins, UART, or `DMX_PERIOD_MS` without hardware approval.
- Do not hold `gLock` across DMX writes or blocking calls.
- Keep embedded UI in the black/cyan square `di.i`-style visual system.
- Record durable lessons, completed work, and next-step breadcrumbs in `CURRENT.md`; promote long-lived rules to `AGENTS.md` so AI work is not repeated after compaction/resume.
- Do not push to GitHub unless validation passes.

## Required validation

```bash
/home/nnn/.platformio/penv/bin/pio test -e native
/home/nnn/.platformio/penv/bin/pio run -e esp32dev
```

## Known issues

- `sg dialout` upload may need elevated shell permission in agent sandboxes.
- Starship may warn about read-only cache; this is not a firmware failure.
- PlatformIO `device monitor` needs an interactive TTY; in this agent shell it can fail with `termios.error: (25, 'Inappropriate ioctl for device')`.
- ESP32 WiFi scan/client mode only sees 2.4 GHz networks; 5 GHz-only studio SSIDs will not appear.

## Current hardware test

- 2026-05-06: Uploaded current `vizzz.di` firmware to ESP32 on `/dev/ttyUSB0`; upload succeeded and hard-reset via RTS. Board MAC: `d4:e9:f4:bc:5a:64`.
- 2026-05-06: Flashed WiFi scan fix to same board; upload succeeded and hard-reset via RTS. Re-test `/network` -> Scan on the ESP AP.
- 2026-05-06: Flashed minimal/mobile UI build to same board; upload succeeded and hard-reset via RTS.
- 2026-05-06: RAM optimization pass validated: RAM 50,140 bytes (15.3%), flash 889,701 bytes (67.9%).
- 2026-05-06: Flashed RAM-optimized build to ESP32 `d4:e9:f4:bc:5a:64`; upload succeeded and hard-reset via RTS.
- 2026-05-06: Full ESP optimization pass validated: RAM 49,420 bytes (15.1%), flash 837,901 bytes (63.9%).
- 2026-05-06: Flashed full ESP-optimized build to ESP32 `d4:e9:f4:bc:5a:64`; upload succeeded and hard-reset via RTS.
- 2026-05-06: WiFi STA join fixed (AsyncTCP task issue). Flashed fix, verified join/forget/rejoin on Yokozo. Art-Net IN verified working on both AP (10.0.0.1) and STA (192.168.88.127) interfaces.
- 2026-05-06: Node name updated to `vizzz.di` via `/node/set`; mDNS is now `vizzz-di.local`. Device on Yokozo at `192.168.88.127`.
- Next: flash the second ESP when connected, set a unique node name (e.g. `vizzz.di-2`) and AP SSID, then join Yokozo from `/network`.
- For TouchDesigner control: Art-Net to `192.168.88.255:6454` (broadcast) or `192.168.88.127:6454` (unicast), universe 0. Use `ARTNET_ONLY` for TD-only or `MERGE_HTP` if web layer should participate.
