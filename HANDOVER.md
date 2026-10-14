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
