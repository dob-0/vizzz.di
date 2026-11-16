# vizzz artist boards — plan

v2 2026-11-16, studio-mac. Status: **plan only — no circuit drawn, nothing ordered.**

## 1. What the owner asked (2026-11-16, his words)

- "i want to work on the pcb and the lighthing devices"
- "i want to create pcb's with our base and backgrund" → asked which: our circuits and firmware,
  or the di look on the board — answer: "both"
- "we have now some diy, but i want to design board which is suite to multidisiplinary artist"
- Jobs: Light, Sound and sync, Sensors and motion, One cable network — "i want to all separate and
  also the mix of the functions, and also the swizz knife of the visual artist"
- Shape: "i want to all case but not all at once" (small box, bare board, rail/rack — in steps)
- "i want to totally custom board"
- How mix works: "a" (each mix is its own board — section 4)
- "with the chips we can have the custom chips so we can desging it fully" → bare chips + FPGA in the
  Monster, our own chip later: "yes" (section 6)
- "and realy monster where we kill all light devices in marked" (section 7)
- "we need to pro devices and the tiers i mean we not work on only one device we need to full line
  of it" (section 5)

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

Every device in the line (section 5) is a set of these blocks.

**Decided 2026-11-16 (owner: "a") — how "mix" works: A.**

- **A (chosen)** — every board is one PCB built from the shared blocks. Strongest at shows
  (no connectors between boards), one more PCB per mix.
- B (not chosen, kept as a later mode) — one CORE board + stackable block boards on our own
  connector. Any mix by stacking, but connectors are the weak point under vibration and current.

## 5. The full line — three tiers for every job

Working tier names, from the owner's words ("pro", "monster"):

- **Start** — an artist's first device. Bare board or small box, USB-C + WiFi, certified radio module.
- **Pro** — shows and touring. Every port isolated and protected, RDM, wired network + PoE,
  metal box or DIN rail.
- **Monster** — the flagship that beats the market (section 7). Bare chips + FPGA, rack.

The line — 12 devices, each its own PCB (choice A). Where two tiers can share one PCB with fewer
parts fitted, they will: fewer designs to prove.

- **Light** — Start · Pro · Monster
- **Sync** (sound, MIDI, timecode) — Start · Pro · Monster
- **Motion** (sensors, servos, relays) — Start · Pro · Monster
- **Swiss knife** (every job, for the visual artist) — Start · Pro · Monster

Build order ("not all at once") — each step proves the blocks the next one reuses:

1. Light Start — CORE + LIGHT from our base (the DMX we already run).
2. Light Pro — adds isolation, RDM and NET (PoE).
3. Sync Start, then Sync Pro.
4. Motion Start, then Motion Pro.
5. Swiss knife Start and Pro — the proven blocks together.
6. The Monster line — Light Monster first (FPGA), then Sync, Motion, Swiss knife.
7. Our own chip — rides on the Monster (section 6).

Shapes follow the tiers: Start = bare board + small box; Pro = rugged box + DIN rail; Monster = rack.

## 6. Chips — designed fully, in three steps (owner: "yes")

1. **Bare chips on our board** (Start, Pro) — every chip placed by us. One exception, the radio:
   a certified ESP32-S3 module, because our own radio layout needs its own tests. Estimates conflict:
   [$2–4k for CE RED](https://fridaynight-bif.pages.dev/esp32/09-certification-manufacturing.html) up to
   [$15–50k more than a module](https://pamfinds.pages.dev/guides/esp32-module-vs-bare-chip/).
   A bare ESP32-S3 follows
   [Espressif's hardware design guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html).
2. **FPGA** (Monster) — logic we write ourselves, so 24+ DMX ports and 48+ pixel ports run in exact
   time together. Candidates with an open toolchain: Lattice iCE40 / ECP5 with Yosys + nextpnr — unchecked.
3. **Our own silicon** (later, an art chip) — open shuttles:
   [wafer.space GF180 Run 3](https://www.crowdsupply.com/wafer-space/gf180mcu-run-3) lists $2,000–8,000
   per slot for 1,000 dies; [Tiny Tapeout](https://tinytapeout.com/) prices by calculator. Dates and
   turnaround unchecked.

## 7. Monster targets — beat every number on the market

From `MARKET_2026-11-16.md` (17 products; maker datasheets for 7, the others marked in the file):

- **DMX:** more than 24 ports (ENTTEC Storm 24), each one isolated (Luminex LumiNode 4), RDM on all.
- **Pixels:** more than 48 outputs (Falcon F48v5), more than 1,360 pixels per output (ENTTEC Octo MK2),
  faster than 100 fps (ENTTEC Pixelator Mini MK2).
- **I/O:** at least 8 relays and 8 × 0–10 V inputs (Visual Productions IoCore2).
- **Timecode:** LTC in + out, MTC, Art-Net timecode, RTP-MIDI (Visual Productions TimeCore).
- **Network:** 2 × gigabit and PoE (Advatek T16X-S Mk3, LumiNode 4, Pharos LPC X).
- **Open:** hardware and firmware both open — today only Falcon links its firmware source.
- **Physics check owed:** a D-size XLR flange is about 26 mm wide (unchecked), so about 17 fit in one
  1U row of a 19" front (≈ 483 mm). Storm 24 uses RJ45 for its 24 DMX ports. XLR on 2U or RJ45 on 1U
  is measured at step 6, never guessed.

## 8. The di look on the board

From di-brand `SYSTEM.md` (fetched 2026-11-16): black ground, one accent `#4df9ff` that only marks
meaning, square corners.

- Black solder mask, white silkscreen, square board corners, the di mark in the silkscreen.
- Cyan cannot be printed by most fabs (white / black silkscreen only — unchecked), so the accent
  lives where it means something: the status LED and the case.
- Frame is di; the inside stays vizzz (signature rule).

## 9. Tools and method (advised)

- **KiCad 10** — pick; 10.0.8 installed on studio-mac 2026-11-16 (pacman). Runner-up: atopile (code-first, writes
  KiCad layouts; young tool). Undo: remove the package.
- Every check by script, in this repo: ERC, DRC, factory files (Gerber, drill, BOM, placement),
  3D pictures for the owner before any order. (`kicad-cli` coverage on 10.x: unchecked.)
- Factory with assembly: JLCPCB or PCBWay (prices unchecked).
- Each block: schematic → review → one prototype → measured on the bench → only then reused.

## 10. What is needed (money and hardware)

- Prototype runs: about 5 boards per design with assembly — price unchecked.
- Bench: multimeter, USB logic analyser (DMX timing), a DMX tester or our Cameo lamps.
- 3D printer for the small box (Bambu Studio named 2026-09-16; printer on hand: unchecked).

## 11. Still the owner's to decide

1. ~~Mix: A or B~~ — A, decided 2026-11-16 (section 4).
2. Licence — this repo has none today. Advice: CERN-OHL-S-2.0 for the boards.
3. Funding: if NLnet funds this, its rule bans generative AI in funded work (memory 2026-11-16) —
   decide before applying.
4. Tier names — Start / Pro / Monster are working names from the owner's words; he renames any time.
