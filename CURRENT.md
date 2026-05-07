# Current State

Read this before changing the firmware. Keep it short and replace stale facts.

## Last verified baseline

`f2946cb` - `fix: defer WiFi.begin/disconnect to loop() task`
Branch: `main`

## What works

- ESP32 firmware builds with project-local PlatformIO core data.
- `onboard_device.py` reads an ESP MAC, can clean erase/flash one USB board, and can assign name/universe/mode over HTTP once the node is reachable.
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
- Art-Net OUT can broadcast the final output for receiver nodes, or unicast to a specific peer IP via `/artout/peer?ip=X`.
- Firmware node manifest is served at `/node/manifest` and `/manifest.json`.
- Peer discovery: nodes broadcast a UDP beacon on port 47777 every 30s; incoming beacons are parsed and stored in a peer table (max 4, expire after 90s). `/peers` returns the live list. Network tab shows WiFi status (AP clients, STA RSSI) and peers with a Link button.
- Fleet controls run peer HTTP forwarding from a FreeRTOS task via a fixed queue, not from `loop()`, so offline peers do not stall DMX timing. Peer table IPs come from the UDP sender address, and forwarded paths are limited to blackout, full, master, and scene recall.
- VJ controls on `/vj` include stabilized FX + color wash: FX (`strobe`/`chase`/`pulse`/`sine`/`sparkle` with BPM + tap), cue runner (up to 16 steps), and fixture groups (8 ranges). External control endpoints: `/fx/*`, `/cue/*`, `/group/*`, `/color/set`, plus OSC input on UDP/9000 (`/ch/N`, `/group/G`, `/master`, `/scene/recall`, `/cue/run`, `/fx/mode`, `/fx/bpm`, `/color/r|g|b`).
- `/vj` is the main mobile VJ controller: command strip, scene pads, FX pad matrix, RGB swatches/sliders, cue runner, group fader deck, and fleet controls. `/performance` remains an alias.
- Unit test file fixed: was referencing old `vidili_core.h` / `vidili::` namespace, corrected to `vizzz_core.h` / `vizzz::`.

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
- 2026-05-06: Peer discovery + WiFi status added. Native tests fixed (vidili→vizzz). RAM 51,876 bytes (15.8%), Flash 870,629 bytes (66.4%). Build SUCCESS.
- 2026-05-07: VJ feature pass added (FX, cues, groups, OSC-in). Validation: native tests PASS, esp32 build SUCCESS. RAM 52,348 bytes (16.0%), Flash 880,349 bytes (67.2%).
- 2026-05-07: VJ stabilization pass: fixed OSC padded-string alignment, added mutex protection for VJ state routes/JSON, enforced group `enabled`, updated node manifest routes/protocols, and added color controls + more FX. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,348 bytes (16.0%), Flash 888,101 bytes (67.8%).
- 2026-05-07: Fleet control repair: moved peer HTTP forwarding out of `loop()` into a FreeRTOS queue/task, made discovery trust UDP sender IPs, whitelisted forwarded paths, fixed partial-success fleet route handling, and documented/manifested fleet routes. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,356 bytes (16.0%), Flash 898,721 bytes (68.6%).
- 2026-05-07: No-light triage over AP `10.0.0.1`: live node was reachable as `vizzz.di_EA8982`, STA disconnected, mode was `ARTNET_ONLY`, universe was `5`. Forced `WEB_ONLY`, master 255, and channels 1-4 to 255; `/page?i=0` reported web/out `[255,255,255,255,0...]`. Firmware bug found/fixed locally: `sendDMX()` now calls `dmx_write()`, `dmx_send()`, then `dmx_wait_sent()` per hardware rule. Validation PASS, but USB device was not visible (`/dev/ttyUSB0` absent), so this fix still needs flashing.
- 2026-05-07: Two ESPs identified. Board A `D4:E9:F4:BA:6F:CC` was flashed first. Board B/light node `D4:E9:F4:BC:5A:64` was then verified on `/dev/ttyUSB0` and flashed successfully. After reboot it is reachable on STA `192.168.88.127`, AP SSID rotated to `vizzz.di_D05B23`, manifest has the new fleet/OSC routes, and direct test pattern reports web/out channels 1-4 at 255.
- 2026-05-07: Clean-erased and reflashed Board B `D4:E9:F4:BC:5A:64` on `/dev/ttyUSB0`; erase wipes STA credentials/NVS, so old `10.0.0.1`/`192.168.88.127` checks timed out afterward until reconnecting to the newly generated AP or reconfiguring WiFi.
- 2026-05-07: Added `onboard_device.py` for future multi-node onboarding: MAC check, optional erase, upload, and HTTP name/universe/mode/test setup. Use it one board at a time, and verify MAC before erase.
- 2026-05-07: Rebuilt the embedded `/vj` UI into a mobile-first VJ deck using existing firmware endpoints; no new DMX hardware behavior changed. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,356 bytes (16.0%), Flash 907,569 bytes (69.2%).
- For TouchDesigner control: Art-Net to `192.168.88.255:6454` (broadcast) or `192.168.88.127:6454` (unicast), universe 0. Use `ARTNET_ONLY` for TD-only or `MERGE_HTP` if web layer should participate.
