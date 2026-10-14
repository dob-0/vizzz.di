# di's light desk as the vizzz.di node console — plan (no code changed)

Owner decision: the ESP32 node serves **di.iiii's own desk UI** (the one at /light), not a restyle. One UI, one source.
Written 2026-10-15, read-only. Sources read: di.iiii `serverXR/src/lighting/ui/*`, `desk.js` (route table l.1161–2210),
`routes/lightingRoutes.js`; vizzz.di `int/2026-10-15` @ a03222b: README, HANDOVER, `include/vizzz_core.h` (`kRoutes`), `src/main.cpp`.
di checkout is on `chore/claude-setup-audit-2026-10-11`, but `ui/` is byte-identical to `origin/dev` 84fc193c, so the sizes below hold for dev.
Live sizes are GETs on the installed di (`diiii.localhost/light/api/*`, show "moxir": 241 fixtures, 5 universes). The raw replies
are in this scratchpad as `api_*.json`.

## 1. Inventory — what the desk UI calls

How it talks to the server: everything is relative `fetch('api/...')`, so it works under any mount. `post()` sends JSON
(`content-type: application/json`) and returns `{error}` on failure. There is no WebSocket and no EventSource in the UI today:
it polls. Pages are Setup / Control / Touch / Fader / MIDI (hash routes).

| Path (method) | Used by panel | Reply size (live) | Rate |
|---|---|---|---|
| `api/state` GET | everything: `renderAll` (header, patch grid, scenes, FX, layers, output, status strip) | 150 KB (9.5 KB gz). Fixtures 127 KB = 525 B each | every 1.5 s (`loopState`), plus after almost every action (~30 call sites); paused while the tab is hidden |
| `api/dmx` GET | stage view, selection list, attribute faders, Fader page | 5.6 KB (5 universes) | **every 100 ms** (10 Hz) |
| `api/cues` GET | cue strip, Touch cue bar, Control scenes visibility | 0.7 KB | 1 s |
| `api/input` GET / POST, `api/input/release` | Setup › Input (`inDetails`, `inLines`) | 0.8 KB | 1 s on Setup only |
| `api/master` POST `{master}` / `{blackout}` | master fader, blackout (×4 call sites) | <100 B | on gesture |
| `api/fixture` POST `{id,values,on}` | attribute faders, colour, pan/tilt | <200 B | leading edge, then batched every 25–60 ms during a drag |
| `api/fixtures/all` POST | MIDI "all dim" | <100 B | MIDI-driven |
| `api/raw` POST `{universe,channel,value}` / `{channels[]}` / `{clear}` | Fader page (`faderBank`), held pill | <1 KB | on drag |
| `api/fixtures/add · remove · move · readdress · identify · limits` POST | Setup › patch (`patchGrid`, stage drag, identify, limits) | <1 KB | on action |
| `api/groups/add · remove` POST | groups | <200 B | on action |
| `api/scenes/save · recall · update · remove` POST | Control › scenes (`ctlScenes`, `bankList`, `sceneDetail`), Touch, MIDI scene grid | <1 KB | on action |
| `api/chase` POST (×7) | Control › chase (`ctlChase`) | <200 B | on action |
| `api/fx` POST (×11) | FX pads, spatial, exclude, depth, BPM | <200 B | on action |
| `api/lfos` POST | LFO list | <1 KB | on action |
| `api/audio` POST (fire-and-forget) / `api/audiocfg` POST | mic meters (`amLevel`…): needs `getUserMedia` | <100 B | **20 Hz while the mic is on** |
| `api/looks` GET; `api/looks/add · remove · capture · fire` POST | looks (`lookList`, `touchLooks`) | 12.8 KB | once on load, then on action |
| `api/layer`, `api/layers/add · remove` POST | layers (`layerList`, `tLayerList`) | <200 B | on drag |
| `api/sets` POST | set player (`setPlayerMain`, `setList`) | <1 KB | on action |
| `api/fan` POST | fan values across the selection | <200 B | on action |
| `api/cues/go · back · stop · loop` POST | cue strip, Touch cue bar | 0.7 KB | on action |
| `api/output` POST (×6), `api/output/send`, `api/output/send/remove` | Setup › Output (`artnetOut`, `sendList`, `serialOut`) | <500 B | on action |
| `api/nodes/add · remove`, `api/discover` POST | Setup › nodes (`nodes`, `pollResult`) | <1 KB | on action |
| `api/serial/identify` POST | Setup › USB (`serialStat`) | <100 B | on action |
| `api/profiles/add · remove` POST | profile builder (`builder*`) | <2 KB | on action |
| `api/library` GET, `library/manufacturer` GET, `library/fixture` GET, `library/import` POST | fixture library (`oflPane`, `libTree`) | 6.8 KB to 100s of KB | on demand |
| `api/midi` GET / POST | MIDI page (`miMon`, bindings): needs `requestMIDIAccess` | 20 B | on load / on bind |
| `api/show` GET; `api/show/open`, `show/copy-machine` POST | show note (`showNote`), which space owns the show | 134 B | on load |

In total the UI makes **59 distinct method+path calls**. desk.js serves about 75; the UI does not call `summary`, `clock`,
`scenes/summary`, `fireable`, `layers` GET, `rig*`, `cues/load`, `scenes/replace` or `dmx/stream`.
The UI also has 6 absolute links `href="/spaces…"` (index.html), and 3 `getUserMedia` plus 3 `requestMIDIAccess` uses, which only
work in a secure context.

## 2. Classification for a node

**(a) Must really exist on the node: 23 calls.** `state` GET, `dmx` GET, `master`, `fixture`, `fixtures/all`, `raw`,
`fixtures/add · remove · move · readdress · identify`, `scenes/save · recall · update · remove`, `groups/add · remove`, `fx`,
`chase`, `output`, `input` GET + POST, `input/release`.
How each maps onto what the node has: levels go to the 512-channel web layer (`/set`); master and blackout to `/master` and
`/blackout`; scenes to the 8 NVS slots (`/scene/save`, `/scene/recall` with fade); patch to `fixtures[]` (12 × start/end/x/y,
`/fixture/set`); groups to `groups[]` (8); chase to the node's cue runner (≤16 steps of scene + dwell + fade, which is closer to
di's chase than to di's cue list); FX to the node's FX engine (the UI paints its pads from `S.fxModes`, so the node lists its own
modes); output to Art-Net OUT and the peer target; input to the source mode (WEB / ARTNET / HTP, fallback; Art-Net, sACN and USB
as sources); DMX view to the web layer, or the merged output frame.
**Model gap.** di fixtures carry a *profile* and per-role values (dimmer, r, g, b…), and di scenes are per-fixture values. Node
fixtures are bare channel ranges, and node scenes are 512-byte snapshots. The bridge needs a `profile` per node fixture. Fixture
values are then *read from and written to the web layer through the profile's channel map*, so no second store exists. Scene
fixture values are derived from the snapshot at read time.

**(b) Static or refusing answer: 28 calls.** `cues` GET → `{cues:{n:0}}` (with n=0 the cue strip hides itself) and
`cues/go · back · stop · loop`; `show` GET → `{space:null,label:<node name>}`; `looks` GET → `{looks:[],kinds,scopes}` and
`looks/add · remove · capture · fire`; `layer`, `layers/add · remove`; `lfos`; `sets`; `fan`; `fixtures/limits`;
`profiles/add · remove` (the node has a fixed profile set); `output/send`, `output/send/remove` (one output); `nodes/add · remove`;
`discover` (answered from the node's `/peers`); `serial/identify`; `audio`, `audiocfg`. State keys that go with these are sent as
empty values of the right type (`looks:[]`, `layers:[]`, `lfos:[]`, `sets:[]`, `raw:{}`, `audioCfg:{…}`, `status.nodes:[]`…).
Writes answer `{error:"not on this node"}`.

**(c) Not applicable on a node: 8 calls.** `library`, `library/manufacturer`, `library/fixture`, `library/import` (Open Fixture
Library: needs the internet and a disk cache); `midi` GET + POST (WebMIDI is refused on an http page); `show/open` and
`show/copy-machine` (di spaces' show files). These answer 404 with a JSON body.

**What the UI does today when a call fails** (read from app.js, not run):
- `api/state` gives 404 or non-JSON: the UI counts poll failures, and after 2 (≈3 s) the strip says *"server offline — is node
  server.js still running?"*. That wording is wrong on a node.
- `api/state` answers but **a key is missing**: `renderAll` dereferences `S.status.*` (23 places), `S.output.*` (30), `S.fx.*` (25),
  `S.chase.*` (13) and `S.lfos/layers/groups` without guards, so it throws. The throw lands in `pullState`'s catch. `pollFails` had
  already been set to 0 by the successful fetch, so it only goes 0→1 and back each cycle: **no banner, half-painted page, silent
  failure**. This is a di bug in its own right, and it is why (b) must send a complete shape.
- `library` fails: shows *"needs the internet the first time"* (misleading on a node). `midi` fails: *"cannot reach the desk"*
  (misleading). `looks`, `show`, `cues`, `dmx`, `input` fail silently, and the state poll reports.
- POST refusals come back as `{error}`; most callers `say()` it in the status strip, a few ignore it.
- Mic and MIDI fail on any http origin (W3C Secure Contexts). The UI already writes a note in index.html for this but does not hide.

## 3. Two build routes, with numbers

Flash, measured with `gzip -9 -c | wc -c` on the real files:

| File | raw | gz -9 |
|---|---|---|
| app.js | 310,853 | 96,508 |
| style.css | 86,433 | 21,274 |
| index.html | 50,143 | 13,124 |
| qr.js / from.js / show.js | 7,572 / 4,849 / 4,291 | 3,107 / 2,120 / 1,822 |
| **total** | 464,141 | **137,955** |

- Today: 933,169 of 1,310,720 B used, so **377,551 B are free**.
- The gzipped UI takes 137,955 B (36.5 % of the free space).
- The old console (`APP_HTML`, stored raw, 53,839 B) goes away, so the net change is **+84,116 B**. That leaves ≈1,017,285 used,
  77.6 %, **≈293 KB free**, before the API bridge.
- The bridge plus ArduinoJson are estimated at 20–40 KB (unmeasured), so the expected total is about 80 % used and ≈250 KB free.
- Minifying before gzip would cut more. That is not measured.
- The default esp32dev partition keeps OTA with two 1.25 MB slots. A UI that outgrows it would force a partition change that drops
  OTA (`huge_app`), so the build gets a free-flash gate.

RAM, for /api/state built at node scale (real di fixture/profile JSON, short names, 8 profiles, 8 scenes, one universe):

| Fixtures | /api/state | gz | of which scenes |
|---|---|---|---|
| 12 (today's MAX_FIXTURES) | 22.3 KB | 2.4 KB | 11.2 KB |
| 32 | 47.0 KB | 2.9 KB | 28.8 KB |
| 64 | 86.5 KB | 3.8 KB | 56.9 KB |

- `/api/dmx` worst case is 2.8 KB.
- Static RAM today is 55,476 B (16.9 % of 327,680). Free heap and the largest free block at runtime are **not measured**: /status
  does not report them.
- Serving gz from PROGMEM (`beginResponse_P` + `Content-Encoding: gzip`) streams from flash with no heap copy.
- /api/state must be **streamed** (chunked / `AsyncResponseStream`), never built as one `String`: String growth doubles, so 47 KB
  can peak near 94 KB.
- The scenes part is cached as text, the same as di's `sceneJsonCache`. Scene snapshots are kept in RAM, 8 × 512 = 4 KB, so the
  scenes are not read from NVS on every poll.

**R1: the node serves di's files from PROGMEM and implements the (a) subset.**
- What changes in di: none.
- The node answers all 59 calls: 23 real, 28 static, 8 at 404.
- Flash and RAM: as above.
- Cost: the 8 (c) panels and many (b) panels stay visible with misleading text. The silent-throw bug above bites whenever the node's state shape lags di's. Every di UI change that adds a state key can break the node silently.
- **It fails "fails silently" and "nothing left behind"**: the knowledge of what a node can do lives only in firmware.

**R2: di's UI gets a node profile.**
- The desk state carries `caps` (for example `{library, midi, looks, layers, lfos, sets, cues, profilesEdit, multiUniverse,
  spaces, stream, node}`).
- A missing `caps` means everything is on, so the di desk is unchanged.
- Panels whose cap is false are hidden. Mic and MIDI are gated on `window.isSecureContext`, which is feature detection, not a flag
  (MDN "Feature detection").
- The node lists the same caps in `/node/manifest`.

As written in the brief, R2 says "the node serves exactly that build". If that means a *separate node build* of the UI, it is a
fork, and fork means drift. The professional route is **R1 + R2 with runtime caps, one bundle**:
- The node serves byte-identical di files at a pinned commit and implements the contract subset.
- di's single UI degrades by capability.
- `tools/fetch_desk_ui.py` in vizzz.di takes `git archive` of the pinned di commit and checks sha256 against `desk_ui.lock`.
- It gzips with `-9 -n` (deterministic, no timestamp) and generates `include/desk_ui_gz.h` as a PlatformIO `extra_scripts`
  pre-build step.
- No hand copies. A di bump is one PR that changes the lock and runs the contract test.
- Why this one: one source (the UI is never edited in vizzz.di), no drift (pin plus contract test), and failure is visible (caps
  plus the render-error fix).
- Method sources:
  - RFC 9110 §8.4 Content-Encoding
  - ESPAsyncWebServer README, "Serving static files … gzip"
  - reproducible builds (reproducible-builds.org, `gzip -n`)
  - capability negotiation as in HTTP `OPTIONS`/feature flags
  - W3C Secure Contexts and Mixed Content

## 4. Work breakdown (PR-sized; agent-hours; model)

**di.iiii**
- **D1, honest render plus caps.** `renderAll` errors are counted apart from network errors and shown as "the desk sent a state this page cannot draw: <key>". Read `S.caps` (default all on) and hide panels by cap. `isSecureContext` gates mic and MIDI. The offline wording comes from caps (desk vs node). Test: Playwright on `<tree>.diiii.localhost/light/` with mocked `api/state`, (i) caps absent and (ii) node caps. Screenshots at 390×844 DPR 3 and 1440×900. Zero console errors. `npm run test:http` stays green. 4 h. Sonnet builds, Opus reviews.
- **D2, desk API contract.** `serverXR/src/lighting/api-contract.json`: every route, class a/b/c, the minimum reply shape, and the state keys the UI dereferences. `tests/test-http.js` accepts `DESK_URL` and `--class a`, so the same checks run against any server. Test: green against the standalone desk. A lint fails if app.js calls a path that is not in the contract. 3 h. Sonnet.
- **D3, stream transport.** When `caps.stream`, the UI takes DMX from `EventSource('api/dmx/stream')` (dmxstream.js already exists in di) instead of the 10 Hz poll, with the poll as fallback. Test: test-stream green, and a Playwright count of frames per second and requests per second before and after. 3 h. Sonnet.
- **D4, bundle export.** `npm run desk:bundle` writes `ui/*`, `manifest.json` (di commit, sha256 per file, contract version) and `profiles-node.json` (the built-in profile subset, taken from di's PROFILES so the node's profiles have one source). Test: two runs give identical hashes. 2 h. Sonnet.
- **D5, "This node" section (caps.node).** Covers the node functions di has no panel for: Wi-Fi (scan/set/forget, netmode), source mode and fallback, Art-Net port-address, burn-safe, peers and fleet, panic, reboot, factory reset. It calls the node's existing console routes with `X-Vizzz-Console: 1`. Generic hardware-node words, no product name in defaults (rule "nothing left behind"). Test: Playwright against `simulate_node.py`. 5 h. Opus designs, Sonnet builds.

**vizzz.di**
- **V0, measure first.** Add `heap_free`, `heap_max_block` and `ws_clients` to `/status`, plus a native test for the keys. Then a baseline on the real node. 1 h. Sonnet.
- **V1, build script.** `fetch_desk_ui.py`, `desk_ui.lock`, the pre-build header and a free-flash gate that fails under 128 KB free. Test: native test that the header sizes match the manifest; `check_routes.py`. 3 h. Sonnet.
- **V2, static serving.** `/` serves index.html. The files are sent with gzip, `ETag` = sha and `Cache-Control: no-cache`, so a repeat load answers 304. Remove `APP_HTML`. `/control /patch /scenes /vj` redirect 301 to `/#…`. Test: `check_http_api.sh` gets 6 file checks, a gzip header check, a 304 check and the redirects; flash measured. 2 h. Sonnet.
- **V3, bridge core in `vizzz_core.h`** (pure, native-testable). Profile channel maps, fixture↔web-layer mapping, scene derivation from snapshots, and a streaming JSON writer. Test: native tests check the shape against `api-contract.json` (D2). 8 h. Opus designs, Sonnet builds.
- **V4, `/api/*` routes in main.cpp.** 23 real, 28 static or refusing, 8 at 404-JSON. JSON bodies parsed with ArduinoJson pinned in `lib_deps`. Gate: POST requires `content-type: application/json` *and* a neutral header the di UI's `post()` always sends (for example `X-Desk-Client: 1`, added in D1), which keeps the preflight CSRF guard of today's console routes (OWASP CSRF Cheat Sheet, "custom request headers"). `kRoutes` gains a `DeskApi` kind; `check_routes.py` is extended. Test: di's `test-http.js --class a DESK_URL=http://127.0.0.1:<sim>`. 8 h. Sonnet, with Opus review.
- **V5, SSE `/api/dmx/stream`** via AsyncEventSource, at 10–20 Hz, with a client cap. Test: measured frame rate and heap with 1, 3 and 5 clients. 2 h. Sonnet.
- **V6, NVS migration.** The fixtures blob becomes v2 under a versioned key: profile id, MAX_FIXTURES 12→32 (+≈0.5 KB RAM). The v1 blob is migrated once and kept for rollback. Test: native migration test. 3 h. Sonnet.
- **V7, simulator parity and the real surface.** `simulate_node.py` reads the same contract. Playwright screenshots (phone and desktop) through `di-test-browser` on the agent screen. Then the real node, a hardware session: flash, phone on the AP, look at every page, measure heap, requests per second, and the fader→DMX latency against the old console. 3 h plus the hardware session.

Order: V0 → D1, D2 → V1, V2, V3 → V4 → D3, V5 → D4 → D5, V6 → V7.
Total ≈ 47 agent-hours: ~80 % Sonnet, Opus for the D1/D5/V3/V4 design and review.

**Stays owed after v1** (ledger rows):
- di's looks, layers, LFOs, sets and cue list on the node.
- Per-fixture limits.
- The fixture library on the node. The node patches only from the exported profile set; di can push a patch over the API.
- Mic and MIDI on the node: they need HTTPS on the node, and a self-signed certificate means phone warnings, so not v1.
- Proof on the owner's phone.

## 5. Risks

1. **Silent half-render (top risk).** Any shape gap between the node's state and di's UI throws in `renderAll` with no banner (§2). It hits on every di bump. Mitigation: D1 render-error honesty, the D2 contract test in vizzz.di CI, and V1's lock.
2. **Polling load.** Per client the UI makes ≈12.7 requests/s: dmx 10, state 0.67, cues 1, input 1 on Setup. Three phones make ≈38 requests/s on one AsyncTCP/lwIP stack (few sockets, and keep-alive behaviour not measured). Mitigation: SSE (D3/V5) takes the 10 Hz dmx poll away. Measure in V7 before claiming it holds.
3. **Heap.** /api/state is 22 KB at 12 fixtures and 47 KB at 32. With concurrent clients and a fragmented heap it can fail. Mitigation: a streaming writer, the scenes text cache, V0's numbers, and a hard reply cap with a clear error.
4. **Universes.** The UI assumes a list (`st.universes`, `fixture.universe`, `DMX[u]`, raw keys `u:ch`, Fader universe tabs). The node is one universe. It sends `universes:[0]` and refuses any other universe with a clear error. Its Art-Net port-address is node config (D5), not a desk universe.
5. **https/http.** A phone on the node's AP opens `http://192.168.4.1/`, which is same-origin, so it is fine. di's https pages (dev.diiii.xyz) cannot fetch the node: Mixed Content, and Chrome's Private Network Access. di reaches the node by Art-Net from its server, never from the browser. Mic and MIDI are lost on http (hidden by D1).
6. **Model mismatch.** Node fixtures without a profile, 512-byte scenes, and node FX modes that differ from di's. The mapping in V3 must be exact or scene editing on the phone lies about what will play.
7. **Lock-out.** Removing the old console removes Wi-Fi setup until D5 exists. D5 ships before V2 is flashed.

## 6. Questions for the owner (with my recommendation)

1. Remove the old console *and* the VJ deck (`/vj`), with their functions taken by di's Control and Touch pages? **Yes, one UI**:
   301 the old URLs.
2. Do node settings (Wi-Fi, mode, Art-Net address, reboot, factory reset) live in di's desk as a "This node" section shown only for a
   node? **Yes.** Otherwise a phone cannot reconfigure the node.
3. Raise the node's fixture limit from 12 to 32? **32.** /api/state is ≈47 KB, streamed; 64 would be ≈87 KB, too big for v1.
4. v1 scope is levels, master, blackout, scenes, patch (exported profiles), groups, FX, chase, output/input and the DMX view. Do
   looks, layers, LFOs, sets and di's cue list stay owed rows? **Yes.** Each needs di's engine logic ported to C++.

Review (this job): 24 tool calls, Opus. Nothing was started or written outside this scratchpad. Sterile: yes; the only reads were
GETs to the installed di.

## 7. V0 result — heap baseline on the real node (measured 2026-10-15, fw 8168931)

`tools/heap_probe.mjs --node http://<ip> --clients 3 --seconds 30 --every 2000` from aylmo, 15 polls per phase, 0 failed.
Two console tabs were already holding `/ws` (ws_clients 2 at idle); the probe added 3 (5 total), each receiving the 400 ms push (75 in 30 s).

| phase | ws clients | heap_free min / max (B) | heap_max_block min (B) |
|---|---|---|---|
| idle | 2 | 164,848 / 173,288 | 110,580 |
| loaded | 5 | 161,648 / 165,596 | 98,292 (−12,288) |
| after close | 2 | 169,208 / 172,604 | 110,580 (recovered) |

Static RAM 57,748 B (17.6 %), Flash 933,261 B (71.2 %), native 38/38.

**Decision on Q3 (fixture limit): 32, taken at V6 after V3, not before.**
- Static cost of 12→32: `FixtureDef` is 24 B, so +480 B static, +480 B on the stack copy in the fixture snapshot, +~0.5 KB in the NVS blob. Nothing in the table above moves by that.
- What can break is the reply, not the table: `/api/state` ≈ 47 KB at 32 fixtures. One copy fits (98 KB largest block under load, 2× margin); five concurrent copies (5 clients × 47 KB = 235 KB) do not fit in heap at all, and even today's 12 fixtures (5 × 22 KB) would not. So the limit is safe only with the streaming writer (V3) and the hard reply cap, which is why the order stays V3 → V6. With streaming, no reply ever needs a contiguous block near 47 KB.
- Rule for the next measurement (V5): the same probe with 1, 3 and 5 clients on `/api/dmx/stream`; `heap_max_block` under load must stay above 64 KB, else the client cap drops.
