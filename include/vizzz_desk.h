#pragma once

// ── Desk bridge core (plan step V3, docs/DESK_BRIDGE_CORE.md) ─────────────────
// Lets the node answer di.iiii's desk API without a second store: fixture role
// values are read from and written to the 512-byte web layer through a profile's
// channel list, scene values are derived from a 512-byte snapshot at read time,
// and the replies are streamed in chunks of at most kChunk bytes.
// Contract: di.iiii serverXR/src/lighting/api-contract.json @ 091a1a6, vendored
// as test/fixtures/api-contract.v1.json. Pure code: no Arduino, no heap.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace vizzz {
namespace desk {

constexpr uint16_t kChannels = 512;

// ── Roles ─────────────────────────────────────────────────────────────────────
// The roles the node's profiles use, named exactly as di's roles.js ROLE_DEFAULTS
// (and so as the contract and the UI). `def` is di's ROLE_DEFAULTS value, `kind`
// di's roleKind(): emitter, level, position, fine, control.
// None: a channel the node never drives (gobo, prism, reset, lamp…): it stays at the web layer's 0.
enum class Role : uint8_t { Dimmer, R, G, B, W, A, Warm, Cool, Strobe, Pan, PanFine, Tilt, TiltFine, None, Halo };
constexpr uint8_t kRoleCount = 15;

enum class Kind : uint8_t { Emitter, Level, Position, Fine, Control };
constexpr uint8_t kKindCount = 5;
constexpr const char* kKindNames[kKindCount] = {"emitter", "level", "position", "fine", "control"};

struct RoleInfo {
  const char* name;
  uint8_t def;
  Kind kind;
};

constexpr RoleInfo kRoles[kRoleCount] = {
  {"dimmer", 255, Kind::Level},   {"r", 255, Kind::Emitter},      {"g", 255, Kind::Emitter},
  {"b", 255, Kind::Emitter},      {"w", 0, Kind::Emitter},        {"a", 0, Kind::Emitter},
  {"warm", 255, Kind::Emitter},   {"cool", 0, Kind::Emitter},     {"strobe", 0, Kind::Control},
  {"pan", 128, Kind::Position},   {"panFine", 0, Kind::Fine},     {"tilt", 128, Kind::Position},
  {"tiltFine", 0, Kind::Fine},   {"none", 0, Kind::Control},
  {"halo", 0, Kind::Level},   // the LED ring round a head's lens: its own master; its colour is r g b
};

inline const char* roleName(Role r) { return kRoles[uint8_t(r)].name; }

// Role for a di role name, or -1 (a role no node profile carries).
inline int roleIndex(const char* name) {
  if (!name) return -1;
  for (uint8_t i = 0; i < kRoleCount; i++)
    if (strcmp(kRoles[i].name, name) == 0) return i;
  return -1;
}

// ── Profiles ──────────────────────────────────────────────────────────────────
// Copied, names and channel order unchanged, from di's built-in PROFILES in
// serverXR/src/lighting/engine.js @ 091a1a6. A profile is its ordered channel
// list; a role's DMX offset is its index in roles[]. constexpr POD in .rodata.
constexpr uint8_t kMaxProfileChannels = 30;   // 9 for di's generic set; 16 / 24 / 30 for the heads (2026-10-16)

struct Profile {
  const char* name;
  const char* cat;
  uint8_t count;
  Role roles[kMaxProfileChannels];
};

namespace detail {
using R = Role;
}

constexpr Profile kProfiles[] = {
  {"dimmer",    "_GENERIC", 1, {detail::R::Dimmer}},
  {"ds",        "_GENERIC", 2, {detail::R::Dimmer, detail::R::Strobe}},
  {"drgb",      "_GENERIC", 4, {detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B}},
  {"drgbs",     "_GENERIC", 5, {detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B, detail::R::Strobe}},
  {"drgbw",     "_GENERIC", 5, {detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B, detail::R::W}},
  {"drgbwa",    "_GENERIC", 6, {detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B, detail::R::W, detail::R::A}},
  {"rgb",       "_GENERIC", 3, {detail::R::R, detail::R::G, detail::R::B}},
  {"rgba",      "_GENERIC", 4, {detail::R::R, detail::R::G, detail::R::B, detail::R::A}},
  {"rgbw",      "_GENERIC", 4, {detail::R::R, detail::R::G, detail::R::B, detail::R::W}},
  {"rgbaw",     "_GENERIC", 5, {detail::R::R, detail::R::G, detail::R::B, detail::R::A, detail::R::W}},
  {"dwwcw",     "_GENERIC", 3, {detail::R::Dimmer, detail::R::Warm, detail::R::Cool}},
  {"ptdrgb",    "_MOVING",  6, {detail::R::Pan, detail::R::Tilt, detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B}},
  {"ppttdrgbw", "_MOVING",  9, {detail::R::Pan, detail::R::PanFine, detail::R::Tilt, detail::R::TiltFine,
                                detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B, detail::R::W}},
  // U-Plus charts (di.iiii docs/moxir/devices/up-b380f.md, up-pl5403.md; owner-approved 2026-10-15). Appended, so stored
  // profile indexes (NVS fixtures/prof) keep their meaning. `none` channels (colour, gobo, prism, frost, focus, speed,
  // reset, lamp; the wash's function and speed) stay at 0: the node never strikes, resets or kills a lamp.
  // B380F ch 2 strobe: 0 = shutter closed, 1–4 / 252–255 = open, 51–240 = rate — the operator opens it.
  {"up-b380f-16", "U-PLUS", 16, {detail::R::None, detail::R::Strobe, detail::R::Dimmer, detail::R::None,
                                 detail::R::None, detail::R::None, detail::R::None, detail::R::None,
                                 detail::R::None, detail::R::Pan, detail::R::PanFine, detail::R::Tilt,
                                 detail::R::TiltFine, detail::R::None, detail::R::None, detail::R::None}},
  {"up-pl5403-8", "U-PLUS", 8, {detail::R::Dimmer, detail::R::R, detail::R::G, detail::R::B, detail::R::W,
                                detail::R::Strobe, detail::R::None, detail::R::None}},
  // The 250 W LED head of the owner's paper manual (photos 2026-10-16; di.iiii docs/moxir/devices/led250-head-24-30ch.md):
  // pan, pan fine, tilt, tilt fine, speed, dimmer, [dimmer fine], strobe, colour, [colour fine], rot gobo, gobo rot, [fine],
  // fixed gobo, prism, prism rot, [fine], focus, [fine], zoom, [fine], frost, halo master, halo r g b, effect ×3, function.
  // Wheels, prism, focus, zoom, frost, effects and function stay 0 (open white, near, no reset). The strobe's open value is
  // not in the manual (unlabelled bands) — walked on the unit, setup day.
  {"led250-24", "LED-HEAD", 24, {detail::R::Pan, detail::R::PanFine, detail::R::Tilt, detail::R::TiltFine, detail::R::None,
                                 detail::R::Dimmer, detail::R::Strobe, detail::R::None, detail::R::None, detail::R::None,
                                 detail::R::None, detail::R::None, detail::R::None, detail::R::None, detail::R::None,
                                 detail::R::None, detail::R::Halo, detail::R::R, detail::R::G, detail::R::B,
                                 detail::R::None, detail::R::None, detail::R::None, detail::R::None}},
  {"led250-30", "LED-HEAD", 30, {detail::R::Pan, detail::R::PanFine, detail::R::Tilt, detail::R::TiltFine, detail::R::None,
                                 detail::R::Dimmer, detail::R::None, detail::R::Strobe, detail::R::None, detail::R::None,
                                 detail::R::None, detail::R::None, detail::R::None, detail::R::None, detail::R::None,
                                 detail::R::None, detail::R::None, detail::R::None, detail::R::None, detail::R::None,
                                 detail::R::None, detail::R::None, detail::R::Halo, detail::R::R, detail::R::G,
                                 detail::R::B, detail::R::None, detail::R::None, detail::R::None, detail::R::None}},
};
constexpr uint8_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);
constexpr uint8_t kGenericProfileCount = 13;   // di's own generic set (engine.js); the device charts follow it
constexpr uint8_t kNoProfile = 0xFF;

inline const Profile* profileAt(uint8_t id) { return id < kProfileCount ? &kProfiles[id] : nullptr; }

inline int profileIndex(const char* name) {
  if (!name) return -1;
  for (uint8_t i = 0; i < kProfileCount; i++)
    if (strcmp(kProfiles[i].name, name) == 0) return i;
  return -1;
}

// DMX offset of a role inside a profile, or -1 when the profile lacks it.
inline int offsetOf(const Profile& p, Role r) {
  if (r == Role::None) return -1;   // never read, written, identified or listed
  for (uint8_t i = 0; i < p.count; i++)
    if (p.roles[i] == r) return i;
  return -1;
}

// ── Fixtures ──────────────────────────────────────────────────────────────────
// A node fixture. `end` is derived (start + channel count - 1), never stored.
// x, y: stage position 0..255 (di uses 0..1). This is the v2 NVS record of V6.
struct NodeFixture {
  char name[16];      // NUL-terminated after load repair; read with a 16-byte bound
  uint16_t start;     // 1-based DMX address
  uint8_t profile;    // index into kProfiles
  uint8_t x;
  uint8_t y;
  bool enabled;       // patched; only enabled fixtures with a known profile are listed
};
static_assert(sizeof(NodeFixture) == 22, "NodeFixture is the V6 NVS record; its size is part of the blob");

// Layout-identical to main.cpp's GroupDef (a channel range).
struct GroupRange {
  char name[16];
  uint16_t start;
  uint16_t end;
  bool enabled;
};

inline uint8_t fixtureWidth(const NodeFixture& f) {
  const Profile* p = profileAt(f.profile);
  return p ? p->count : 0;
}

// Last channel (1-based). May pass 512 for a fixture from an unrepaired blob.
inline uint16_t fixtureEnd(const NodeFixture& f) {
  uint8_t w = fixtureWidth(f);
  return w ? uint16_t(f.start + w - 1) : f.start;
}

// True when a fixture of this profile at this address fits inside 1..512.
inline bool fixtureFits(uint16_t start, uint8_t profile) {
  const Profile* p = profileAt(profile);
  return p && start >= 1 && uint32_t(start) + p->count - 1 <= kChannels;
}

inline bool fixtureListed(const NodeFixture& f) { return f.enabled && profileAt(f.profile) != nullptr; }

// 0-based channel of a role, or -1 (role not in the profile, or past 512).
inline int roleChannel(const NodeFixture& f, Role r) {
  const Profile* p = profileAt(f.profile);
  if (!p || f.start < 1) return -1;
  int off = offsetOf(*p, r);
  if (off < 0) return -1;
  int ch = int(f.start) - 1 + off;
  return ch < int(kChannels) ? ch : -1;
}

// A role's value read from a 512-byte frame (web layer or scene snapshot), or -1.
inline int readRole(const uint8_t* frame, const NodeFixture& f, Role r) {
  int ch = roleChannel(f, r);
  return ch < 0 ? -1 : frame[ch];
}

inline uint8_t clamp8(int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : uint8_t(v)); }

// Writes one role into the web layer; false when the fixture has no such channel.
inline bool writeRole(uint8_t* web, const NodeFixture& f, Role r, int32_t value) {
  int ch = roleChannel(f, r);
  if (ch < 0) return false;
  web[ch] = clamp8(value);
  return true;
}

struct RoleValue {
  Role role;
  int32_t value;
};

struct WriteCount {
  uint8_t applied;
  uint8_t ignored;   // roles the profile lacks (di's virtual dimmer on rgb, etc.)
};

inline WriteCount writeValues(uint8_t* web, const NodeFixture& f, const RoleValue* vals, uint8_t n) {
  WriteCount c = {0, 0};
  for (uint8_t i = 0; i < n; i++) {
    if (writeRole(web, f, vals[i].role, vals[i].value)) c.applied++;
    else c.ignored++;
  }
  return c;
}

// di's x/y is 0..1. The node prints x/255 as thousandths and reads thousandths
// back with rounding; every byte survives the round trip.
inline uint16_t posToMilli(uint8_t v) { return uint16_t((uint32_t(v) * 1000 + 127) / 255); }
inline uint8_t posFromMilli(uint32_t milli) {
  if (milli > 1000) milli = 1000;
  return uint8_t((milli * 255 + 500) / 1000);
}

// Fixture fully inside a channel range (group membership).
inline bool fixtureInRange(const NodeFixture& f, uint16_t start, uint16_t end) {
  return fixtureWidth(f) && f.start >= start && fixtureEnd(f) <= end;
}

// ── V6 migration helper (nothing in NVS changes in V3) ─────────────────────────
// Today's FixtureDef stores start..end. The width picks a profile; widths with no
// node profile become a one-channel `dimmer` at `start` and *exact is false, so
// the migration can say so.
inline uint8_t profileForWidth(uint16_t width, bool* exact) {
  static const char* const byWidth[10] = {nullptr, "dimmer", "ds", "rgb", "drgb", "drgbw",
                                          "drgbwa", nullptr, nullptr, "ppttdrgbw"};
  const char* n = width < 10 ? byWidth[width] : nullptr;
  if (exact) *exact = n != nullptr;
  return uint8_t(profileIndex(n ? n : "dimmer"));
}

inline NodeFixture migrateV1Fixture(const char* name, uint16_t start, uint16_t end, uint8_t x, uint8_t y,
                                    bool enabled, bool* exact) {
  NodeFixture f;
  memset(&f, 0, sizeof(f));
  if (name) strncpy(f.name, name, sizeof(f.name) - 1);
  if (start < 1) start = 1;
  if (start > kChannels) start = kChannels;
  if (end < start) end = start;
  f.start = start;
  f.profile = profileForWidth(uint16_t(end - start + 1), exact);
  if (!fixtureFits(f.start, f.profile)) {       // cannot happen for v1 ranges inside 1..512
    f.profile = uint8_t(profileIndex("dimmer"));
    if (exact) *exact = false;
  }
  f.x = x;
  f.y = y;
  f.enabled = enabled;
  return f;
}

// ── V6 fixture store (NVS fixtures/items2) ──────────────────────────────────────
// v2 = NodeFixture[kMaxFixtures] (704 B) + count, under NEW keys beside the v1
// blob (items/count/prof), which is never written again once v2 exists, so an
// older build still reads the patch as it was at migration time.
constexpr uint8_t kMaxFixtures = 32;
constexpr uint8_t kV1Slots = 12;            // MAX_FIXTURES of the v1 build: the v1 blob holds this many
constexpr uint8_t kStoreVersion = 2;

// Layout-identical to main.cpp's FixtureDef (what the v1 blob holds).
struct V1Fixture {
  char name[16];
  uint16_t start;
  uint16_t end;
  uint8_t x;
  uint8_t y;
  bool enabled;
};
static_assert(sizeof(V1Fixture) == 24, "the v1 blob is 12 x 24 B");

struct MigrateReport {
  uint8_t migrated;   // slots converted (all kV1Slots; the rest of the 32 get defaults)
  uint8_t unmapped;   // active slots whose width has no profile: kept at `start` as `dimmer`
};

// The factory patch: 4 enabled rgb at 1/4/7/10, the historic v1 positions for
// slots 4..11, then 3-wide slots on a grid.
inline NodeFixture defaultFixture(uint8_t i) {
  static const uint16_t starts[kV1Slots] = {1, 4, 7, 10, 64, 67, 70, 73, 128, 131, 134, 137};
  NodeFixture f;
  memset(&f, 0, sizeof(f));
  f.name[0] = 'F';
  const uint8_t n = uint8_t(i + 1);
  if (n >= 10) { f.name[1] = char('0' + n / 10); f.name[2] = char('0' + n % 10); }
  else f.name[1] = char('0' + n);
  f.start = i < kV1Slots ? starts[i] : uint16_t(140 + (i - kV1Slots) * 3);   // 140..199
  f.profile = uint8_t(profileIndex("rgb"));
  f.x = uint8_t(32 + (i % 4) * 64);
  f.y = uint8_t(i < kV1Slots ? 32 + (i / 4) * 96 : 32 + ((i - kV1Slots) / 4) * 28);
  f.enabled = i < 4;
  return f;
}

inline void defaultFixtures(NodeFixture* out, uint8_t* count) {
  for (uint8_t i = 0; i < kMaxFixtures; i++) out[i] = defaultFixture(i);
  *count = 4;
}

// One v1 blob (kV1Slots slots, `prof` = its optional per-slot profile bytes, may
// be null) into the 32-slot v2 array. A stored profile whose width still matches
// start..end is kept (what the desk showed); otherwise width -> profile. Slots
// kV1Slots.. get the defaults. Slots at or past `count` are disabled.
inline MigrateReport migrateV1Blob(const V1Fixture* v1, const uint8_t* prof, uint8_t count, NodeFixture* out) {
  MigrateReport rep = {0, 0};
  for (uint8_t i = 0; i < kMaxFixtures; i++) out[i] = defaultFixture(i);
  for (uint8_t i = 0; i < kV1Slots; i++) {
    const bool active = i < count && v1[i].enabled;
    bool exact = true;
    NodeFixture f = migrateV1Fixture(v1[i].name, v1[i].start, v1[i].end, v1[i].x, v1[i].y, active, &exact);
    const uint16_t e = v1[i].end < f.start ? f.start : v1[i].end;
    if (prof && prof[i] < kProfileCount && profileAt(prof[i])->count == e - f.start + 1 && fixtureFits(f.start, prof[i])) {
      f.profile = prof[i];
      exact = true;
    }
    if (!exact && active) rep.unmapped++;
    out[i] = f;
    rep.migrated++;
  }
  return rep;
}

// Repair a v2 array read from NVS, as loadFixtures() did for v1: a blob of the
// right size can still carry garbage. A bad profile becomes `dimmer`, a start
// that does not fit moves to one that does, the name gets its NUL, slots at or
// past the count are disabled. Returns the count bounded to 1..kMaxFixtures.
inline uint8_t repairV2(NodeFixture* fx, uint8_t count) {
  if (count < 1) count = 1;
  if (count > kMaxFixtures) count = kMaxFixtures;
  for (uint8_t i = 0; i < kMaxFixtures; i++) {
    fx[i].name[sizeof(fx[i].name) - 1] = 0;
    if (!profileAt(fx[i].profile)) fx[i].profile = uint8_t(profileIndex("dimmer"));
    if (fx[i].start < 1) fx[i].start = 1;
    if (!fixtureFits(fx[i].start, fx[i].profile)) fx[i].start = uint16_t(kChannels - fixtureWidth(fx[i]) + 1);
    fx[i].enabled = fx[i].enabled && i < count;
  }
  return count;
}

// ── Streaming JSON writer ─────────────────────────────────────────────────────
// Formats into buf and hands each full buffer to the sink, so no chunk is longer
// than kChunk and the document is never whole in memory. A sink that returns
// false stops the writer: every later call does nothing. Integers only; the 0..1
// stage position is printed fixed-point.
using SinkFn = bool (*)(void* ctx, const char* data, size_t len);
constexpr size_t kChunk = 512;
constexpr uint8_t kMaxDepth = 32;

struct JsonWriter {
  SinkFn sink;
  void* ctx;
  uint32_t total;      // bytes handed to the sink
  uint32_t chunks;
  uint16_t maxChunk;
  uint16_t len;
  uint32_t hasItem;    // bit d: container at depth d already holds an item
  uint8_t depth;
  bool afterKey;
  bool stopped;
  char buf[kChunk];

  void begin(SinkFn fn, void* c) {
    sink = fn; ctx = c; total = 0; chunks = 0; maxChunk = 0; len = 0;
    hasItem = 0; depth = 0; afterKey = false; stopped = false;
  }

  void flush() {
    if (stopped || len == 0) { len = 0; return; }
    total += len;
    chunks++;
    if (len > maxChunk) maxChunk = len;
    if (!sink(ctx, buf, len)) stopped = true;
    len = 0;
  }

  // Flushes the tail. True when the sink took the whole document.
  bool finish() { flush(); return !stopped; }

  void put(char c) {
    if (stopped) return;
    if (len == kChunk) flush();
    buf[len++] = c;
  }
  void put(const char* s, size_t n) { for (size_t i = 0; i < n && !stopped; i++) put(s[i]); }
  void put(const char* s) { if (s) put(s, strlen(s)); }

  // Comma and key bookkeeping before any value.
  void pre() {
    if (afterKey) { afterKey = false; return; }
    if (depth == 0) return;
    uint32_t bit = 1u << (depth - 1);
    if (hasItem & bit) put(',');
    hasItem |= bit;
  }
  void open(char c) {
    pre();
    put(c);
    if (depth < kMaxDepth) depth++;
    hasItem &= ~(1u << (depth - 1));
  }
  void close(char c) {
    put(c);
    if (depth) depth--;
  }
  void obj() { open('{'); }
  void endObj() { close('}'); }
  void arr() { open('['); }
  void endArr() { close(']'); }

  // A JSON string from at most maxLen bytes of s (stops at NUL). Escapes '"',
  // '\' and every byte below 0x20; other bytes pass through (UTF-8).
  void strRaw(const char* s, size_t maxLen) {
    static const char hex[] = "0123456789abcdef";
    put('"');
    for (size_t i = 0; s && i < maxLen && s[i] && !stopped; i++) {
      unsigned char c = (unsigned char)s[i];
      if (c == '"' || c == '\\') { put('\\'); put(char(c)); }
      else if (c == '\n') put("\\n", 2);
      else if (c == '\r') put("\\r", 2);
      else if (c == '\t') put("\\t", 2);
      else if (c < 0x20) { put("\\u00", 4); put(hex[c >> 4]); put(hex[c & 15]); }
      else put(char(c));
    }
    put('"');
  }

  void key(const char* k) { pre(); strRaw(k, SIZE_MAX); put(':'); afterKey = true; }
  void str(const char* s, size_t maxLen = SIZE_MAX) { pre(); strRaw(s, maxLen); }
  void strOrNull(const char* s) { if (s) str(s); else null(); }
  void null() { pre(); put("null", 4); }
  void boolean(bool b) { pre(); if (b) put("true", 4); else put("false", 5); }
  void u(uint32_t v) {
    pre();
    char t[10];
    uint8_t n = 0;
    do { t[n++] = char('0' + v % 10); v /= 10; } while (v);
    while (n) put(t[--n]);
  }
  void i(int32_t v) {
    if (v < 0) { pre(); put('-'); afterKey = true; u(uint32_t(-(int64_t)v)); }
    else u(uint32_t(v));
  }
  // An id made of a one-letter prefix and a 1-based number: "f3", "s1".
  void id(char prefix, uint32_t n) {
    pre();
    put('"'); put(prefix);
    afterKey = true; u(n);   // u() must not add a comma here
    put('"');
  }
  // 0..255 as a 0..1 number, three decimals at most ("0", "0.502", "1").
  void unit255(uint8_t v) {
    pre();
    uint16_t m = posToMilli(v);
    if (m >= 1000) { put('1'); return; }
    put('0');
    if (m == 0) return;
    char d[3] = {char('0' + m / 100), char('0' + m / 10 % 10), char('0' + m % 10)};
    uint8_t n = 3;
    while (n > 1 && d[n - 1] == '0') n--;
    put('.');
    put(d, n);
  }
  // A literal JSON value the caller vouches for (static shapes).
  void rawValue(const char* json) { pre(); put(json); }
};

// Collects the whole reply (tests, small replies).
struct MemorySink {
  char* dst;
  size_t cap;
  size_t len;
  bool overflow;
};
inline bool memorySink(void* ctx, const char* d, size_t n) {
  MemorySink& m = *static_cast<MemorySink*>(ctx);
  if (m.len + n > m.cap) { m.overflow = true; return false; }
  memcpy(m.dst + m.len, d, n);
  m.len += n;
  return true;
}

// Pull adaptor for ESPAsyncWebServer's chunked response, which asks for
// (buf, maxLen, index): skip the first `skip` bytes of the document, copy up to
// `cap`, then stop the writer. Re-runs the writer per pull, so the facts must not
// change between pulls (docs/DESK_BRIDGE_CORE.md, "Pull adaptor").
struct WindowSink {
  char* dst;
  size_t cap;
  size_t skip;
  size_t seen;
  size_t written;
};
inline bool windowSink(void* ctx, const char* d, size_t n) {
  WindowSink& w = *static_cast<WindowSink*>(ctx);
  size_t at = w.seen;
  w.seen += n;
  if (w.seen <= w.skip) return true;
  size_t from = at < w.skip ? w.skip - at : 0;
  size_t take = n - from;
  if (take > w.cap - w.written) take = w.cap - w.written;
  memcpy(w.dst + w.written, d + from, take);
  w.written += take;
  return w.written < w.cap;
}

// ── Live facts ────────────────────────────────────────────────────────────────
struct NetIface {
  const char* iface;
  const char* address;
  const char* netmask;
};

struct InputFacts;  // api/input facts, below

// Everything the replies need, as pointers and values. A zeroed DeskFacts is
// valid: every list is empty and every string falls back, so the shape holds.
struct DeskFacts {
  const uint8_t* web;                 // 512: the web layer (fixture values)
  const uint8_t* dmxOut;              // 512: what api/dmx shows (nullptr: web)
  const NodeFixture* fixtures;
  uint8_t fixtureSlots;
  const GroupRange* groups;
  uint8_t groupSlots;
  const uint8_t* const* scenes;       // sceneSlots pointers to 512-byte snapshots; nullptr = empty slot
  const char* const* sceneNames;      // optional, per slot; nullptr → "Scene N"
  uint8_t sceneSlots;
  int8_t activeScene;                 // slot, or -1
  uint32_t sceneFadeMs;
  uint8_t master;
  bool blackout;
  const char* fxMode;
  uint16_t fxBpm;
  uint8_t fxDepth;
  bool fxEnabled;
  const char* const* fxModes;
  uint8_t fxModeCount;
  const uint8_t* chaseScenes;         // scene slot per cue step
  uint8_t chaseCount;
  bool chaseRunning;
  uint32_t chaseHoldMs;
  uint32_t chaseFadeMs;
  const char* outputDriver;
  const char* outputMode;             // "broadcast" | "unicast"
  const char* const* outputTargets;
  uint8_t outputTargetCount;
  bool outputEnabled;
  uint16_t refreshHz;
  const NetIface* interfaces;
  uint8_t interfaceCount;
  const char* const* broadcast;
  uint8_t broadcastCount;
  uint32_t packetsSent;
  const char* lastError;              // nullptr → null
  const char* nodeName;
  // V4 additions (both optional; a zeroed DeskFacts keeps the V3 meaning).
  const uint32_t* sceneFades;         // per slot (NVS sN.fade); nullptr → sceneFadeMs for all
  const InputFacts* input;     // api/input; nullptr → input off, nothing seen
};

// ── Replies ───────────────────────────────────────────────────────────────────
namespace detail {

inline const char* orStr(const char* s, const char* fallback) { return s ? s : fallback; }

inline void strList(JsonWriter& w, const char* const* items, uint8_t n) {
  w.arr();
  for (uint8_t k = 0; items && k < n; k++) w.str(orStr(items[k], ""));
  w.endArr();
}

// {"role":value,...} for the profile's roles, read from frame, in profile order.
inline void fixtureValues(JsonWriter& w, const NodeFixture& f, const uint8_t* frame) {
  w.obj();
  const Profile* p = profileAt(f.profile);
  for (uint8_t k = 0; p && k < p->count; k++) {
    int v = frame ? readRole(frame, f, p->roles[k]) : -1;
    if (v < 0) continue;
    w.key(roleName(p->roles[k]));
    w.u(uint32_t(v));
  }
  w.endObj();
}

constexpr const char* kDefaultLimits =
    "{\"dimMin\":0,\"dimMax\":255,\"panMin\":0,\"panMax\":255,\"tiltMin\":0,\"tiltMax\":255,"
    "\"invertPan\":false,\"invertTilt\":false,\"swapPT\":false}";

inline void fixture(JsonWriter& w, const NodeFixture& f, uint8_t slot, const uint8_t* web) {
  w.obj();
  w.key("id"); w.id('f', slot + 1u);
  w.key("index"); w.u(slot + 1u);
  w.key("name"); w.str(f.name, sizeof(f.name));
  w.key("universe"); w.u(0);
  w.key("address"); w.u(f.start);
  w.key("profile"); w.str(kProfiles[f.profile].name);
  w.key("on"); w.boolean(true);
  w.key("x"); w.unit255(f.x);
  w.key("y"); w.unit255(f.y);
  w.key("values"); fixtureValues(w, f, web);
  w.key("limits"); w.rawValue(kDefaultLimits);
  w.endObj();
}

inline uint8_t listedCount(const DeskFacts& d) {
  uint8_t n = 0;
  for (uint8_t k = 0; d.fixtures && k < d.fixtureSlots; k++) if (fixtureListed(d.fixtures[k])) n++;
  return n;
}

inline void scene(JsonWriter& w, const DeskFacts& d, uint8_t slot) {
  const uint8_t* snap = d.scenes[slot];
  w.obj();
  w.key("id"); w.id('s', slot + 1u);
  w.key("name");
  if (d.sceneNames && d.sceneNames[slot]) w.str(d.sceneNames[slot]);
  else {
    char nm[12] = "Scene ";
    uint8_t n = uint8_t(slot + 1u), at = 6;
    if (n >= 100) nm[at++] = char('0' + n / 100);
    if (n >= 10) nm[at++] = char('0' + n / 10 % 10);
    nm[at++] = char('0' + n % 10);
    nm[at] = 0;
    w.str(nm);
  }
  w.key("fadeMs"); w.u(d.sceneFades ? d.sceneFades[slot] : d.sceneFadeMs);
  w.key("fixtures"); w.arr();
  for (uint8_t k = 0; d.fixtures && k < d.fixtureSlots; k++) {
    if (!fixtureListed(d.fixtures[k])) continue;
    w.obj();
    w.key("id"); w.id('f', k + 1u);
    w.key("on"); w.boolean(true);
    w.key("values"); fixtureValues(w, d.fixtures[k], snap);
    w.endObj();
  }
  w.endArr();
  w.key("raw"); w.rawValue("{}");
  w.key("live"); w.u(listedCount(d));
  w.key("missing"); w.u(0);
  w.endObj();
}

inline void show(JsonWriter& w, const DeskFacts& d) {
  w.obj();
  w.key("space"); w.null();
  w.key("label"); w.str(orStr(d.nodeName, "node"));
  w.key("file"); w.str("");
  w.key("saved"); w.boolean(true);
  w.key("live"); w.boolean(d.outputEnabled);
  w.key("machine"); w.null();
  w.endObj();
}

// A group is listed when its slot is enabled and at least one listed fixture
// lies wholly inside its range: di never keeps a group with no members (it
// drops them on fixtures/remove), so an empty range is not shown either.
inline bool groupListed(const DeskFacts& d, uint8_t g) {
  if (!d.groups || g >= d.groupSlots || !d.groups[g].enabled) return false;
  for (uint8_t k = 0; d.fixtures && k < d.fixtureSlots; k++)
    if (fixtureListed(d.fixtures[k]) && fixtureInRange(d.fixtures[k], d.groups[g].start, d.groups[g].end)) return true;
  return false;
}

inline void group(JsonWriter& w, const DeskFacts& d, uint8_t g) {
  const GroupRange& gr = d.groups[g];
  w.obj();
  w.key("id"); w.id('g', g + 1u);
  w.key("name"); w.str(gr.name, sizeof(gr.name));
  w.key("ids"); w.arr();
  for (uint8_t k = 0; d.fixtures && k < d.fixtureSlots; k++)
    if (fixtureListed(d.fixtures[k]) && fixtureInRange(d.fixtures[k], gr.start, gr.end)) w.id('f', k + 1u);
  w.endArr();
  w.endObj();
}

inline void chase(JsonWriter& w, const DeskFacts& d) {
  w.obj();
  w.key("enabled"); w.boolean(d.chaseRunning);
  w.key("sceneIds"); w.arr();
  for (uint8_t k = 0; d.chaseScenes && k < d.chaseCount; k++) w.id('s', d.chaseScenes[k] + 1u);
  w.endArr();
  w.key("holdMs"); w.u(d.chaseHoldMs);
  w.key("fadeMs"); w.u(d.chaseFadeMs);
  w.key("sync"); w.str("clock");
  w.key("beats"); w.u(4);
  w.endObj();
}

inline void fx(JsonWriter& w, const DeskFacts& d) {
  w.obj();
  w.key("mode"); w.str(orStr(d.fxMode, "none"));
  w.key("bpm"); w.u(d.fxBpm);
  w.key("depth"); w.u(d.fxDepth);
  w.key("enabled"); w.boolean(d.fxEnabled);
  w.key("spatial"); w.str("patch");
  w.key("exclude"); w.rawValue("[]");
  w.endObj();
}

inline void output(JsonWriter& w, const DeskFacts& d) {
  w.obj();
  w.key("driver"); w.str(orStr(d.outputDriver, "artnet"));
  w.key("serialPort"); w.str("");
  w.key("mode"); w.str(orStr(d.outputMode, "broadcast"));
  w.key("targets"); strList(w, d.outputTargets, d.outputTargetCount);
  w.key("enabled"); w.boolean(d.outputEnabled);
  w.key("refreshHz"); w.u(d.refreshHz);
  w.key("extra"); w.rawValue("[]");
  w.endObj();
}

inline void cues(JsonWriter& w) {
  w.rawValue("{\"project\":null,\"list\":[],\"loop\":false,\"autoplay\":false,\"index\":-1,"
             "\"running\":false,\"nextAt\":null,\"missing\":[],\"n\":0,\"name\":null,\"nextInMs\":null}");
}

}  // namespace detail

// GET api/state: every state_keys.required and nested_required key of the
// contract, class-b keys as empty values of the right type.
inline bool writeState(JsonWriter& w, const DeskFacts& d) {
  w.obj();
  w.key("master"); w.u(d.master);
  w.key("blackout"); w.boolean(d.blackout);

  w.key("fixtures"); w.arr();
  for (uint8_t k = 0; d.fixtures && k < d.fixtureSlots; k++)
    if (fixtureListed(d.fixtures[k])) detail::fixture(w, d.fixtures[k], k, d.web);
  w.endArr();

  w.key("groups"); w.arr();
  for (uint8_t g = 0; d.groups && g < d.groupSlots; g++)
    if (detail::groupListed(d, g)) detail::group(w, d, g);
  w.endArr();

  w.key("raw"); w.rawValue("{}");

  w.key("scenes"); w.arr();
  for (uint8_t s = 0; d.scenes && s < d.sceneSlots; s++)
    if (d.scenes[s]) detail::scene(w, d, s);
  w.endArr();
  w.key("activeScene");
  if (d.activeScene >= 0 && d.scenes && d.activeScene < d.sceneSlots && d.scenes[d.activeScene])
    w.id('s', uint32_t(d.activeScene) + 1u);
  else w.null();

  w.key("chase"); detail::chase(w, d);
  w.key("fx"); detail::fx(w, d);
  w.key("fxModes");
  if (d.fxModes && d.fxModeCount) detail::strList(w, d.fxModes, d.fxModeCount);
  else w.rawValue("[\"none\"]");

  w.key("lfos"); w.rawValue("[]");
  w.key("layers"); w.rawValue("[]");
  w.key("looks"); w.rawValue("[]");
  w.key("sets"); w.rawValue("[]");
  w.key("customProfiles"); w.rawValue("[]");
  w.key("audioCfg");
  w.rawValue("{\"enabled\":false,\"mode\":\"level\",\"amount\":255,\"release\":300,\"useBeats\":true}");

  w.key("output"); detail::output(w, d);

  w.key("profiles"); w.obj();
  for (uint8_t p = 0; p < kProfileCount; p++) {
    const Profile& pr = kProfiles[p];
    w.key(pr.name); w.obj();
    w.key("label");
    {
      // di's labelFor(): "<name> (<n> Channel[s])"
      char lb[40];
      size_t n = strlen(pr.name);
      if (n > 24) n = 24;
      memcpy(lb, pr.name, n);
      const char* tail = pr.count == 1 ? " Channel)" : " Channels)";
      lb[n++] = ' '; lb[n++] = '(';
      lb[n++] = char('0' + pr.count);   // kMaxProfileChannels < 10
      size_t t = strlen(tail);
      memcpy(lb + n, tail, t + 1);
      w.str(lb);
    }
    w.key("channels"); w.arr();
    for (uint8_t c = 0; c < pr.count; c++) w.str(roleName(pr.roles[c]));
    w.endArr();
    w.key("cat"); w.str(pr.cat);
    w.key("custom"); w.boolean(false);
    w.endObj();
  }
  w.endObj();

  w.key("roleKinds"); w.obj();
  for (uint8_t k = 0; k < kKindCount; k++) {
    w.key(kKindNames[k]); w.arr();
    for (uint8_t r = 0; r < kRoleCount; r++)
      if (uint8_t(kRoles[r].kind) == k && Role(r) != Role::None) w.str(kRoles[r].name);
    w.endArr();
  }
  w.endObj();

  w.key("caps");
  w.rawValue("{\"midi\":false,\"audio\":false,\"library\":false,\"looks\":false,\"layers\":false,"
             "\"lfos\":false,\"sets\":false,\"cues\":false,\"profilesEdit\":false,"
             "\"multiUniverse\":false,\"stream\":true,\"node\":true}");
  w.key("show"); detail::show(w, d);

  w.key("status"); w.obj();
  w.key("universes"); w.rawValue("[0]");
  w.key("nodes"); w.rawValue("[]");
  w.key("interfaces"); w.arr();
  for (uint8_t k = 0; d.interfaces && k < d.interfaceCount; k++) {
    w.obj();
    w.key("iface"); w.str(detail::orStr(d.interfaces[k].iface, ""));
    w.key("address"); w.str(detail::orStr(d.interfaces[k].address, ""));
    w.key("netmask"); w.str(detail::orStr(d.interfaces[k].netmask, ""));
    w.endObj();
  }
  w.endArr();
  w.key("broadcast"); detail::strList(w, d.broadcast, d.broadcastCount);
  w.key("packetsSent"); w.u(d.packetsSent);
  w.key("lastError"); w.strOrNull(d.lastError);
  w.key("serial"); w.null();
  w.key("identifying"); w.rawValue("[]");
  w.key("extra"); w.rawValue("[]");
  w.key("driver"); w.str(detail::orStr(d.outputDriver, "artnet"));
  w.key("outputEnabled"); w.boolean(d.outputEnabled);
  w.key("fading"); w.boolean(false);
  w.endObj();

  w.endObj();
  return w.finish();
}

// GET api/dmx: {dmx:{"0":[512]}, master, blackout, looks:[], cues:null}.
inline bool writeDmx(JsonWriter& w, const DeskFacts& d) {
  const uint8_t* f = d.dmxOut ? d.dmxOut : d.web;
  w.obj();
  w.key("dmx"); w.obj();
  w.key("0"); w.arr();
  for (uint16_t c = 0; c < kChannels; c++) w.u(f ? f[c] : 0);
  w.endArr();
  w.endObj();
  w.key("master"); w.u(d.master);
  w.key("blackout"); w.boolean(d.blackout);
  w.key("looks"); w.rawValue("[]");
  w.key("cues"); w.null();
  w.endObj();
  return w.finish();
}

// GET api/cues: an empty cue list (n:0 hides the cue strip).
inline bool writeCues(JsonWriter& w) {
  w.obj();
  w.key("cues"); detail::cues(w);
  w.endObj();
  return w.finish();
}

// GET api/show: one show, always this node's.
inline bool writeShow(JsonWriter& w, const DeskFacts& d) {
  detail::show(w, d);
  return w.finish();
}

// GET api/looks: none on a node.
inline bool writeLooks(JsonWriter& w) {
  w.obj();
  w.key("looks"); w.rawValue("[]");
  w.endObj();
  return w.finish();
}

using ReplyFn = bool (*)(JsonWriter&, const DeskFacts&);

// One pull of a chunked response: bytes [index, index + maxLen) of the reply.
// Returns the count copied; 0 means the reply is complete.
inline size_t pullWindow(ReplyFn fn, const DeskFacts& d, JsonWriter& w, uint8_t* buf, size_t maxLen,
                         size_t index) {
  WindowSink s = {reinterpret_cast<char*>(buf), maxLen, index, 0, 0};
  if (maxLen == 0) return 0;
  w.begin(windowSink, &s);
  fn(w, d);
  return s.written;
}

// ══ V4: the /api routes (docs/DESK_BRIDGE_CORE.md §4) ═════════════════════════
// main.cpp parses the JSON body (ArduinoJson) and calls these; everything that
// decides what a body means or what a reply says is here, pure and tested.

// ── Route table ───────────────────────────────────────────────────────────────
// One row per path of the contract (091a1a6), in its order. kRoutes has the
// same paths as RouteKind::DeskApi (a native test holds the two and the
// vendored contract together).
constexpr uint8_t kGet = 1;
constexpr uint8_t kPost = 2;
enum class ApiClass : uint8_t { A, B, C };

struct ApiRoute {
  const char* path;
  uint8_t methods;
  ApiClass cls;
};

constexpr ApiRoute kDeskApi[] = {
  {"/api/state", kGet, ApiClass::A},
  {"/api/dmx", kGet, ApiClass::A},
  {"/api/input", kGet | kPost, ApiClass::A},
  {"/api/input/release", kPost, ApiClass::A},
  {"/api/master", kPost, ApiClass::A},
  {"/api/raw", kPost, ApiClass::A},
  {"/api/fx", kPost, ApiClass::A},
  {"/api/chase", kPost, ApiClass::A},
  {"/api/output", kPost, ApiClass::A},
  {"/api/fixtures/add", kPost, ApiClass::A},
  {"/api/fixture", kPost, ApiClass::A},
  {"/api/fixtures/all", kPost, ApiClass::A},
  {"/api/fixtures/move", kPost, ApiClass::A},
  {"/api/fixtures/readdress", kPost, ApiClass::A},
  {"/api/fixtures/identify", kPost, ApiClass::A},
  {"/api/groups/add", kPost, ApiClass::A},
  {"/api/scenes/save", kPost, ApiClass::A},
  {"/api/scenes/update", kPost, ApiClass::A},
  {"/api/scenes/recall", kPost, ApiClass::A},
  {"/api/scenes/remove", kPost, ApiClass::A},
  {"/api/groups/remove", kPost, ApiClass::A},
  {"/api/fixtures/remove", kPost, ApiClass::A},
  {"/api/cues", kGet, ApiClass::B},
  {"/api/cues/go", kPost, ApiClass::B},
  {"/api/cues/back", kPost, ApiClass::B},
  {"/api/cues/stop", kPost, ApiClass::B},
  {"/api/cues/loop", kPost, ApiClass::B},
  {"/api/show", kGet, ApiClass::B},
  {"/api/looks", kGet, ApiClass::B},
  {"/api/looks/add", kPost, ApiClass::B},
  {"/api/looks/remove", kPost, ApiClass::B},
  {"/api/looks/capture", kPost, ApiClass::B},
  {"/api/looks/fire", kPost, ApiClass::B},
  {"/api/layer", kPost, ApiClass::B},
  {"/api/layers/add", kPost, ApiClass::B},
  {"/api/layers/remove", kPost, ApiClass::B},
  {"/api/lfos", kPost, ApiClass::B},
  {"/api/sets", kPost, ApiClass::B},
  {"/api/fan", kPost, ApiClass::B},
  {"/api/fixtures/limits", kPost, ApiClass::B},
  {"/api/profiles/add", kPost, ApiClass::B},
  {"/api/profiles/remove", kPost, ApiClass::B},
  {"/api/output/send", kPost, ApiClass::B},
  {"/api/output/send/remove", kPost, ApiClass::B},
  {"/api/nodes/add", kPost, ApiClass::B},
  {"/api/nodes/remove", kPost, ApiClass::B},
  {"/api/discover", kPost, ApiClass::B},
  {"/api/serial/identify", kPost, ApiClass::B},
  {"/api/audio", kPost, ApiClass::B},
  {"/api/audiocfg", kPost, ApiClass::B},
  {"/api/library", kGet, ApiClass::C},
  {"/api/library/manufacturer", kGet, ApiClass::C},
  {"/api/library/fixture", kGet, ApiClass::C},
  {"/api/library/import", kPost, ApiClass::C},
  {"/api/midi", kGet | kPost, ApiClass::C},
  {"/api/show/open", kPost, ApiClass::C},
  {"/api/show/copy-machine", kPost, ApiClass::C},
};
constexpr size_t kDeskApiCount = sizeof(kDeskApi) / sizeof(kDeskApi[0]);

// Paths served under the same gate that are NOT in the vendored contract (the
// UI of contract v1 never calls them; plan step V5, caps.stream tells a newer
// UI to). tools/check_routes.py holds them apart from kDeskApi so the contract
// comparison stays exact.
constexpr ApiRoute kDeskExtra[] = {
  {"/api/dmx/stream", kGet, ApiClass::A},
};
constexpr size_t kDeskExtraCount = sizeof(kDeskExtra) / sizeof(kDeskExtra[0]);

inline const ApiRoute* findDeskApi(const char* path) {
  if (!path) return nullptr;
  for (size_t i = 0; i < kDeskExtraCount; i++)
    if (strcmp(kDeskExtra[i].path, path) == 0) return &kDeskExtra[i];
  for (size_t i = 0; i < kDeskApiCount; i++)
    if (strcmp(kDeskApi[i].path, path) == 0) return &kDeskApi[i];
  return nullptr;
}

inline const char* deskMethods(uint8_t m) {
  return m == kGet ? "GET" : (m == kPost ? "POST" : "GET, POST");
}

// The media type of a Content-Type value is application/json, compared
// case-insensitively, optionally followed by parameters (RFC 9110 §8.3.1:
// "application/json; charset=utf-8"). Nothing else counts, not +json types.
inline bool isJsonContentType(const char* v) {
  if (!v) return false;
  while (*v == ' ' || *v == '\t') v++;
  static const char kType[] = "application/json";
  for (size_t i = 0; i < sizeof(kType) - 1; i++) {
    char c = v[i];
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c != kType[i]) return false;
  }
  const char* t = v + sizeof(kType) - 1;
  while (*t == ' ' || *t == '\t') t++;
  return *t == 0 || *t == ';';
}

// The DeskApi gate. OPTIONS: 403 without ACAO (the browser drops the request).
// A method the path lacks: 405 + Allow. POST without application/json: 415.
inline Gate gateDesk(uint8_t methods, HttpMethod m, bool jsonBody) {
  switch (m) {
    case HttpMethod::Options: return Gate::PreflightDenied;
    case HttpMethod::Get: return (methods & kGet) ? Gate::Allow : Gate::NotAllowed;
    case HttpMethod::Post:
      if (!(methods & kPost)) return Gate::NotAllowed;
      return jsonBody ? Gate::Allow : Gate::NeedJson;
    default: return Gate::NotAllowed;
  }
}

constexpr size_t kMaxBody = 2048;              // larger bodies answer 413
constexpr uint32_t kMinFreeBlock = 32768;      // below this largest free block: 503 "node busy"
constexpr const char* kNotOnNode = "not on this node";
constexpr uint32_t kFadeMaxMs = 600000;        // = main.cpp FADE_MAX_MS
constexpr uint8_t kSceneNameMax = 23;          // bytes, NVS sN.name holds at most this
constexpr uint8_t kFixtureNameMax = 15;        // NodeFixture::name minus the NUL

// ── Body rules ────────────────────────────────────────────────────────────────
// An id the node printed: one prefix letter, then 1..max with no sign, no
// leading zero, digits only ("f3" -> 2). Anything else is -1.
inline int parseId(const char* s, char prefix, uint32_t max) {
  if (!s || s[0] != prefix || s[1] < '1' || s[1] > '9') return -1;
  uint32_t v = 0;
  for (const char* p = s + 1; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
    v = v * 10 + uint32_t(*p - '0');
    if (v > max) return -1;
  }
  return int(v) - 1;
}

// A name from a body: ASCII control bytes dropped, spaces trimmed at both
// ends, at most cap - 1 bytes kept and never half a UTF-8 sequence. Returns
// the length; 0 means "no usable name" (the caller keeps its default).
inline size_t cleanName(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  if (in) {
    while (*in == ' ' || *in == '\t') in++;
    for (; *in && n + 1 < cap; in++) {
      unsigned char c = (unsigned char)*in;
      if (c < 0x20 || c == 0x7f) continue;
      out[n++] = char(c);
    }
  }
  // Back off a cut multi-byte sequence at the end.
  if (n) {
    size_t j = n - 1;
    while (j > 0 && ((unsigned char)out[j] & 0xC0) == 0x80) j--;
    unsigned char lead = (unsigned char)out[j];
    size_t need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    if (j + need > n) n = j;
  }
  while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
  out[n] = 0;
  return n;
}

// Numbers from a body. NaN and negatives go to 0; fades stop at kFadeMaxMs
// (the node's fade engine limit), stage positions are di's 0..1.
inline uint32_t clampFadeMs(double v) {
  if (!(v > 0)) return 0;
  if (v >= double(kFadeMaxMs)) return kFadeMaxMs;
  return uint32_t(v + 0.5);
}
inline uint8_t unitToByte(double v) {
  if (!(v > 0)) return 0;
  if (v >= 1) return 255;
  return posFromMilli(uint32_t(v * 1000 + 0.5));
}
inline int32_t levelFrom(double v) {
  if (!(v > 0)) return 0;
  if (v >= 255) return 255;
  return int32_t(v + 0.5);
}

// The listed fixture (outside `ignore`, a slot bit mask) that shares a channel
// with [start, start + width - 1], or -1.
inline int overlapSlot(const NodeFixture* fx, uint8_t n, uint16_t start, uint8_t width, uint32_t ignore) {
  if (!width) return -1;
  uint32_t end = uint32_t(start) + width - 1;
  for (uint8_t k = 0; fx && k < n; k++) {
    if ((ignore >> k) & 1u) continue;
    if (!fixtureListed(fx[k])) continue;
    if (fx[k].start <= end && fixtureEnd(fx[k]) >= start) return k;
  }
  return -1;
}

// The lowest address where `width` channels fit inside 1..512 without touching
// a listed fixture; 0 when the universe has no such gap.
inline uint16_t nextFreeAddress(const NodeFixture* fx, uint8_t n, uint8_t width) {
  if (!width || width > kChannels) return 0;
  uint16_t a = 1;
  while (uint32_t(a) + width - 1 <= kChannels) {
    int hit = overlapSlot(fx, n, a, width, 0);
    if (hit < 0) return a;
    a = uint16_t(fixtureEnd(fx[hit]) + 1);
  }
  return 0;
}

// groups/add stores a channel range, and membership is read back as "listed
// fixtures wholly inside the range" (rule 7). So a selection is storable only
// when that rule gives back exactly the selection: the range is
// [lowest start, highest end] of the selected fixtures, and it is refused when
// any listed fixture outside the selection lies wholly inside it.
enum class GroupFit : uint8_t { Ok, Empty, NotContiguous };
inline GroupFit groupRangeFor(const NodeFixture* fx, uint8_t n, uint32_t selected, uint16_t* start, uint16_t* end) {
  uint16_t lo = 0xFFFF, hi = 0;
  for (uint8_t k = 0; fx && k < n; k++) {
    if (!((selected >> k) & 1u) || !fixtureListed(fx[k])) continue;
    if (fx[k].start < lo) lo = fx[k].start;
    if (fixtureEnd(fx[k]) > hi) hi = fixtureEnd(fx[k]);
  }
  if (hi == 0) return GroupFit::Empty;
  for (uint8_t k = 0; k < n; k++) {
    if ((selected >> k) & 1u) continue;
    if (fixtureListed(fx[k]) && fixtureInRange(fx[k], lo, hi)) return GroupFit::NotContiguous;
  }
  if (start) *start = lo;
  if (end) *end = hi;
  return GroupFit::Ok;
}

// Slot for a new group: the first disabled slot, else the first enabled slot
// whose range holds no listed fixture (not shown, see groupListed); -1 = full.
inline int freeGroupSlot(const DeskFacts& d) {
  for (uint8_t g = 0; d.groups && g < d.groupSlots; g++)
    if (!d.groups[g].enabled) return g;
  for (uint8_t g = 0; d.groups && g < d.groupSlots; g++)
    if (!detail::groupListed(d, g)) return g;
  return -1;
}

// What identify flashes: the fixture's level and emitter roles (dimmer and
// colour), so a fixture with no dimmer channel still lights. Returns the
// count of 0-based channels written to out (at most kMaxProfileChannels).
inline uint8_t identifyChannels(const NodeFixture& f, uint16_t* out) {
  const Profile* p = profileAt(f.profile);
  uint8_t n = 0;
  for (uint8_t k = 0; p && k < p->count; k++) {
    Kind kd = kRoles[uint8_t(p->roles[k])].kind;
    if (kd != Kind::Level && kd != Kind::Emitter) continue;
    int ch = roleChannel(f, p->roles[k]);
    if (ch >= 0) out[n++] = uint16_t(ch);
  }
  return n;
}

// ── api/input facts ───────────────────────────────────────────────────────────
// The node's source mode in di's words: input "enabled" = the network sources
// (Art-Net, sACN, USB widget) are used at all; the universe line's "desk" is
// follow (network only) or htp (merged with the web layer); "loss" release =
// fall back to the web layer, hold = keep the last network frame.
struct InputFacts {
  uint8_t mode;              // 0 web only, 1 network only, 2 HTP (main.cpp Mode)
  bool fallbackToWeb;
  uint16_t universe;         // Art-Net port-address the node listens on
  uint16_t sacnUniverse;     // = universe + 1 (E1.31 has no universe 0)
  bool artActive, sacnActive, usbActive;
  uint8_t artSources, sacnSources;
  int32_t artAgeMs, sacnAgeMs, usbAgeMs;   // since the last frame; -1 = none since boot
  bool held;                 // network only, no signal, holding the last frame
  const char* name;          // node name (di's input "name")
};

namespace detail {

inline void portAddressLabel(char* out, uint16_t pa) {   // di's "net:sub:uni"
  unsigned net = (pa >> 8) & 0x7f, sub = (pa >> 4) & 0x0f, uni = pa & 0x0f;
  size_t n = 0;
  if (net >= 100) out[n++] = char('0' + net / 100);
  if (net >= 10) out[n++] = char('0' + net / 10 % 10);
  out[n++] = char('0' + net % 10);
  out[n++] = ':';
  if (sub >= 10) out[n++] = '1';
  out[n++] = char('0' + sub % 10);
  out[n++] = ':';
  if (uni >= 10) out[n++] = '1';
  out[n++] = char('0' + uni % 10);
  out[n] = 0;
}

inline void inputSource(JsonWriter& w, const char* key, const char* protocol, const char* name, bool active,
                        int32_t ageMs, uint8_t sources) {
  w.obj();
  w.key("key"); w.str(key);
  w.key("protocol"); w.str(protocol);
  w.key("name"); w.str(name);
  w.key("ip"); w.str("");
  w.key("priority"); w.u(100);
  w.key("fps"); w.u(0);              // not counted on the node
  w.key("ageMs"); w.u(uint32_t(ageMs));
  w.key("packets"); w.u(0);
  w.key("outOfSequence"); w.u(0);
  w.key("sources"); w.u(sources);
  w.key("winning"); w.boolean(active);
  w.key("lost"); w.boolean(!active);
  w.key("lostWhy"); w.null();
  w.endObj();
}

inline const char* lineState(const InputFacts& in) {
  if (in.artActive || in.sacnActive || in.usbActive) return "live";
  if (in.held) return "held";
  if (in.artAgeMs >= 0 || in.sacnAgeMs >= 0 || in.usbAgeMs >= 0) return "lost";
  return "waiting";
}

}  // namespace detail

// GET/POST api/input: di's input status (config, interfaces, listening, lines,
// summary) built from the node's source facts.
inline bool writeInput(JsonWriter& w, const DeskFacts& d) {
  static const InputFacts kOff = {0, true, 0, 1, false, false, false, 0, 0, -1, -1, -1, false, nullptr};
  const InputFacts& in = d.input ? *d.input : kOff;
  const bool enabled = in.mode != 0;
  const char* deskMode = in.mode == 1 ? "follow" : "htp";
  const char* state = detail::lineState(in);
  w.obj();
  w.key("config"); w.obj();
  w.key("enabled"); w.boolean(enabled);
  w.key("artnet"); w.boolean(true);
  w.key("sacn"); w.boolean(true);
  w.key("interfaces"); w.arr();
  for (uint8_t k = 0; d.interfaces && k < d.interfaceCount; k++) w.str(detail::orStr(d.interfaces[k].address, ""));
  w.endArr();
  w.key("universes"); w.arr();
  w.obj(); w.key("universe"); w.u(in.universe); w.key("merge"); w.str("htp"); w.key("desk"); w.str(deskMode); w.endObj();
  w.endArr();
  w.key("loss"); w.str(in.fallbackToWeb ? "release" : "hold");
  w.key("name"); w.str(detail::orStr(in.name, detail::orStr(d.nodeName, "node")));
  w.endObj();

  w.key("interfaces"); w.arr();
  for (uint8_t k = 0; d.interfaces && k < d.interfaceCount; k++) {
    w.obj();
    w.key("iface"); w.str(detail::orStr(d.interfaces[k].iface, ""));
    w.key("address"); w.str(detail::orStr(d.interfaces[k].address, ""));
    w.key("internal"); w.boolean(false);
    w.key("netmask"); w.str(detail::orStr(d.interfaces[k].netmask, ""));
    w.endObj();
  }
  w.endArr();

  // The node's sockets are bound in every mode; mode only decides whether the
  // frames are used.
  char group[20] = "239.255.";
  {
    size_t n = 8;
    uint8_t hi = uint8_t(in.sacnUniverse >> 8), lo = uint8_t(in.sacnUniverse);
    uint8_t parts[2] = {hi, lo};
    for (uint8_t i = 0; i < 2; i++) {
      uint8_t v = parts[i];
      if (v >= 100) group[n++] = char('0' + v / 100);
      if (v >= 10) group[n++] = char('0' + v / 10 % 10);
      group[n++] = char('0' + v % 10);
      if (i == 0) group[n++] = '.';
    }
    group[n] = 0;
  }
  w.key("listening"); w.arr();
  w.obj();
  w.key("protocol"); w.str("artnet"); w.key("address"); w.str("0.0.0.0"); w.key("port"); w.u(6454);
  w.key("ok"); w.boolean(true); w.key("error"); w.null(); w.key("broadcast"); w.boolean(true);
  w.endObj();
  w.obj();
  w.key("protocol"); w.str("sacn"); w.key("address"); w.str("0.0.0.0"); w.key("port"); w.u(5568);
  w.key("ok"); w.boolean(true); w.key("error"); w.null();
  w.key("groups"); w.arr(); w.str(group); w.endArr();
  w.key("groupErrors"); w.rawValue("[]");
  w.endObj();
  w.endArr();

  char label[12];
  detail::portAddressLabel(label, in.universe);
  int32_t age = -1;
  const int32_t ages[3] = {in.artAgeMs, in.sacnAgeMs, in.usbAgeMs};
  for (int32_t a : ages) if (a >= 0 && (age < 0 || a < age)) age = a;
  w.key("lines"); w.arr();
  w.obj();
  w.key("universe"); w.u(in.universe);
  w.key("merge"); w.str("htp");
  w.key("desk"); w.str(deskMode);
  w.key("artnet"); w.str(label);
  w.key("sacn"); w.u(in.sacnUniverse);
  w.key("state"); w.str(state);
  w.key("lastAt"); w.null();
  w.key("ageMs"); if (age >= 0) w.u(uint32_t(age)); else w.null();
  w.key("sources"); w.arr();
  if (in.artAgeMs >= 0) detail::inputSource(w, "artnet", "artnet", "Art-Net", in.artActive, in.artAgeMs, in.artSources);
  if (in.sacnAgeMs >= 0) detail::inputSource(w, "sacn", "sacn", "sACN", in.sacnActive, in.sacnAgeMs, in.sacnSources);
  if (in.usbAgeMs >= 0) detail::inputSource(w, "usb", "usb", "USB widget", in.usbActive, in.usbAgeMs, 1);
  w.endArr();
  w.key("exceeded"); w.null();
  w.endObj();
  w.endArr();
  w.key("unlistened"); w.rawValue("[]");
  w.key("stats"); w.rawValue("{}");
  w.key("problems"); w.rawValue("[]");
  w.key("foreign"); w.u(0);
  w.key("polls"); w.null();

  const bool live = !strcmp(state, "live");
  w.key("summary"); w.obj();
  w.key("enabled"); w.boolean(enabled);
  if (!enabled) { w.key("text"); w.str("Input off"); }
  else {
    w.key("text");
    if (live) {
      char t[64] = "Following ";
      size_t n = 10;
      const char* names[3] = {in.artActive ? "Art-Net" : nullptr, in.sacnActive ? "sACN" : nullptr,
                              in.usbActive ? "USB widget" : nullptr};
      bool first = true;
      for (const char* nm : names) {
        if (!nm) continue;
        if (!first) { t[n++] = ','; t[n++] = ' '; }
        size_t l = strlen(nm);
        memcpy(t + n, nm, l); n += l;
        first = false;
      }
      memcpy(t + n, " on 1 of 1 universe", 20);
      w.str(t);
    } else if (!strcmp(state, "held")) w.str("No signal — holding the last look");
    else if (!strcmp(state, "lost")) w.str("No signal — the desk has the rig");
    else w.str("Listening — no signal yet");
    w.key("level"); w.str(live ? "ok" : "warn");
    w.key("live"); w.u(live ? 1 : 0);
    w.key("universes"); w.u(1);
  }
  w.endObj();
  w.key("lanAllowed"); w.boolean(true);
  w.endObj();
  return w.finish();
}

// ── POST replies ──────────────────────────────────────────────────────────────
enum class Reply : uint8_t {
  State, Dmx, Input, Cues, Show, Looks,      // GET
  Ok, Error, CuesRefused,                    // {ok}, {error}, {error, cues}
  Fixture, Added, Moved, Removed, Group, Scene, Chase, Fx, Output, Raw, Identify,
};

struct ReplyArgs {
  Reply kind;
  const char* error;    // Error, CuesRefused: the refusal text (a literal)
  uint32_t slots;       // Added, Moved, Identify: fixture slot bits
  uint8_t slot;         // Fixture, Group, Scene
  uint8_t count;        // Removed
  bool chasePaused;     // Ok after scenes/recall stopped the cue runner
};

inline bool writeReply(JsonWriter& w, const DeskFacts& d, const ReplyArgs& a) {
  switch (a.kind) {
    case Reply::State: return writeState(w, d);
    case Reply::Dmx: return writeDmx(w, d);
    case Reply::Input: return writeInput(w, d);
    case Reply::Cues: return writeCues(w);
    case Reply::Show: return writeShow(w, d);
    case Reply::Looks: return writeLooks(w);
    case Reply::Error:
      w.obj(); w.key("error"); w.str(detail::orStr(a.error, "refused")); w.endObj();
      return w.finish();
    case Reply::CuesRefused:
      w.obj(); w.key("error"); w.str(detail::orStr(a.error, kNotOnNode));
      w.key("cues"); detail::cues(w); w.endObj();
      return w.finish();
    default: break;
  }
  w.obj();
  w.key("ok"); w.boolean(true);
  switch (a.kind) {
    case Reply::Ok:
      if (a.chasePaused) { w.key("chasePaused"); w.boolean(true); }
      break;
    case Reply::Fixture:
      w.key("fixture");
      if (d.fixtures && a.slot < d.fixtureSlots) detail::fixture(w, d.fixtures[a.slot], a.slot, d.web);
      else w.null();
      break;
    case Reply::Added:
    case Reply::Moved:
    case Reply::Identify:
      w.key(a.kind == Reply::Added ? "added" : a.kind == Reply::Moved ? "moved" : "identifying");
      w.arr();
      for (uint8_t k = 0; d.fixtures && k < d.fixtureSlots && k < 32; k++) {
        if (!((a.slots >> k) & 1u)) continue;
        if (a.kind == Reply::Added) detail::fixture(w, d.fixtures[k], k, d.web);
        else if (a.kind == Reply::Identify) w.id('f', k + 1u);
        else {
          w.obj();
          w.key("id"); w.id('f', k + 1u);
          w.key("universe"); w.u(0);
          w.key("address"); w.u(d.fixtures[k].start);
          w.endObj();
        }
      }
      w.endArr();
      break;
    case Reply::Removed:
      w.key("removed"); w.u(a.count);
      w.key("groupsRemoved"); w.rawValue("[]");
      break;
    case Reply::Group:
      w.key("group");
      if (d.groups && a.slot < d.groupSlots) detail::group(w, d, a.slot); else w.null();
      break;
    case Reply::Scene:
      w.key("scene");
      if (d.scenes && a.slot < d.sceneSlots && d.scenes[a.slot]) detail::scene(w, d, a.slot); else w.null();
      break;
    case Reply::Chase: w.key("chase"); detail::chase(w, d); break;
    case Reply::Fx: w.key("fx"); detail::fx(w, d); break;
    case Reply::Output:
      w.key("output"); detail::output(w, d);
      w.key("serial"); w.null();
      w.key("error"); w.null();
      break;
    case Reply::Raw: w.key("raw"); w.rawValue("{}"); break;
    default: break;
  }
  w.endObj();
  return w.finish();
}

// One pull of any reply (the WindowSink re-run of pullWindow).
inline size_t pullReply(const DeskFacts& d, const ReplyArgs& a, JsonWriter& w, uint8_t* buf, size_t maxLen,
                        size_t index) {
  WindowSink s = {reinterpret_cast<char*>(buf), maxLen, index, 0, 0};
  if (maxLen == 0) return 0;
  w.begin(windowSink, &s);
  writeReply(w, d, a);
  return s.written;
}

}  // namespace desk
}  // namespace vizzz
