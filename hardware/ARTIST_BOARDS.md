# vizzz artist boards — plan

Draft 2026-11-16, studio-mac. Status: **plan only — no circuit drawn, nothing ordered.**

## 1. What the owner asked (2026-11-16, his words)

- "i want to work on the pcb and the lighthing devices"
- "i want to create pcb's with our base and backgrund" → asked which: our circuits and firmware,
  or the di look on the board — answer: "both"
- "we have now some diy, but i want to design board which is suite to multidisiplinary artist"
- Jobs: Light, Sound and sync, Sensors and motion, One cable network — "i want to all separate and
  also the mix of the functions, and also the swizz knife of the visual artist"
- Shape: "i want to all case but not all at once" (small box, bare board, rail/rack — in steps)
- "i want to totally custom board"

Open source, for other artists too (owner 2026-11-10: artists get tools to create).

## 2. What we start from (checked 2026-11-16)

- **Firmware:** this repo, branch `int/2026-10-15` (f76567e) — Art-Net / sACN in, DMX out, browser
  console, the di desk UI served from flash. Written for the classic ESP32.
- **Proven DMX wiring:** `docs/BUILD_A_NODE.md` — GPIO25 → MAX485 DI, GPIO21 → DE + RE, A → XLR 3,
  B → XLR 2, GND → XLR 1. Not isolated, no protection.
- **Boards:** the soldered perfboard node BA6FCC and node BC5A64 (state from notes of 2026-10-15,
  not checked live).
- **Fixtures to test with:** 2 × Cameo Thunder Wash 100 RGB; the di.iiii fixture library has 13 types
  (dev b9bc26405).
- **PCB files:** none — 0 `hardware/`, .kicad_pcb, .step, .stl or .3mf files on all 18 branches.

## 3. Prior art we study (never copy)

- [LuxDMX](https://github.com/tombueng/LuxDMX) (Apache-2.0) — ESP32-S3, W5500 wired Ethernet,
  2 isolated DMX outs on XLR-5, PoE, 4-layer board. Closest to our Light board.
- [Miauwceiver](https://www.hackster.io/news/miauwceiver-syncs-stage-lighting-and-diy-robotics-f42b2ecc32fa)
  — ESP32 + CRMX radio; Art-Net, sACN, OSC, MIDI; GPIO for robotics.
- [MoonLight](https://github.com/MoonModules/MoonLight) (GPL-3.0) — firmware for DMX + LEDs, best on
  ESP32-S3 / P4; asks for MIDI help.
- [SparkFun ESP32 DMX to LED shield](https://github.com/sparkfun/SparkFun_ESP32_Thing_Plus_DMX_to_LED_Shield)
  — isolated DMX (Eagle files; licence not stated in GitHub's metadata).
- [DFRobot EDGE102-DMX512](https://www.cnx-software.com/2024/11/20/dfrobot-edge102-dmx512-lighting-controller-combines-esp32-s3-mcu-and-dmx512-protocol-chip/)
  — closed, but shows the market: ESP32-S3, relay, 8 isolated inputs.

None of them does light + sound + sensors + motion on one board. That gap is ours.

## 4. The family: one set of blocks, many boards

Every job is **one circuit block**, drawn once (a KiCad hierarchical sheet), tested once on a real
board, then reused in every board that needs it. A fix in a block reaches every board.

Blocks (first draft — parts are candidates, not checked against stock or price):

- **CORE** — ESP32-S3 module (certified radio), USB-C (power, flashing, USB-MIDI), 12–24 V in →
  5 V / 3.3 V, status LEDs, reset + boot buttons.
- **LIGHT** — 2 × DMX512 out, galvanically isolated RS-485 with surge protection, XLR;
  4 × LED-pixel outputs (5 V level shift, fused).
- **SYNC** — MIDI in / out, LTC timecode in, trigger in / out.
- **MOTION** — 8 protected sensor / switch inputs, 4 servo outputs, 2 motor or relay drivers.
- **NET** — wired Ethernet with power over the same cable (PoE 802.3af).

Boards, in steps ("not all at once"):

1. **Light** = CORE + LIGHT — from our base: the DMX we already run. Bare board + small box.
2. **Light + NET** — the one-cable show version.
3. **Sync** = CORE + SYNC.
4. **Motion** = CORE + MOTION.
5. **Mixes** — the combinations artists ask for most (chosen after 1–4 are proven).
6. **Swiss knife** — every block on one board, for the visual artist. Candidate brain:
   ESP32-P4 (camera, display, LED panels) + a radio chip — unchecked, decided at step 6.

Shapes follow the same steps: bare board and small box first, rail / rack later.

**Decided 2026-11-16 (owner: "a") — how "mix" works: A.**

- **A (chosen)** — every board is one PCB built from the shared blocks. Strongest at shows
  (no connectors between boards), one more PCB per mix.
- B (not chosen, kept as a later mode) — one CORE board + stackable block boards on our own
  connector. Any mix by stacking, but connectors are the weak point under vibration and current.

## 5. The di look on the board

From di-brand `SYSTEM.md` (fetched 2026-11-16): black ground, one accent `#4df9ff` that only marks
meaning, square corners.

- Black solder mask, white silkscreen, square board corners, the di mark in the silkscreen.
- Cyan cannot be printed by most fabs (white / black silkscreen only — unchecked), so the accent
  lives where it means something: the status LED and the case.
- Frame is di; the inside stays vizzz (signature rule).

## 6. Tools and method (advised)

- **KiCad 10** — pick. Arch `extra` has 10.0.8 (45 MiB). Runner-up: atopile (code-first, writes
  KiCad layouts; young tool). Undo: remove the package.
- Every check by script, in this repo: ERC, DRC, factory files (Gerber, drill, BOM, placement),
  3D pictures for the owner before any order. (`kicad-cli` coverage on 10.x: unchecked.)
- Factory with assembly: JLCPCB or PCBWay (prices unchecked).
- Each block: schematic → review → one prototype → measured on the bench → only then reused.

## 7. What is needed (money and hardware)

- Prototype runs: about 5 boards per design with assembly — price unchecked.
- Bench: multimeter, USB logic analyser (DMX timing), a DMX tester or our Cameo lamps.
- 3D printer for the small box (Bambu Studio named 2026-09-16; printer on hand: unchecked).

## 8. Still the owner's to decide

1. ~~Mix: A or B~~ — A, decided 2026-11-16 (section 4).
2. Licence — this repo has none today. Advice: CERN-OHL-S-2.0 for the boards.
3. Funding: if NLnet funds this, its rule bans generative AI in funded work (memory 2026-11-16) —
   decide before applying.
