# Lead hand-over — 2026-10-15 03:3x (info@ session stopped at 91 % of its 5 h window, owner's order)

**State of the node BA6FCC (192.168.88.66, USB /dev/ttyUSB0, dob has an ACL on the port until replug):**
runs `int/2026-10-15` a03222b = audit fix-all (PR #1) + console in di's look (PR #2) + USB ENTTEC Pro widget (PR #3).
Proven from aylmo on this build: `tools/net_probe.py verify` 13/13, `tools/check_http_api.sh` 24/24,
`tools/usb_probe.js` 8/8 + `params` 4/4 (di's own enttec.js). Serial log is at **250000 baud** now.
Not proven: a real light on the DMX cable (none attached); 1 h soak (watchdog, ledger N686 HTTP-after-flood timeout).

**PRs (github dob-0/vizzz.di):** #1 fix/audit-2026-10-15 → audit/2026-08-24; #2 feat/console-di-look → #1;
#3 feat/usb-enttec-pro → #1; #4 plan/di-desk-ui → int/2026-10-15 (docs/DI_DESK_UI_PLAN.md, the "full change").
Owner's answers to the plan's questions: Q1 yes but carry over what the node console has and di lacks (D0 inventory
first, "look to not mess"); Q2 flex: alone = the node serves the desk UI itself, with di = di drives it; Q3 fixture
limit by measurement (32 only if it cannot break anything); Q4 yes (looks/layers/LFOs/sets/cues owed).

**Cloud runs on the dob login (env Default), fired 22:44Z, watch both `cloud/` and `claude/` prefixes:**
- trig_017au3LyBw7vb33WV2ZH6kGm — di.iiii D0+D1 → `cloud/desk-honest-render-caps-2026-10-15`, draft PR to dev.
- trig_01WnJwKMipa7ms9h3idLxQjG — vizzz.di V0 → `cloud/status-heap-keys-2026-10-15`, draft PR to int/2026-10-15.

**Next (in order):** review both PRs; merge V0 into int/2026-10-15; build + flash; read heap_free / heap_max_block from
/status at idle and with 3 phones on /vj → decide MAX_FIXTURES (12 vs 32) by the number; then plan steps D2–D5, V1–V7
(docs/DI_DESK_UI_PLAN.md §4). Owner decisions still open: login/token on operator routes; DMX Out blackout = web-layer
clear or /panic; the "di in a high-level language" conversation (ledger N689).

```bash
cd ~/work/vizzz.di && git checkout int/2026-10-15
PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio test -e native      # 36 green
PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio run -e esp32dev -t upload
PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio device monitor -b 250000
python3 tools/net_probe.py verify --node 192.168.88.66 --ch 64; tools/check_http_api.sh http://192.168.88.66
node tools/usb_probe.js --di ~/work/di.iiii --port /dev/ttyUSB0 --node http://192.168.88.66 --ch 64 --v 180 --seconds 5
```
Ledger rows: N668 N669 N671 N677 N680 N686 N689 N690 N691. Memory: reference_vizzz_node_ba6fcc.md.

---

# Handover — fix/audit-a (runtime findings of docs/AUDIT_2026_10_15.md), 2026-10-15

Worktree `/home/dob/work/vizzz.di-fix-a`, branch `fix/audit-a`, base `6e26c94`. Nothing flashed,
no serial, no call to the live node. Each finding's line in the audit doc says Fixed/Open.

## Done (18 runtime ids)

| commit | findings | what |
|---|---|---|
| d17912c | 1-C2 | identity Strings read via `IdentitySnap` under gLock; Art OUT peer = atomic u32 (strict IPv4, else 400); `mdnsName` fixed char[32]; MDNS restart + /discover beacon deferred to loop() |
| 617e4d0 | 1-H2, 1-M5 | `applyBlackout_locked()` also stops cue list + FX; /net/blackout defers on busy lock (202) |
| 48ae2e9 | 1-H1 | ap_pass 8–63 printable or 400, empty ap_ssid 400; bad stored pass repaired at boot; softAP() refusal → MAC SSID + build pass, `ap_fb` in /status; status buffer 800→1024 |
| 4c456ea | 2-H4, 1-L9 | /artnet/set → `pendingSockRefresh`, serviced in loop(); rebind only on STA link edges |
| 601d02e | 1-M1, 1-L5, 1-L6 | cue step ≥ 50 ms, clamp at set time, same-cue retry after a busy lock |
| ac0d3cd | 1-M6, 1-M9, 1-L2 | non-blocking STA join; parked STA with 30→300 s back-off while the AP is up and has no client; hostname before mode() |
| 718a6eb | 1-M8, 1-L4 | loop task on the TWDT (5 s, panic), bounded panic/blackout waits, `rst` + `fw` (git SHA) in /status |
| 7bcfc17 | 1-L3 | espressif32@7.0.1, AsyncTCP@3.3.2, ESPAsyncWebServer@3.6.0; registry esp_dmx dropped |
| 02675dc | 1-L7, 1-L8 | beacon reply only on a real add; loadFixtures repairs names/ranges |

## Open

- 1-L1, 1-L10 (OSC), 1-L11 (Art OUT) — inside worktree B's scope (Art-Net/sACN/OSC); marked Open in the doc.
  1-M7 (OSC floats) is OSC parsing too, left to B and not annotated. 1-C1, 1-M2/M3/M4 are B's.
- 1-M6 partly: no separate DMX task before WiFi; reset → first DMX frame time is unmeasured.
- 1-M8 timeout is 5 s, not the audit's ~2 s (a global `esp_task_wdt_init(2)` also shortens the IDLE0 check). Tighten after measuring loop() period on the node.
- CURRENT.md not updated here (avoid a conflict with worktree B); add one line at merge.

## Numbers

- Build `pio run -e esp32dev`: SUCCESS, RAM 16.1 % (52 772 B), Flash 70.9 % (929 521 B of 1 310 720).
  Base 6e26c94 size was not measured; the esp_dmx switch alone took −128 B.
- Native tests: 4 → **11 test cases, 11 succeeded** (new: WPA pass-phrase, host label, IPv4, cue timing, elapsed/wrap, STA back-off, channel range).

## Flash-and-verify on the real node (owed)

1. **DMX output first**: the patched `lib/esp_dmx` is now linked instead of the registry copy (1-L3). Fixture responds, scope/analyser if at hand: break ≥ 92 µs, MAB ≥ 12 µs, ~43 Hz (ANSI E1.11).
2. `/status` shows `fw` = the flashed SHA, `rst`, `ap_fb:false`; JSON parses.
3. 1-C2: WebSocket open + loop `/node/set?name=<31 chars>` and `/artout/peer?ip=…` for minutes — no reboot, DMX steady; `/artout/peer?ip=foo` → 400.
4. 1-H2: cue list running, press Blackout — stays black past fade+dwell; /net/blackout under load → 204/202, never 503 on the lock.
5. 1-H1: `/node/set?ap_pass=abc` → 400, `ap_pass=` → 400; nothing changes.
6. 2-H4: sACN flowing, change universe via /artnet/set repeatedly — no crash, new universe received.
7. 1-M6/1-M9: STA creds for an absent router, reboot: time reset → first DMX frame (expect ≈ 1 s, was ≥ 10 s); phone on the AP stays connected for 10 min (serial: "STA: join failed (n), next try in … s").
8. 1-L2: router lease shows the node name, not `esp32-XXXXXX`.
9. 1-M8: soak 1 h, `rst` must not read `task_wdt`.
# Handover — protocol pass of audit 2026-10-15 (branch fix/audit-b)

Worktree `/home/dob/work/vizzz.di-fix-b`, base `6e26c94`. Scope: the protocol findings (part 2 §(a)–(d),
part 1 C1/M2/M3/M4/M7, protocol Lows). Locking/String, blackout/cue, AP password, socket refresh
(worktree A) and the HTTP routes (later pass) were not touched. Nothing was flashed; the live node was
not contacted.

## Commits

| commit | findings | what |
|---|---|---|
| `cd70eed` | 2-M10 | Art-Net, sACN, OSC parsing in `include/vizzz_core.h`; 10 new native tests; `tools/check_di_compat.sh` |
| `df638a1` | 1-M7, 1-L1 | OSC floats scaled, not truncated; bundles dispatched |
| `d0eb01b` | 2-H1, 1-M4, 1-C1, 2-M1, 2-H3, 1-M2, 2-M2, 1-M3, 2-L1, 2-L2, 2-L8, 2-L9 (declared), 1-L11 (part) | firmware wiring |
| `ab465c4` | 2-M9 (part) | simulator `--udp` parity; `tools/net_probe.py` |
| this commit | — | audit doc marks, AGENTS.md, this file |

`d0eb01b` is one commit for twelve findings because they share one function-body hunk in `main.cpp`
(pollArtNet/pollSacn/sendArtNetOut); intermediate states would not have built correctly.

## Done (17) / open (6)

Fixed: 2-H1, 2-H3, 2-M1, 2-M2, 2-M10, 2-L1, 2-L2, 2-L8, 2-L9 (declared in Status3), 1-C1, 1-M2, 1-M3,
1-M4, 1-M7, 1-L1, 2-M9 (part), 1-L11 (part).
Open, each with its reason in `docs/AUDIT_2026_10_15.md`: 2-M9 rest (HTTP: ACAO, /panic, /fixture/set,
/discover), 2-M11 and 2-M12 (di side), 1-L9 (socket refresh, worktree A), 1-L10 (scene/cue path),
1-L11 rest (send Art OUT before master/ceiling: changes what a slave shows; the owner decides).

## Measured

- `pio run -e esp32dev`: SUCCESS. RAM 55 380 B (16.9 %) vs 52 740 B at `6e26c94` (+2 640 B: two Art-Net
  merge slots, two sACN source slots, Art OUT last-frame copy). Flash 929 953 B (70.9 %) vs 927 125 B (+2 828 B).
- `pio test -e native`: **14 test cases: 14 succeeded** (was 4).
- `tools/check_di_compat.sh /home/dob/work/di.iiii`: PASS — di's 14-byte ArtPoll detected; di's ArtDmx
  (desk U1) parsed as port-address 0; di's sACN (desk U1) parsed as universe 1 = the node's sACN universe;
  the node's 239-byte ArtPollReply listed by di's `ArtNet.nodeList()` with name and universe.
- `tools/net_probe.py verify` against `simulate_node.py --udp` on 127.0.0.1: 13/13 PASS.

Not proven: anything on the ESP32 itself. The two-source Art-Net merge and the two-node latch rule are
covered by native tests only (one computer = one source IP; the latch needs two nodes with Art OUT on).

## Flash-and-verify step (owed; run from aylmo, after the merge with worktree A)

```
cd ~/work/vizzz.di                                    # merged branch
PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio test -e native     # 14/14
tools/check_di_compat.sh ../di.iiii                   # di compat: PASS
sg dialout -c "PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio run -e esp32dev -t upload"
python3 tools/net_probe.py poll   --node 192.168.88.66            # one ArtPollReply, decoded
python3 tools/net_probe.py verify --node 192.168.88.66 --ch 64    # expect "13/13 checks passed"
```

Conditions for `verify`: node in MERGE_HTP or ARTNET_ONLY, channel 64 at 0 in the web layer, Art OUT
off, di's desk output off and di's desk not bound to 6454 on aylmo while polling (the reply is unicast
to port 6454; a second socket on that port may take it). The check drives only channel 64 and sets it
back to 0. Then the owner's look: di's desk lists the node as discovered (age shown, universe 1), and
desk U1 moves the light over Art-Net and over sACN.

## Review (AAR)

- Meant: fix the protocol findings with spec-cited methods, tests growing, build green, no hardware.
- Happened: as meant; 2 rework rounds (a simulator patch script failed its own assertion and was rerun;
  no code rework). ~45 tool calls.
- Lesson, made durable: protocol code now lives in the tested core and is cross-checked against di's own
  packet code by `tools/check_di_compat.sh` (AGENTS.md "Adding Unit Tests"), so a di/firmware mismatch
  like 2-H3 shows up without a node.

# Handover — HTTP API / console pass of audit 2026-10-15 (branch fix/audit-c)

Worktree `/home/dob/work/vizzz.di-fix-c`, base `6245e2a` (A + B merged). Scope: part 2 H2, M3–M9 and the
API Lows. Nothing flashed, no serial, the live node not contacted. Each finding in
`docs/AUDIT_2026_10_15.md` carries its Fixed/Open line.

## Commits

| commit | findings | what |
|---|---|---|
| `9372087` | 2-H2, 2-L3 (part), 2-L5, 2-L7 | `vizzz::kRoutes` + `route()` gate: Console = POST + `X-Vizzz-Console: 1`; console JS sends it; /discover read-only + POST /announce; /ws Origin check |
| `efeff26` | 2-M5, 2-L6, 2-L3 | `parseUintArg/parseIntArg` + `ArgReader`, 400 JSON; start ≤ end; SSID/pass lengths |
| `aea72d3` | 2-M3 | `escAttrJs` in onclick; `tools/check_console_escape.js` |
| `1781443` | 2-M7 | ACAO only on GetShared reads, from the table |
| `af68558` | 2-M4 | manifest routes + route_table from the table, git SHA, net/sub/uni; `tools/check_routes.py`; README table; onboard/crash tools send POST + header; check_di_compat.sh relative-path fix |
| `c790f15` | 2-L5 | beacon route renamed `/announce` (prefix-swallow by `/discover`); guard test + check |
| `0cdb7b7` | 2-M9 (HTTP rest) | simulator reads the same table and gate; `tools/check_http_api.sh` |

## Done (13) / open (5)

Fixed: 2-H2 (console routes), 2-M3, 2-M4, 2-M5, 2-M7, 2-M9 rest, 2-L3, 2-L5, 2-L6, 2-L7; verified as A's:
2-M6 (48ae2e9), 2-L4 (d17912c).
Open: 2-H2 rest and 2-M7 rest — **owner decides** on a real login/token (operator routes stay plain GET for
di; /status keeps `sta_ssid`/`mac` cross-origin); 2-M8 — **owner decides** web-layer clear vs /panic;
2-L10, 2-L11 — di.iiii side.

## Measured

- `pio run -e esp32dev`: SUCCESS. RAM 55 476 B (16.9 %) vs 55 412 B at `6245e2a` (+64 B); Flash 931 301 B vs
  932 441 B (−1 140 B: one gated `server.on` per route instead of GET+POST pairs).
- `pio test -e native`: **27 test cases: 27 succeeded** (was 21): route kinds, gate decisions, WS origin,
  strict uint/int parsing, ACAO rule.
- `tools/check_di_compat.sh ../di.iiii`: PASS, now including `check_routes.py`: 57 routes in table = 57
  registered = 57 in README; di's `/status /set /master /blackout` all plain-GET reachable.
- `tools/check_http_api.sh` against `tools/simulate_node.py`: 24/24. `node tools/check_console_escape.js`: PASS.

Not proven: anything on the ESP32 (ESPAsyncWebServer 3.6.0's handling of `HTTP_ANY`, OPTIONS, and form
bodies is read from its source, not observed).

## Flash-and-verify step (owed; after merge, on aylmo)

```
cd ~/work/vizzz.di
PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio test -e native      # 27/27
tools/check_di_compat.sh ../di.iiii                                             # PASS
sg dialout -c "PLATFORMIO_CORE_DIR=$PWD/.platformio-core ~/.local/bin/pio run -e esp32dev -t upload"
tools/check_http_api.sh http://192.168.88.66        # expect "24 passed, 0 failed"
```

What the script runs (harmless: channel 64 → 0, master and Art OUT re-set to their current values):

```
N=http://192.168.88.66
curl -s -o /dev/null -w '%{http_code}\n' $N/reboot                                   # 405 (GET on POST-only)
curl -s -D - -o /dev/null $N/reboot | grep -i '^allow'                               # Allow: POST
curl -s -o /dev/null -w '%{http_code}\n' -X POST $N/reboot                           # 403 (no header)
curl -s -o /dev/null -w '%{http_code}\n' -X OPTIONS -H 'Origin: http://evil.example' $N/reboot   # 403, no ACAO
curl -s -o /dev/null -w '%{http_code}\n' -X POST -H 'X-Vizzz-Console: 1' "$N/artout/set?en=0"   # 204 (use the current ao)
curl -s -o /dev/null -w '%{http_code}\n' "$N/set?ch=64&v=0"                          # 204 (di's plain GET)
curl -s -o /dev/null -w '%{http_code}\n' "$N/set?ch=abc&v=255"                       # 400
curl -s -o /dev/null -w '%{http_code}\n' "$N/set?ch=900&v=0"                         # 400
curl -s -D - -o /dev/null $N/status | grep -i access-control-allow-origin            # present: *
curl -s -D - -o /dev/null -X POST $N/reboot | grep -ci access-control-allow-origin   # 0 (absent)
```

Then the owner's look: the console (Network page: Wi-Fi scan → pick → Join; Save node; Reboot) still
works from the phone and laptop, and di.iiii's DMX Out still moves channels, master and blackout.
If the console's buttons do nothing after the flash, the first suspect is the browser cache serving the
old page (it sent GET): reload once.

## Review (AAR)

- Meant: fix part 2's HTTP findings by established method, tests growing, build green, no hardware.
- Happened: as meant, 6 commits, ~62 tool calls. Rework: 3 rounds, all in my own patch scripts (nested
  triple quotes; a pattern with leading spaces; README pipes split by the checker), none in firmware code.
- Found on the way: `tools/check_di_compat.sh ../di.iiii` (the documented relative form) failed before this
  pass — `di_side.js` `require()`d a path relative to itself. Fixed in af68558.
- Caught in self-review before hand-off: `/discover/announce` would have been answered by the `/discover`
  handler (ESPAsyncWebServer serves `<uri>/...` with the `<uri>` handler), i.e. 405. Renamed `/announce`;
  a native test and `check_routes.py` now fail on any route that is another's prefix + "/".
- Lesson, made durable: the route list exists once (`vizzz::kRoutes`) and `tools/check_routes.py` fails the
  di compat check on drift, so 2-M4's "manifest out of date" cannot come back silently (AGENTS.md
  "Adding a New HTTP Route" rewritten around it).

---

# Handover — USB widget input (branch feat/usb-enttec-pro), 2026-10-15

The node speaks the ENTTEC DMX USB PRO widget protocol ("ENTTEC DMX USB PRO API":
labels 3, 4, 6, 10) on UART0, so di.iiii's light desk drives it over the USB cable with
its existing `enttec` driver. README "USB" has the protocol table, baud and modes table.

| Commit | What |
|---|---|
| `e35f760` | `vizzz::widget` parser (resync, 600-byte cap), label 3/10 reply builders; tests 27 → 35 |
| `2038ef2` | firmware: `pollUsb()` bounded 2048 B/pass, RX buffer 4096, `usbVals` under gLock, `usbActive()` 2.5 s (E1.31 §6.7.1), HTP with Art-Net/sACN via pure `vizzz::merge`; `/status` `usb_active` `usb_frames` `usb_baud`; manifest `usb`; simulator mirrors; tests → 36 |
| `7678493` | `tools/usb_probe.js` (di's own driver), `tools/widget_pty_harness.cpp` (parser on a pty) |

Measured (aylmo, no board): esp32dev RAM 55,476 → 57,748 B (17.6 %), Flash 931,285 →
933,169 B (71.2 %); native 36/36; check_routes, check_di_compat, check_http_api (simulator,
24/0) PASS. Pty loopback: di's `enttec.js` opened the pty at 250000, sent 119 frames in 3 s,
0 dropped; the firmware parser took 119 label-6 frames, 0 errors, ch64 = 180;
`usb_probe.js params` 4/4 PASS with 170 interleaved log bytes skipped.
**Not measured: anything on the real board** (UART timing at 250000, RX overruns under
Wi-Fi load, DTR/RTS reset on port open). Live node before flashing: `fw` cacd490, no `usb_baud`.

**Baud change for every log reader.** UART0 now runs at 250000 (`USB_WIDGET_BAUD`).
`pio device monitor` picks it up from `platformio.ini` (`monitor_speed = 250000`); any
other reader (screen, `stty`-based bootlog readers, tools/provision_sta.sh's note) must use
250000. The ROM bootloader lines before the app starts stay at 115200.

## USB flash-and-verify

Run on aylmo with the board on `/dev/ttyUSB0`, di's desk output NOT on enttec (the probe
needs the port to itself), `pio device monitor` closed.

```
cd ~/work/vizzz.di-usb
PLATFORMIO_CORE_DIR=/home/dob/work/vizzz.di/.platformio-core ~/.local/bin/pio test -e native   # 36 test cases: 36 succeeded
sg dialout -c "PLATFORMIO_CORE_DIR=/home/dob/work/vizzz.di/.platformio-core ~/.local/bin/pio run -e esp32dev -t upload"
curl -s http://192.168.88.66/status | python3 -m json.tool | grep -E '"fw"|usb_'     # fw = this SHA; usb_baud 250000
sg dialout -c "node tools/usb_probe.js params --di /home/dob/work/di.iiii --port /dev/ttyUSB0"
#   expect: label 3 reply (firmware 1.0, break 16-17 x10.67us, MAB 1, rate 40), label 10 serial 8 digits; "4 passed, 0 failed"
sg dialout -c "node tools/usb_probe.js --di /home/dob/work/di.iiii --port /dev/ttyUSB0 --node http://192.168.88.66 --ch 64 --v 180 --seconds 5"
#   expect: 8 PASS, "0 failed": reachable, usb_baud present, host baud 250000 == node, node stayed
#   reachable (no DTR/RTS reset), usb_active true, usb_frames grew (~200 for 5 s at 40 Hz),
#   out at ch 64 == 180 (MERGE_HTP, master 255, ceiling 255; the probe prints the formula), released to web
```

Conditions: mode MERGE_HTP or ARTNET_ONLY (WEB_ONLY ignores USB by design and the out check
then expects the web value), channel 64 at 0 in the web layer, burn-safe off or the ceiling is
taken into the expectation. Then the owner's look: in di's light desk set output driver
`enttec`, serial port `/dev/ttyUSB0`, move a fader — the fixture follows; close the desk —
the rig returns to the web layer within 2.5 s.

Owed if a check fails:
- "node stayed reachable" FAIL → opening the port resets the ESP32 through the DTR/RTS
  auto-reset transistors. Fix belongs in di's driver (`stty -hupcl`, DTR/RTS handling) or a
  hardware cut; write it as a ledger row, do not patch around it in firmware.
- usb_frames grows slower than the driver's `sent` → RX overruns; measure `loop()` pass time
  and raise `USB_RX_BUFFER` / `USB_BYTES_PER_PASS`.

## Review (AAR)

- Meant: widget protocol on UART0 driven by di's driver, merged HTP, proof tool. Happened:
  built and tested on host; board steps above owed (no flashing in this job by rule).
- Lesson made durable: the merge rule was inline in `computeOutput_locked` and untestable;
  it now lives in `vizzz::merge` with a modes test and a static_assert to `enum Mode`.
- Simulator `--serial` mode: skipped — not cheap (needs a C++-equivalent parser in Python
  and a pty); the pty harness covers the parser against di's real driver instead.
- Nothing left behind: everything is in this repo (`vizzz::widget`, `pollUsb`, probe, harness);
  no production name in code.
