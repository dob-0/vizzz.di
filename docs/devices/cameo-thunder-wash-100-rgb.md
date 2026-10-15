# Cameo THUNDER WASH 100 RGB (CLTW100RGB) — datasheet

Read this before any search about this lamp (owner's rule 2026-10-15 "remember what we have"). One row per fact, with its source and date.
Status words: TESTED = walked on a real unit · ASSUMED = from a chart not yet seen on the unit · CANDIDATE = a maker chart that may not match the unit.

| fact | value | status | source, date |
|---|---|---|---|
| what | 3-in-1 strobe / blinder / wash, 132 × 0.2 W RGB LEDs, strobe 1–30 Hz | — | maker manual (Adam Hall), fetched 2026-10-15 |
| units in the studio | 2, both on the vizzz node's DMX line (node BA6FCC, 192.168.88.66, universe 0) | TESTED (cabled by the owner 2026-10-15 18:3x) | ledger N827, N856 |
| mode on the display | `6Ch2` (lamp one, photos 2026-10-15 15:1x); lamp two: photos owed | TESTED (seen on the display) | ledger N827 |
| 6Ch2 chart | 1 Dimmer · 2 Strobe · 3 Flash duration · 4 Red · 5 Green · 6 Blue | ASSUMED | Open Fixture Library `cameo/thunder-wash-100-rgb` (2020-02-21), community chart; the maker manual edition we hold has no mode 2 |
| 6-CH chart (older firmware, the only one in the manual we hold) | 1 Dimmer · 2 Strobe · 3 Red · 4 Green · 5 Blue · 6 Sound | CANDIDATE | maker manual `376245_manual_multi.pdf` (Thomann copy), "6-CH Mode CLTW100RGB / CLTW600RGB" |
| strobe channel values | 0–5 open (reacts to the dimmer) · 6–10 closed · 11–250 slow→fast 0–30 Hz · 251–255 open | CANDIDATE | maker manual, 2-CH Mode 2 / 3-CH rows; OFL 6ch2 uses the same ranges |
| flash duration | 0–255 = 0–510 ms | CANDIDATE | maker manual, 3-CH Mode 1 row |
| address, lamp one | A001 (set by the owner) | TESTED (display) | ledger N827 |
| address, lamp two | A007 assumed (next free 6-ch block); the owner's photos decide | ASSUMED | ledger N856 |
| node patch | f1 rgb@1 + f2 rgb@4 = lamp one (ch 1–6); f3 rgb@7 + f4 rgb@10 = lamp two (ch 7–12); values 245,0,0,245,0,0 per lamp = dimmer full, strobe open, duration 0, red | TESTED on the node (/api/dmx read-back 2026-10-15 18:42) | node 192.168.88.66 |

## The one check that settles the chart (owner's eye)
Both lamps red → the 6Ch2 chart above is right (mark TESTED). A lamp GREEN → that unit runs the older 6-CH chart (ch 3 = red, ch 4 = green): set its display to the plain `6Ch` chart row and re-patch.
A lamp dark → check the display address (A001 / A007) and that the mode is a 6-channel one; the dimmer and strobe-open values are already sent.

## Owed
- A proper 6-channel profile on the node (`dimmer, strobe, duration, r, g, b`) instead of two 3-channel `rgb` cells per lamp (the node's roles carry no `duration` yet).
- Lamp two's address and mode from the owner's photos; then both rows above go TESTED.
- Manual copy: `docs/devices/` holds no PDF (maker copyright); the Thomann URL is in the source column.
