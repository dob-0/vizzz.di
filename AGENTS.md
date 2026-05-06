# vizzz.di — AI Agent Context

Quick-reference for AI coding agents resuming work on this codebase. Read this before making changes.

---

## Project in One Sentence

ESP32 firmware (Arduino / PlatformIO) that outputs DMX512 via a MAX485 transceiver, accepts Art-Net and sACN over WiFi, exposes a browser UI + WebSocket for real-time control, and publishes a firmware-node manifest.

---

## Repo Layout

```
src/main.cpp          – entire firmware (single-file)
include/vidili_core.h – pure C++ helpers (no Arduino deps), unit-tested
lib/esp_dmx/          – vendored esp_dmx library (do not modify)
test/test_smoke/      – Unity tests for vidili_core.h, run on native platform
platformio.ini        – two envs: esp32dev (hardware), native (unit tests)
.vscode/tasks.json    – Build / Upload / Test tasks
README.md             – user-facing docs
CURRENT.md            – short current-state checkpoint; update it to avoid duplicate AI work
AGENTS.md             – this file
```

---

## Session Memory — Prevent Duplicate Work

Use `CURRENT.md` as the living project checkpoint. At the end of each AI session, after any context compaction/resume, and whenever a durable lesson or smart project point appears, add or update a short fact there so future agents do not redo discovery or implementation.

Promote only long-lived rules, hardware constraints, workflow rules, or contracts into `AGENTS.md`. Keep recent status, completed work, validation results, blockers, and next-step breadcrumbs in `CURRENT.md`.

Before starting implementation, read `CURRENT.md` and `AGENTS.md`, then treat recorded completed work as already done unless the user explicitly asks to revisit it. Replace stale facts instead of piling up duplicate notes.

---

## Hardware Constraints — Never Violate These

| Constraint | Detail |
|---|---|
| Use `DMX_NUM_1` (UART2) | `DMX_NUM_0` (UART0) shares pins with USB-Serial → silent failure |
| DMX period ≥ 23 ms | Full 512-ch frame at 250 kbaud = 22.7 ms; shorter period drops frames |
| Always call `dmx_wait_sent()` after `dmx_write()` | Without it the next write may corrupt the outgoing frame |
| GPIO25 = TX, GPIO21 = DIR | MAX485 DE+RE bridged to GPIO21 |

---

## Key Constants (`src/main.cpp`)

```cpp
static constexpr int  DMX_TX           = 25;
static constexpr int  DMX_DIR          = 21;
static constexpr int  MAX_CH           = 512;
static constexpr uint32_t DMX_PERIOD_MS      = 23;
static constexpr TickType_t DMX_SEND_WAIT    = pdMS_TO_TICKS(30);
static constexpr uint32_t ARTNET_TIMEOUT_MS  = 3000;
static constexpr uint16_t ARTNET_PORT        = 6454;
static constexpr uint32_t SACN_TIMEOUT_MS    = 3000;
static constexpr uint16_t SACN_PORT          = 5568;
static constexpr uint8_t  SCENE_COUNT        = 8;
```

AP: `10.0.0.1`, SSID: `vizzz.di`, generated prefix `vizzz.di_`, pass: `Poghka888$`
Art-Net OUT broadcast target: `10.0.0.255:6454`

---

## Thread Safety — Mandatory

Two FreeRTOS cores run concurrently:

- **Core 0**: AsyncWebServer callbacks, WebSocket push
- **Core 1**: `loop()` — DMX write, Art-Net read, fade engine

All access to `webVals[512]`, `outVals[512]`, and fade state **must** hold `gLock`.

```cpp
// Correct pattern for any route that reads/writes webVals:
if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
    request->send(503, "text/plain", "busy");
    return;
}
// ... read/write webVals ...
xSemaphoreGive(gLock);
```

Never hold gLock across a `dmx_write()` or any blocking call.

---

## Adding a New HTTP Route

1. Find `void setupWeb()` in `src/main.cpp`
2. Add before the 404 handler (always the last entry)
3. Follow the mutex pattern above for any route touching `webVals`
4. Return JSON or plain text; use `snprintf` into a stack buffer (avoid `String`)
5. No route should block longer than ~20 ms

Example skeleton:

```cpp
server.on("/my/route", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
        request->send(503, "text/plain", "busy");
        return;
    }
    // work with webVals here
    xSemaphoreGive(gLock);
    request->send(200, "text/plain", "ok");
});
```

---

## JSON Helpers (already in main.cpp)

```cpp
// Escape a String for embedding in a JSON string value.
static void jsonEsc(char* dst, size_t n, const String& src);
```

Use `snprintf(buf, sizeof(buf), "...")` for fixed JSON responses — avoids heap fragmentation from repeated `String` concatenation.

---

## UI Style Rules — Mandatory

The embedded UI is intentionally aligned with the local `di.i` design language:

- black base: `#000` / `#0a0a0a`
- cyan-only primary accent: `#4df9ff`
- square corners: `border-radius: 0`
- dense operational panels, monospace labels, and the hollow-square `□` motif
- no beige theme, gradients, rounded pills, drop-shadow cards, or decorative blobs

Keep all UI work inside the `APP_HTML` block unless the task explicitly adds a new firmware API.

---

## Node Manifest Contract

The firmware node manifest lives at both:

```text
GET /node/manifest
GET /manifest.json
```

It must keep `schema: "vi_di_li.node.manifest.v1"` and `product: "vizzz.di"` and include identity, firmware tag, network state, DMX hardware pins, protocol capabilities, routes, and sync/source-control policy. Keep it machine-readable JSON and update `README.md` if fields materially change.

---

## Adding Unit Tests

Tests live in `test/test_smoke/test_main.cpp` and only test code in `include/vidili_core.h`.

Rules:
- `vidili_core.h` must have **zero** Arduino / ESP-IDF includes
- Tests use the `#if defined(ARDUINO) / #else` dual-main pattern (already in place)
- Run: `~/.platformio/penv/bin/pio test -e native`

If you add a new pure helper (math, protocol packing, etc.), put it in `vidili_core.h` and add a test case.

---

## Build & Upload Commands

```bash
# Build only
~/.platformio/penv/bin/pio run -e esp32dev

# Upload (Linux: dialout group not active in agent shells)
sg dialout -c "~/.platformio/penv/bin/pio run --target upload"

# Serial monitor
sg dialout -c "~/.platformio/penv/bin/pio device monitor"

# Unit tests (no hardware)
~/.platformio/penv/bin/pio test -e native
```

PlatformIO is **not** on `$PATH` — always use the full path above.

---

## Global State Overview

```
webVals[512]   – values set via web UI / HTTP API  (0-based index)
artVals[512]   – values received from Art-Net IN
sacnVals[512]  – values received from sACN IN
outVals[512]   – final mixed output (HTP merge or web-only or artnet-only)
holdVals[512]  – last Art-Net values before timeout
sceneBuf[512]  – scratch buffer for scene recall
dmxFrame[513]  – byte 0 unused (DMX start code), bytes 1-512 = outVals
```

All arrays are 0-based internally; DMX channel N maps to index N-1 in webVals but to index N in dmxFrame.

Fade engine: `fadeActive`, `fadeStartMs`, `fadeTimeMs`, `fadeFrom[512]`, `fadeTo[512]` — all guarded by gLock.

---

## Art-Net Universe Packing

```
universe = (artNet << 8) | (artSubnet << 4) | (artUni & 0x0F)
```

Implemented as `vidili::packUniverse(artNet, artSubnet, artUni)` in `include/vidili_core.h`.

---

## Master Dimmer

Applied to every channel on the output path:

```
outVal = (value * master) >> 8
```

Implemented as `vidili::applyMaster(value, master)` in `include/vidili_core.h`.  
`master=255` → full, `master=0` → black. Note: `255*255>>8 = 254`, not 255 — this is correct per DMX convention.

---

## Verify, Commit, Push

For normal AI/code changes:

1. Read `CURRENT.md` and this file first.
2. Do not touch `lib/esp_dmx/`.
3. Run:

```bash
/home/nnn/.platformio/penv/bin/pio test -e native
/home/nnn/.platformio/penv/bin/pio run -e esp32dev
```

4. Commit verified changes with a clear message.
5. Push `main` to `origin/main` only if validation passes and `git status --short` shows no unrelated edits.

If validation fails, do not push.

---

## Current Resource Budget

| Resource | Used | Free | Headroom for features |
|---|---|---|---|
| RAM | 53.9 KB (16.4%) | ~274 KB | Large — room for OTA buffers, fixture maps |
| Flash | 888.8 KB (67.8%) | ~422 KB | Moderate — limit large HTML additions |

---

## Feature Backlog (priority order)

1. **OTA update** — `ArduinoOTA` or `ElegantOTA`; eliminates USB cable for future flashes. High value.
2. **Fixture groups** — name channels/ranges, recall by group name via API
3. **Cue list** — ordered list of scenes with auto-advance and time
4. **Second WiFi / LAN2** — route Art-Net from a wired Ethernet shield

---

## Common Pitfalls

| Symptom | Likely cause |
|---|---|
| No DMX output, no error | Using DMX_NUM_0 (UART0) — change to DMX_NUM_1 |
| Lights flicker every other frame | DMX_PERIOD_MS too small (< 23) |
| Web UI hangs | Mutex held too long in loop(); check for blocking calls under gLock |
| Native tests fail to compile | `include/vidili_core.h` accidentally pulled in an Arduino header |
| Upload fails with permission error | Run with `sg dialout -c "..."` wrapper |
| `pio` / `platformio` command not found | Use `~/.platformio/penv/bin/pio` |
