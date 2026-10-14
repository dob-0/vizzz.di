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
