# Desk bridge core (plan step V3)

What this is: the pure, native-testable code that lets the node answer di's class-a desk API
(`docs/DI_DESK_UI_PLAN.md` §2) **without a second store**. It lives in `include/vizzz_desk.h`,
which `include/vizzz_core.h` includes, namespace `vizzz::desk`. No Arduino headers, no heap.
Nothing in `src/main.cpp` uses it yet; V4 wires the `/api/*` routes to it, V6 changes NVS.

Contract it is written against: di.iiii `serverXR/src/lighting/api-contract.json` at commit
`091a1a6` (PR #941, step D2), vendored as `test/fixtures/api-contract.v1.json`
(sha256 `b4365121c8914c3c9be311dbfbd9c0b0a3aef7816676835787e70dded7d7790d`).
Shapes were read from di's `desk.js` (`publicState`, `DEFAULT_STATE`, `GET /api/dmx`, `showInfo`),
`engine.js` (`PROFILES`, `makeFixture`, `captureScene`), `roles.js`, `cuerun.js` (`full`) and
`ui/app.js` (`renderAll` and what it reads) at the same commit.

## 1. Model

| di (desk.js) | node (this header) | where the value really lives |
|---|---|---|
| profile = ordered list of role names (`PROFILES[name].channels`) | `Profile` = name, cat, channel count, ordered `Role[]`, a constexpr table in flash | flash |
| fixture `{id, index, name, universe, address, profile, on, x, y, values, limits}` | `NodeFixture` = `{name[16], start, profile, x, y, enabled}`, 22 B | NVS blob (V6), RAM table |
| `fixture.values[role]` | **not stored**: read from / written to the 512-byte web layer at `start - 1 + offset(role)` | `webVals[512]` |
| scene `{id, name, fadeMs, fixtures:[{id,on,values}], raw, live, missing}` | 512-byte snapshot in one of 8 NVS slots | NVS `scenes/sN` (RAM copy in V4) |
| scene fixture values | derived at read time: the snapshot read through **today's** patch | — |
| group `{id, name, ids[]}` | channel range `{name, start, end, enabled}` | NVS `vj` |
| `master` 0..255, `blackout` | `masterDimmer`, blackout fact from the caller | RAM |

### Profiles (one source)

The node's set is copied, names and role order unchanged, from di's built-in `PROFILES` in
`serverXR/src/lighting/engine.js` @ 091a1a6. Role names are di's (`roles.js` `ROLE_DEFAULTS`),
spelled exactly as the contract and the UI use them. The 13 copied entries:

`dimmer`, `ds`, `drgb`, `drgbs`, `drgbw`, `drgbwa`, `rgb`, `rgba`, `rgbw`, `rgbaw`, `dwwcw`
(cat `_GENERIC`) and `ptdrgb`, `ppttdrgbw` (cat `_MOVING`).

They need 13 roles: `dimmer r g b w a warm cool strobe pan panFine tilt tiltFine`. A profile is
`{name, cat, count, roles[9]}`; the role→offset map is the index in `roles[]` (the same thing
di stores: a channel list), found by a ≤ 9-step scan. All tables are `constexpr` POD with
`const char*` literals, so on the ESP32 they sit in flash (`.rodata`) with no copy at boot.
A test pins every profile against the strings copied from engine.js, so an edit here that drifts
from di fails natively. Plan step D4 (`profiles-node.json`) will replace the hand copy with an
exported one; until then the copy is the one source and the test is its guard.

### Mapping rules

1. **Address.** `address = start` (1-based), `universe = 0`, `end = start + count - 1` (derived,
   never stored). A fixture whose end would pass 512 is refused by `fixtureFits()`; if one is
   found anyway (old blob), roles past 512 are left out of `values` and ignored on write.
2. **Read.** `values[role] = web[start - 1 + offset(role)]`, for the profile's roles only, in profile
   order. The UI builds its attribute faders from `profiles[f.profile].channels` and reads
   `values[role] ?? 0`, so a role a profile lacks is simply absent.
3. **Write.** A `{role, value}` pair writes `web[start - 1 + offset(role)] = clamp(value, 0, 255)`
   when the profile has the role; otherwise it is counted as ignored (`writeValues` returns
   applied and ignored counts). Overlapping fixtures share channels: the last write wins,
   exactly as the cable will show.
4. **Stage position.** di's `x`, `y` are 0..1; the node keeps 0..255. Out: `x/255` printed with
   3 decimals (`0.502`); in: `round(x * 255)`. Every byte round-trips (test).
5. **Identity.** fixture id `f<slot+1>` (`f1`..`f32`), `index = slot + 1`. Only enabled slots are
   listed; a listed fixture is always `on: true` (see §2).
6. **Scenes.** id `s<slot+1>` (`s1`..`s8`), only saved slots listed, name from the caller (NVS
   has none today: `Scene N`), `fadeMs` from the caller (the node's default recall fade).
   `fixtures` = each listed fixture's roles read from the **snapshot** with rule 2.
   `live` = listed fixtures, `missing = 0`, `raw = {}`.
7. **Groups.** id `g<slot+1>`; `ids` = the listed fixtures whose whole channel range lies inside
   the group's range.
8. **Chase.** The node's cue runner: `sceneIds` = the step scene ids in order; `holdMs`/`fadeMs`
   come from the caller (V4 picks step 0's); `enabled` = cue runner running.
9. **Class-b keys** go out as empty values of the right type: `looks: []`, `layers: []`,
   `lfos: []`, `sets: []`, `raw: {}`, `status.nodes: []`, `status.serial: null`,
   `audioCfg: {enabled:false, mode:"level", amount:255, release:300, useBeats:true}`
   (di's `DEFAULT_STATE.audioCfg`). `caps` names what the node has not
   (`midi, audio, library, looks, layers, lfos, sets, cues, profilesEdit, multiUniverse: false`,
   `node: true`); at 091a1a6 the UI gates on `audio` and `midi` only, unknown caps are ignored.
10. **`fixtures/all` and `master`** keep their meaning: `fixtures/all` is rule 3 applied to every
    listed fixture, `master` is `masterDimmer` (applied on the output path as today, not
    written into the web layer).

### The writer

`JsonWriter` formats into a fixed 512-byte buffer inside itself and hands full chunks to a sink
`bool (*)(void* ctx, const char* data, size_t len)`. A sink that returns false stops the writer
(every later call is a no-op). No chunk is longer than 512 bytes, the whole document is never
in one buffer, numbers are integer-formatted (the 0..1 stage position is fixed-point, no `%f`),
strings are escaped (`"` `\` and every byte below 0x20; bytes ≥ 0x80 pass through as UTF-8).

`writeState`, `writeDmx`, `writeCues`, `writeShow`, `writeLooks` take a `DeskFacts` struct of
pointers and values; main.cpp fills it, so main.cpp stays thin.

**Pull adaptor for V4.** ESPAsyncWebServer's chunked response *pulls*
(`(uint8_t* buf, size_t maxLen, size_t index) -> size_t`); the writer *pushes*. `WindowSink`
bridges the two: each pull re-runs the writer, skips the first `index` bytes, copies up to
`maxLen`, then stops the writer. Cost is quadratic in the reply (measured natively in the test
output, see the PR), and it is only correct if every re-run produces the same bytes, so **V4 must
freeze the facts once per request**: copy the web layer (512 B), the fixture table (22 B × n),
the master/blackout/fx/cue facts under `gLock` into a per-request block and run every pull
against that copy. The scene snapshots are already a RAM copy that changes only on save; V4
must either copy the one or two saved slots or bump a version and restart the reply. A changed
fact between pulls would cut a number in half and break the JSON.

## 2. What a node cannot represent (and what di's UI then shows)

- **Fixture `on`.** Node `enabled` only means "slot is patched"; it does not darken the output.
  A listed fixture is always `on: true`. An `{on:false}` from the UI has no store: V4 must
  refuse it (`{error}`) or decide to write the profile's level roles to 0 (lossy). Open (§4).
- **Virtual dimmer.** di gives every fixture `values.dimmer` and scales emitters by it when the
  profile has no dimmer channel. The node has no such value: for `rgb`, `rgba`… no `dimmer`
  key is sent and a `dimmer` write is ignored. The UI shows no dimmer fader for those (it reads
  the profile's channel list).
- **Per-fixture limits, identify flash, custom profiles, universes > 0, LFOs, layers, looks,
  sets, di's cue list, audio, MIDI, library.** Sent as di's defaults or empty (rule 9);
  `limits` is di's `DEFAULT_LIMITS`, `universes: [0]`. Panels follow the D1 caps.
- **Raw holds.** di's `raw` overrides fixtures; on the node every write lands in the one web
  layer, so `raw` is `{}`. A scene's channels that belong to no fixture are played on recall but
  are not visible in the scene's fixture list (scene `raw: {}`).
- **Scene fx/lfos/audioCfg.** Node scenes are levels only; those keys are not sent per scene.
- **Scene against a changed patch.** di keeps per-fixture values and says `missing`; the node's
  snapshot is read through today's patch, which is exactly what recall will put on the cable, so
  `missing` is always 0. Re-patching changes what a stored scene *shows*, never what it plays.
- **Group membership** is derived from ranges: a fixture that straddles a group's edge is not a
  member. `groups/add` with a non-contiguous selection cannot be stored as one range (V4).
- **FX modes.** The node sends its own `fxModes` list (main.cpp `kFxNames`, 10 modes); di's
  `pump breathe pingpong blocks` are not offered.

## 3. Memory budget

Plan §7 (measured on the node, fw 8168931): largest free block **110,580 B idle, 98,292 B with
5 /ws clients**; static RAM 57,748 B.

Measured natively (x86-64 host, `pio test -e native`, test output lines starting `desk:`):

| item | bytes | where |
|---|---|---|
| `kProfiles` (13 × `Profile` 32 B) + `kRoles` (13 × 16 B) | 416 + 208 on the host, in flash on the node | `.rodata` |
| `NodeFixture` | 22 (`static_assert`) | RAM per slot |
| fixture table at 12 / 32 slots | 264 / 704 | static |
| `GroupRange` | 22 (same layout as `GroupDef`) | static |
| `JsonWriter` | 552 on the host (512 B buffer + bookkeeping + 2 pointers) | stack of the caller (async_tcp task) |
| `DeskFacts` | 216 on the host (pointers and scalars; smaller with 32-bit pointers) | stack |
| `WindowSink` | 40 on the host | stack |
| per-request frozen facts (V4) | ≈ 512 + 22 n + 64, *estimate* | heap, one per open reply |
| largest chunk handed to a sink | 512 (`kChunk`), asserted | — |

Reply sizes from the test's synthetic node (short names `F1`…, every profile once then `rgb`,
6 of 8 scene slots saved, 7 groups):

| reply | bytes |
|---|---|
| `api/state`, 32 fixtures | 25,111 in 50 chunks |
| `api/state`, 12 fixtures | 12,147 |
| `api/dmx` (512 channels) | 1,896 |

These are smaller than plan §3's 47.0 / 22.3 KB, which used di's own fixture/profile JSON (longer
names, 8 di profiles, more keys per fixture). Neither is a measurement on the node.

Pull adaptor cost (re-run per pull, measured as bytes formatted for one full reply at 32 fixtures):
1,436-byte pulls → 19 pulls, 274,478 B formatted (×10.9); 5,744-byte pulls → 6 pulls,
108,590 B (×4.3). CPU time on the ESP32 is **not measured** (V4/V7).

MAX_FIXTURES goes 12 → 32 **only at V6** (plan §7 decision), together with the v2 NVS blob.
The esp32dev build size is reported in the PR only if it was run (V3 adds no call from main.cpp,
so the linker drops the code; only the include is compiled).

### V6 migration (done, step V6)

NVS namespace `fixtures`: the v1 blob `items` is `FixtureDef[12]` = `{name[16], start, end, x, y, enabled}`,
24 B each, 288 B (+ `count`, + `prof`, one profile byte per slot). V6 writes a v2 store under new keys:
`ver = 2`, `items2` = `NodeFixture[32]` (704 B), `count2`; the v1 keys are never written again, so
an older build still boots with the patch it had at migration. Boot order: v2 if `ver == 2` and the
blob is 704 B; else a one-time migration of the v1 blob (`migrateV1Blob()` in this header: a stored
`prof` byte wins when its width still matches, otherwise `migrateV1Fixture()`: width =
`end - start + 1`, `profileForWidth()` — 1 dimmer, 2 ds, 3 rgb, 4 drgb, 5 drgbw, 6 drgbwa,
9 ppttdrgbw; any other width keeps the start and becomes `dimmer`), slots 12..31 take the defaults,
v2 is written; else the defaults (4 rgb at 1/4/7/10). The unmapped widths are reported: serial log
and `/status` `fixtures_migrated` / `fixtures_unmapped` (active slots only). `repairV2()` repairs
what NVS returned. The run-time table stays `FixtureDef` + `fixtureProfile[]` (start..end is what
the console routes and the DMX path read); `saveFixtures()` converts it through
`deskFixture_locked()`, so a range edited through `/fixture/set` to a width with no profile is stored
as the profile the desk shows for that width. `MAX_FIXTURES` is `vizzz::desk::kMaxFixtures` = 32.
`fixturesJSON()` (`/fixtures`) took its 768 B copy plus text buffer from the stack; at 32 slots it
needs ~4 KB, so it now mallocs them for the call.

## 4. Decisions taken in V4 (2026-10-15; the cloud run's own text was lost with its sandbox, rewritten by the lead)

1. **`on:false`** is refused with `{error}`: the node has no mute mask, and zeroing the level roles would be lossy.
2. **Scene names and per-scene fade** live in NVS keys `sN.name` and `sN.fade` next to the 512-byte slot, so di's
   `scenes/save` name and fade survive a reboot.
3. **`/api/dmx` shows `outVals`**, what is on the cable (merged, mastered, slew-limited), which is di's meaning of its
   rendered output; the web layer is what `api/fixture` writes.
4. **Freeze per request:** every GET reply allocates ONE block (`DeskBlock`: web layer and outVals copies, the fixture
   table, groups, scene names/fades, the listed scene snapshots, the writer) under `gLock`, releases the lock, and runs
   every chunked pull against that copy; the response owns the block (`shared_ptr` with `free`). No lock is held across
   a pull or a TCP wait. Scene snapshots are copied into the block, so no version counter is needed.
5. **`groups/add` from a non-contiguous selection** is refused with `{error}`; a node group is one channel range.
6. **`fixtures/add` never writes the web layer**; patching a fixture cannot light the rig.
7. **Reply cap:** under 32 KB largest free block (`kMinFreeBlock`) a GET answers 503 `{error:"node busy"}` and counts
   `api_busy`; `api_replies` counts the served ones. Measured on the node (fw defb77e): `/api/state` 3,981 B in 48 ms at
   4 fixtures; largest free block 94,196 B idle, 86,004 B with 3 ws clients, 65–82 KB while a desk page polls at ~6 req/s.
   The windowed-pull CPU cost at 32 fixtures is measured at V7.
8. **CSRF gate:** `Content-Type: application/json` is required on every `/api/*` POST (415 otherwise); di's `post()`
   sends exactly that and no custom header, and a cross-site page cannot send it without a CORS preflight, which the
   DeskApi routes do not answer (no ACAO) — OWASP CSRF cheat sheet, "non-simple requests".

Open for V5–V7: the stream (V5) replaces the 10 Hz `api/dmx` poll; the 0-byte reply seen once after a POST burst
(ledger N686); phone, latency and the pull cost (V7).
