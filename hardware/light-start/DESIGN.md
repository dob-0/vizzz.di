# Light Start — circuit design

v0.1 2026-11-16, studio-mac. Device 1 of the line (`../ARTIST_BOARDS.md` section 5).
Status: **parts decided, stock read live; schematic not drawn yet.** Nothing ordered.

Every LCSC part below was read with `python3 -I hardware/tools/lcsc_check.py` on 2026-11-16
(stock and price at quantity 10 from LCSC's product page). Re-run before any order:
`python3 -I hardware/tools/lcsc_check.py --from hardware/light-start/DESIGN.md`.

Blocks: POWER + USB + CORE + LIGHT (DMX, pixels, status). Not in Start, on purpose: isolation, RDM,
Ethernet / PoE, pixel power through the board — those come with Light Pro.

## POWER — 12–24 V in, 5 V and 3.3 V

| Ref | Part | LCSC | Stock 11-16 | $ @10 | Note |
|---|---|---|---|---|---|
| J1 | DC in, 2-pin 5.08 mm terminal, KF128-5.08-2P-AA | C474952 | 53,840 | 0.19 | 12–24 V nominal |
| F1 | Resettable fuse, hold ≥ 1 A, rated ≥ 60 V | to pick | — | — | in stock ≥ 1,000 |
| D1 | Reverse-polarity Schottky SS34 (40 V, 3 A) | C7420365 | 2,703,660 | 0.04 | series, after F1 |
| D2 | TVS SMBJ33A (stand-off 33 V) | C499719 | 2,960 | 0.09 | clamp 53.3 V at Ipp (SMBJ table, unchecked this turn) < 63 V buck max |
| U1 | Buck 5 V, **LMR51635YFDDCR** (TI, 4.3–60 V in, 3.5 A, sync, 1.1 MHz FPWM) | C44750836 | 5,082 | 1.55 | replaces TPS54202 (28 V max — too close to 24 V) |
| L1, R, C | Inductor, feedback divider, BOOT cap, input / output caps | to pick | — | — | values from TI datasheet SLUSF64, cited per part |
| U2 | 5 V power mux, TPS2116DRL (buck first, USB second) | to pick | — | — | fallback: two Schottkys, buck set to 5.3 V |
| U3 | LDO 3.3 V, AMS1117-3.3 (1 A) | C6186 | 898,640 | 0.20 | about 0.85 W at 0.5 A: copper area at layout; Pro moves to a buck |

Runner-up buck: TPS54360DDAR (C44377, 22,798 in stock, $0.56, non-synchronous, needs a 60 V
Schottky). Checked and rejected: LM5164DDAR (C477928) — 0 in stock; TPS54202 — 28 V max.

## USB-C — power, flashing, USB-MIDI

| Ref | Part | LCSC | Stock 11-16 | $ @10 | Note |
|---|---|---|---|---|---|
| J2 | USB-C HRO TYPE-C-31-M-12 | C165948 | 506,515 | 0.19 | KiCad footprint `USB_C_Receptacle_HRO_TYPE-C-31-M-12` |
| R | CC1, CC2 5.1 kΩ to GND (2×) | C23186 | 13,716,300 | 0.002 | sink at 5 V |
| U4 | USB ESD USBLC6-2SC6 (ST) | C7519 | 34,460 | 0.16 | on D+, D−, VBUS |

## CORE — ESP32-S3

| Ref | Part | LCSC | Stock 11-16 | $ @10 | Note |
|---|---|---|---|---|---|
| U5 | ESP32-S3-WROOM-1-N16R8 (certified radio) | C2913202 | 14,403 | 4.75 | KiCad symbol and footprint in stock libraries |
| C | 10 µF 0603 CL10A106KP8NNNC | C19702 | 1,937,240 | 0.02 | 3V3 bulk |
| C | 100 nF 0603 CC0603KRX7R9BB104 | C14663 | 51,200 | 0.006 | one per supply pin |
| R, C | EN: 10 kΩ pull-up + 1 µF | C25804, C15849 | 457,400 / 40,300 | — | power-up delay |
| SW1, SW2 | BOOT (GPIO0) and RESET (EN), TS-1187A-B-A-B | C318884 | 532,700 | 0.02 | |

GPIO map — every pin to be checked against the ESP32-S3-WROOM-1 datasheet pin table before drawing:

| Signal | GPIO | Why |
|---|---|---|
| DMX TX → U6 DI | 17 | UART1, 250 kbit/s 8N2 with break |
| DMX RX ← U6 RO | 18 | for RDM in Pro, same pins |
| DMX DE + /RE (joined) | 21 | 100 kΩ pull-down: driver off during reset and boot |
| Pixel 1–4 | 4, 5, 6, 7 | RMT capable, not strapping |
| Status LED data | 38 | DevKitC-1 v1.1 uses 38; 48 avoided (VDD_SPI domain doubt, unchecked) |
| USB D− / D+ | 19 / 20 | native USB, no UART chip |
| BOOT | 0 | button to GND |
| Debug UART0 TX / RX | 43 / 44 | 3-pin header, not fitted |

Avoided: strapping GPIO3, 45, 46; octal flash and PSRAM GPIO26–37.

## LIGHT — DMX out (one universe, not isolated)

| Ref | Part | LCSC | Stock 11-16 | $ @10 | Note |
|---|---|---|---|---|---|
| U6 | RS-485 SP3485EN-L/TR (MaxLinear), 3.3 V | C8963 | 191,296 | 0.24 | life status (NRND?) unchecked; ships today |
| D3 | RS-485 TVS SM712 (MDD) | C502564 | 255,460 | 0.07 | −7 / +12 V, on A and B |
| JP1 | 120 Ω termination, solder jumper, **open** | — | — | — | closed for RDM (Pro) |
| R | Fail-safe bias 2 × 560 Ω | — | — | — | footprints, not fitted (RDM later) |
| J3 | XLR-5 **female**, horizontal, Neutrik NC5FAH | not on LCSC | — | — | hand-fitted; KiCad `Jack_XLR_Neutrik_NC5FAH_Horizontal` |
| J4 | XLR-3 female, horizontal, Neutrik NC3FAH1, in parallel | C368476 | 1,258 | 1.93 | for 3-pin fixtures |

DMX pins (ANSI E1.11, read from secondary sources: [Wikipedia DMX512](https://en.wikipedia.org/wiki/DMX512)):
pin 1 signal common, pin 2 Data− (B), pin 3 Data+ (A), pins 4 and 5 not connected.
The standard puts a **female** socket on a transmitter and names XLR-5; XLR-3 is not in the standard
but is on most fixtures, so Start carries both.

Note on the DIY node: `docs/BUILD_A_NODE.md` uses a 3-pin **male** socket for DMX out. It works and is
left as it is; the boards follow the standard.

Checked and rejected: NC5FAV (C368512) — 25 in stock and vertical.

## LIGHT — pixels and status LED

| Ref | Part | LCSC | Stock 11-16 | $ @10 | Note |
|---|---|---|---|---|---|
| U7 | Octal buffer 3.3 V → 5 V, SN74AHCT541PWR (TTL inputs) | C50989 | 106,870 | 0.35 | 4 pixel lines + status + 3 spare inputs to GND |
| R | 330 Ω series, 4 × 0603 | C23138 | 3,100 | 0.002 | at the buffer outputs |
| D4 | ESD SRV05-4 (TECH PUBLIC), 4 data lines | C558418 | 489,840 | 0.03 | VCC pin to +5 V |
| J5–J8 | Pixel out, 2-pin pluggable (DATA, GND) + plug | to pick | — | — | pixels powered from their own supply; common GND |
| LED1 | SK6812MINI-E status LED (shows di cyan) | C5149201 | 183,285 | 0.08 | on +5 V, data from U7 |

- Checked and rejected, stock 0 on 2026-11-16: SN74AHCT125PWR (C36365), 74AHCT125PW (C148198).
- Checked and rejected, stock 0 on 2026-11-16: SN74AHCT1G125DBVR (C7484), WS2812B-2020 (C965555).
- Checked and rejected, 898 in stock on 2026-11-16: SN74HCT541DWR (C352960).

(The checker skips lines that say "rejected", so keep each rejected part on such a line.)

## Still open

1. F1, L1 and the buck passives, U2, the pixel plugs: picked when the schematic is drawn, each with
   LCSC stock ≥ 1,000 and a datasheet citation.
2. NC5FAH sourcing and price (Neutrik distributor), and whether NC5FAH has a newer replacement part.
3. SP3485 life status.
