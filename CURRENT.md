# Current State

Read this before changing the firmware. Keep it short and replace stale facts.

## Last verified baseline

`4f94223` - `fix: use local platformio core dir`
Branch: `main`

## What works

- ESP32 firmware builds with project-local PlatformIO core data.
- Native Unity tests cover `vidili_core.h` helpers.
- DMX output uses `DMX_NUM_1`, GPIO25 TX, GPIO21 DIR.
- Browser console exposes control, patch, scenes, network, system, and VJ routes.
- WebSocket `/ws` pushes status roughly every 400 ms.
- Art-Net and sACN input share the configured universe.
- Art-Net OUT can broadcast the final output for receiver nodes.
- Firmware node manifest is served at `/node/manifest` and `/manifest.json`.

## Hard rules

- Do not edit `lib/esp_dmx/`.
- Do not change DMX pins, UART, or `DMX_PERIOD_MS` without hardware approval.
- Do not hold `gLock` across DMX writes or blocking calls.
- Keep embedded UI in the black/cyan square `di.i`-style visual system.
- Do not push to GitHub unless validation passes.

## Required validation

```bash
/home/nnn/.platformio/penv/bin/pio test -e native
/home/nnn/.platformio/penv/bin/pio run -e esp32dev
```

## Known issues

- `sg dialout` upload may need elevated shell permission in agent sandboxes.
- Starship may warn about read-only cache; this is not a firmware failure.
