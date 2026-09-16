# Current State

Read this before changing the firmware. Keep it short and replace stale facts.

## Last verified baseline

`f4c695e` - `fix: 17 defects from the audit, and take the AP password out of the repo`
Branch: `audit/2026-08-24` (pushed to origin; `main` untouched at `42f1963`)
RAM 52,740 bytes (16.1%), Flash 927,093 bytes (70.7%), native tests 4/4.

## Last session - 2026-08-24/25

- Audited the firmware in three lanes (DMX/safety, network/HTTP, console UI + docs): 33 findings in `docs/AUDIT_2026_08_24.md`, 17 of them fixed in the same branch.
- Worst find: all four UDP pollers never called `flush()`, so one oversized datagram killed that input until reboot - OSC at 256 bytes, which TouchOSC exceeds by accident.
- Also fixed: Art-Net OUT feeding its own IN, a panic button that could set the rig to full, `loadFixtures()` that was never called, group/cue "Save" that only wrote to RAM, and a `/web/set?en=0` that bricked the console beyond serial reflash.
- Added `tools/simulate_node.py`, which serves the real `APP_HTML` against an in-memory model of the firmware so the console can be run and screenshotted with no ESP32 attached.
- Added `docs/BUILD_A_NODE.md`; default AP password moved to gitignored `src/secrets.h` (template in `src/secrets.example.h`) so it is not in the public repo.

## Open

- **`d4:e9:f4:ba:6f:cc` still runs the older build** - new password, none of the 17 fixes. Erase + flash when it is next on USB.
- **Nothing is verified over HTTP.** Fixes are compile/boot-verified only. Put a node on the LAN (console -> WiFi -> join the house network) and re-run `tools/crash_http.py` + `tools/crash_udp.py`, which is the only real proof of the flush fix.
- **Two boards are dead**: the soldered perfboard node and one other. Both silent on serial at both boot baud rates with power LEDs lit, and no AP. Measure `EN` against `GND` (expect ~3.3 V; near 0 V means held in reset, which is repairable) and check whether the MAX485's `RO` is wired to anything - it must not be.
- Audit items deliberately not fixed: no auth on any route and `/reboot` by GET; master/ceiling/slew scaling all 512 channels with no channel-role model; `/blackout` doing nothing while a console feeds Art-Net; one shared `Preferences` object across three tasks; `delay(200)` + SoftAP restart inside `/wifi/scan`; peer-table spoofing into `/peer/cmd`; the WiFi scan wedge; `/ws` connection flood; burn-safe invisible in the UI; no DMX health signal; `/vj` at 7.1 screens of scroll with 38 sub-44px tap targets.
- No pull request opened - the branch is pushed for sharing, nothing is merged.

## What works

- ESP32 firmware builds with project-local PlatformIO core data.
- `onboard_device.py` reads an ESP MAC, can clean erase/flash one USB board, and can assign name/universe/mode over HTTP once the node is reachable.
- Visible project identity, default node name, and generated AP prefix use `vizzz.di`.
- Native Unity tests cover `vizzz_core.h` helpers.
- VS Code PlatformIO Build/Upload tasks explicitly target `esp32dev`, so they no longer fail by trying to build the `native` test environment with `pio run`.
- DMX output uses `DMX_NUM_1`, GPIO25 TX, GPIO21 DIR.
- Browser console exposes control, patch, scenes, network, system, and VJ routes.
- Embedded UI is intentionally minimal/mobile-first: hidden helper copy, compact cards, horizontal tabs, large touch targets, and WiFi first on `/network`.
- WiFi scan enables STA/AP+STA as needed and reports scan failures/timeouts to the UI instead of looping forever.
- Static RAM is 52,740 bytes (16.1%); short-lived scene/protocol/output scratch buffers use stack space instead of permanent globals.
- Flash is 927,093 bytes (70.7%) with C++ exceptions off, Arduino core debug logging off, and no ArtnetWifi dependency.
- Art-Net IN uses the local `pollArtNet()` WiFiUDP ArtDMX parser; keep it small/non-blocking.
- WebSocket `/ws` pushes status roughly every 400 ms using a stack JSON buffer to avoid periodic `String` heap churn.
- DMX output now has built-in safety guardrails: output ceiling (`SAFE_MAX_LEVEL=245`) and per-frame slew limiting (`SAFE_SLEW_STEP=18`) to reduce hard spikes and stress on fixtures/transceiver paths.
- Burn-safe stress profile added: `/safety/set?burn=1` lowers effective output ceiling/slew (`BURN_SAFE_MAX_LEVEL=96`, `BURN_SAFE_SLEW_STEP=8`) and is exposed in `/status`, `/safety/status`, and node manifest state.
- UDP ingest loops (Art-Net, sACN, OSC, discovery) now use per-loop packet budgets so heavy inbound traffic cannot starve the main DMX loop.
- Factory reset clears every namespace the firmware writes: `cfg`, `scenes`, `fixtures`, `vj`.
- Scenes and VJ are now unified at UI route level: `/scenes` is treated as an alias of `/vj` in the browser app, and the top tab points to one performance surface.
- WebSocket UI updates are route-aware to reduce browser CPU work (status/system/network/vj DOM blocks update only on their active route).
- Art-Net and sACN input share the configured universe.
- Art-Net OUT can broadcast the final output for receiver nodes, or unicast to a specific peer IP via `/artout/peer?ip=X`.
- Firmware node manifest is served at `/node/manifest` and `/manifest.json`.
- Peer discovery: nodes broadcast a UDP beacon on port 47777 every 30s; incoming beacons are parsed and stored in a peer table (max 4, expire after 90s). `/peers` returns the live list. Network tab shows WiFi status (AP clients, STA RSSI) and peers with a Link button.
- Fleet controls run peer HTTP forwarding from a FreeRTOS task via a fixed queue, not from `loop()`, so offline peers do not stall DMX timing. Peer table IPs come from the UDP sender address, and forwarded paths are limited to blackout, full, master, and scene recall.
- VJ controls on `/vj` include stabilized FX + color wash: FX (`strobe`/`chase`/`pulse`/`sine`/`sparkle`/`comet`/`bars`/`glitch` with BPM + tap), cue runner (up to 16 steps), and fixture groups (8 ranges). External control endpoints: `/fx/*`, `/cue/*`, `/group/*`, `/color/set`, plus OSC input on UDP/9000 (`/ch/N`, `/group/G`, `/master`, `/scene/recall`, `/cue/run`, `/fx/mode`, `/fx/bpm`, `/color/r|g|b`).
- `/vj` is the main mobile VJ controller: command strip, scene pads, FX pad matrix, RGB swatches/sliders, cue runner, group fader deck, and fleet controls. `/performance` remains an alias.
- Scene launcher now has a quick workflow: tap slot to select+preview, then one-tap `Capture`, `Go Selected`, or `Capture + Next` for faster programming with fewer clicks.
- Unit test file fixed: was referencing old `vidili_core.h` / `vidili::` namespace, corrected to `vizzz_core.h` / `vizzz::`.
- Core mutating routes now accept both GET and POST during transition (`/set`, `/blackout`, `/full`, `/master`, `/mode/set`, `/netmode/set`, `/web/set`, `/artnet/set`, `/artout/set`, `/scene/save`, `/scene/recall`, `/wifi/set`, `/wifi/forget`, `/node/set`, `/reboot`, `/safety/set`).
- Transition coverage expanded to VJ/fleet mutating routes: `/group/set`, `/group/apply`, `/cue/count`, `/cue/set`, `/cue/run`, `/cue/next`, `/fx/set`, `/fx/tap`, `/color/set`, `/net/blackout`, `/net/full`, `/net/master`, `/net/scene/recall` also accept GET+POST.
- Factory reset is now deferred from async callback to `loop()` (`pendingFactoryReset`) before NVS clear/restart.
- Added software crash/stress tools: `tools/crash_http.py` and `tools/crash_udp.py`.

## Hard rules

- Do not edit `lib/esp_dmx/`.
- Do not change DMX pins, UART, or `DMX_PERIOD_MS` without hardware approval.
- Do not hold `gLock` across DMX writes or blocking calls.
- Keep embedded UI in the black/cyan square `di.i`-style visual system.
- Record durable lessons, completed work, and next-step breadcrumbs in `CURRENT.md`; promote long-lived rules to `AGENTS.md` so AI work is not repeated after compaction/resume.
- Do not push to GitHub unless validation passes.

## Required validation

```bash
.venv-pio/bin/pio test -e native
.venv-pio/bin/pio run -e esp32dev
```

PlatformIO lives in a project-local virtualenv. First time:
`python3 -m venv .venv-pio && .venv-pio/bin/pip install platformio`

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
- 2026-05-07: Rebuilt the embedded `/vj` UI into a mobile-first VJ deck and upgraded FX behavior with depth scaling, softened chases/comet tails, travelling wave, sparkle variation, bars, and glitch. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,356 bytes (16.0%), Flash 904,605 bytes (69.0%).
- 2026-05-07: Elite multi-device identity fix (WLED-grade): (1) AP SSID now MAC-derived (`vizzz.di_AABBCC`, deterministic, survives NVS erase); (2) `WiFi.setHostname()` called before `WiFi.begin()` so DHCP table shows correct name; (3) beacon payload now includes `mac` + `ap_ssid` instead of hardcoded `ap_ip`; (4) peer self-exclusion and dedup use MAC as primary key, IP as fallback; (5) `mac` added to status JSON, `/discover`, `/peers`. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,436 bytes (16.0%), Flash 905,361 bytes (69.1%).
- 2026-05-07: Password/SSID mismatch hardening: startup now migrates any stored default-prefix AP SSID to the MAC-derived value automatically after reflashing, while preserving truly custom SSIDs; added `POST /factory-reset` to clear Preferences/NVS and reboot back to defaults (`vizzz.di_<MACSUFFIX>`, `Poghka888$`). Validation: esp32 build SUCCESS. RAM 52,436 bytes (16.0%), Flash 905,849 bytes (69.1%).
- 2026-05-14: Stability + safety optimization pass: added output ceiling + slew limiting in DMX output path, bounded UDP packet processing per loop for Art-Net/sACN/OSC/discovery, and fixed `/factory-reset` to clear `cfg` + `scenes` namespaces. Validation: native tests PASS (4/4), esp32 build SUCCESS. RAM 52,436 bytes (16.0%), Flash 906,177 bytes (69.1%).
- 2026-05-14: UI simplification pass: unified scenes into the VJ surface (`/scenes` -> `/vj` in frontend routing), removed duplicate scene recall JS functions, optimized scene key storage (no temporary `String` allocation in `loadScene`/`saveScene`), and reduced per-frame browser DOM updates by route. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,436 bytes (16.0%), Flash 906,517 bytes (69.2%).
- 2026-05-14: Reliability hardening phase 1 started: added burn-safe mode + status routes, switched core mutating routes to GET+POST compatibility, moved factory reset execution to deferred loop path, and hardened high-traffic JSON builders with bounded append helper. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,436 bytes (16.0%), Flash 908,705 bytes (69.3%).
- 2026-05-14: Reliability hardening phase 2: expanded GET+POST transition aliases to remaining mutating VJ/fleet routes (`/group/*`, `/cue/*`, `/fx/*`, `/color/set`, `/net/*`), rebuilt/flashed board `d4:e9:f4:ba:6f:cc`, and reran validation. Validation: native tests PASS, esp32 build SUCCESS. RAM 52,436 bytes (16.0%), Flash 909,513 bytes (69.4%).
- 2026-05-14: Extended software stress soak with new tools: `tools/crash_http.py --seconds 60 --workers 10` and `tools/crash_udp.py --seconds 60 --pps 220` completed (HTTP ok=812, fail=535; UDP sent=11216). Node API reachability from this shell to `10.0.0.1` remained environment-dependent/timeouts, so final live field inspection should be confirmed from the console browser network context.
- 2026-05-14: Live runtime reliability fix after user report ("not totally working"): switched app HTML route delivery to `beginResponse_P` with explicit cache header and increased status snapshot lock timeout from 2ms to 10ms. Reflashed board `d4:e9:f4:ba:6f:cc`. Browser-context verification: 12/12 sequential `/vj` fetches succeeded (stable content length), `/status` and `/safety/status` return valid JSON including `burn_safe` fields.
- 2026-05-14: User report: "no light but web platform work". Diagnostics: web API (GET/POST `/set`, `/mode/set`, etc.) fully functional; channel values correctly computed and clamped in outVals array; but DMX output to MAX485 hardware producing no signal. Root cause: hardware-level issue (MAX485 not installed, wrong pin connections, or short circuit). Software confirmed working after reflash: firmware boots, listens for commands, computes output correctly. Recommend: verify MAX485 physical installation, GPIO 25 TX→DI and GPIO 21 DIR→DE+RE connections, XLR shield/A/B wiring.
- 2026-05-07: Fixed `.vscode/tasks.json` so VS Code `PlatformIO: Build` and `PlatformIO: Upload` target `esp32dev` explicitly. Validation: `PlatformIO: Test` PASS, `PlatformIO: Build` PASS.
- For TouchDesigner control: Art-Net to `192.168.88.255:6454` (broadcast) or `192.168.88.127:6454` (unicast), universe 0. Use `ARTNET_ONLY` for TD-only or `MERGE_HTP` if web layer should participate.
- 2026-05-15: Anti-regression guard for no-light incidents: added persistent `artnetFallbackToWeb` safety behavior (default ON). In `ARTNET_ONLY`, when Art-Net/sACN is inactive, output now falls back to web layer instead of holding stale output. Added `GET/POST /mode/fallback?en=0|1`, plus status visibility via `artnet_fallback_to_web` in `/status` and `/safety/status`. Validation: native tests PASS, esp32 build SUCCESS, upload SUCCESS to `d4:e9:f4:ba:6f:cc`, live check in ARTNET mode shows `web_first=200` and `out_first=200` with no network input.
- 2026-05-15: Scene UX rewrite for beginners: removed legacy dedicated `/scenes` panel UI and replaced VJ scene controls with a child-friendly flow (`Pick scene` -> `Save This Look` / `Play This Scene`), plus speed presets (`Instant`/`Soft`/`Slow`) and simpler status hints. Backend scene APIs unchanged (`/scene/save`, `/scene/recall`). Validation: native tests PASS, esp32 build SUCCESS.
- 2026-05-15: Added THUNDER WASH 100 RGB quick mode for 6 fixtures: one-tap `THUNDER 6` patch preset writes six 3-channel fixtures (`TW1..TW6`, channels 1-18, 3x2 stage layout), switches to HTP + radar FX defaults, and applies an underground electro look. Added `Underground Colors` guard toggle plus muted palette mapping in `colorPreset` to avoid overly colorful scenes by default. Validation: native tests PASS, esp32 build SUCCESS, upload SUCCESS to `d4:e9:f4:ba:6f:cc`. Post-flash HTTP reachability from this shell to `10.0.0.1` was timeout-dependent.
- 2026-08-24: Four boards triaged over USB. `d4:e9:f4:bc:5a:64` and `d4:e9:f4:ba:6f:cc` both answer esptool immediately and were clean-erased and reflashed. Two others - the soldered perfboard node and one more - return zero bytes at 115200 and 74880, with BOOT held and after a forced RTS reset, and broadcast no AP; power LEDs lit on both, so they are powered but not executing. A bare board flashed fine on the same cable and port minutes earlier, so the cable, CH340 driver, toolchain and permissions are all ruled out. Next step is a meter on `EN` vs `GND`.
- 2026-08-24: Default AP password changed and then removed from the repository. It now comes from gitignored `src/secrets.h`; `src/secrets.example.h` is the template, the fallback placeholder is `changeme123`, and the firmware warns on serial at boot when the placeholder is in use. The branch was rebuilt from `origin/main` before pushing so no password appears in any pushed commit. Verified: `src/secrets.h` returns 404 on GitHub.
- 2026-08-25: Audit fix pass flashed to `d4:e9:f4:bc:5a:64`. Validation: native tests PASS (4/4), esp32 build SUCCESS, RAM 52,740 bytes (16.1%), Flash 927,093 bytes (70.7%), one clean boot with no brownout and no reset loop. The stage-patch coordinate fix was confirmed in a real browser against `tools/simulate_node.py` - all four default fixtures now render inside the canvas, where previously only F1 and half of F2 did. HTTP behaviour is still unexercised against hardware.
- 2026-09-16: Perfboard node `D4:E9:F4:BA:6F:CC` (AP `vizzz.di_BA6FCC`) is NOT dead: the AP comes up with the secrets.h password and answers ping, but TCP 80 is refused and a full Art-Net (uni 0-15, unicast+broadcast) + sACN (uni 1-16) sweep of ch 1-12 lit nothing on two Cameo Thunder Wash 100 RGB (6ch, addr 1 and 7). Reading: boot stalls after `startSoftAP()` and before `server.begin()`, so DMX never starts. USB did not enumerate at all (charge-only cable or dead USB bridge) - serial log is the next step.
- 2026-09-16: AP address moved from `10.0.0.1/24` to `192.168.4.1/24` (firmware, UI label, tools, README). 10.0.0.x collided with the studio OpenWrt mesh, so the bench router could not reach a node AP. `192.168.4.1` is also what the Art-Net Desk v2 already targets. Validation: native tests PASS 4/4, esp32 build SUCCESS, RAM 52,740 (16.1%), Flash 927,097 (70.7%). Not yet flashed.
