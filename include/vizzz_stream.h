#pragma once

// ── GET /api/dmx/stream: the pure part (plan step V5) ─────────────────────────
// The event text di's UI reads (di.iiii serverXR/src/lighting/dmxstream.js, read
// by src/rigMirror/useLightingMirror.js):
//   event: frame
//   data: {"s":seq,"t":ms,"k":1|0,"d":[[0,start,[v,v,...]],...],"m":{...}}
// The library prints the `event:` / `data:` lines and the blank line; this file
// writes the JSON object after `data: `. One universe (0), key frame = all 512
// slots, delta = the runs of slots that changed since the frame before, runs
// closer than kGap slots joined (dmxstream.js deltaRuns). `m` (master,
// blackout, looks, cues: what /api/dmx carries besides the buffer) rides on a
// key frame and on a delta where it changed.
//
// What differs from di's own stream, on purpose:
//  - one frame for every client (the library queues one shared message), so `s`
//    counts events of the node, not per client, and a client joins through a key
//    frame that every client then receives;
//  - the keepalive is a key frame once a second, not a comment every 15 s: it
//    keeps proxies quiet and heals a client whose queue overflowed;
//  - `t` is the node's uptime in ms (it has no wall clock); only a look's `since`
//    uses it, and the node has no looks.
// Pure code: no Arduino, no heap.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace vizzz {
namespace desk {
namespace stream {

constexpr uint16_t kSlots = 512;
constexpr uint32_t kPeriodMs = 50;        // 20 Hz: the fastest the stream may send
constexpr uint32_t kKeepaliveMs = 1000;   // an idle frame is repeated (as a key frame) this often
constexpr uint8_t kMaxClients = 4;        // the 5th gets 503 {"error":"stream full"}
constexpr uint8_t kGap = 6;               // dmxstream.js GAP: join runs separated by fewer unchanged slots
constexpr uint32_t kRetryMs = 1000;       // dmxstream.js RETRY_MS
constexpr size_t kBufSize = 2304;         // a key frame is at most ~2.2 KB (512 x "255," + header + meta)
constexpr const char* kFullError = "stream full";
constexpr const char* kEvent = "frame";

static_assert(kPeriodMs >= 50 && kPeriodMs <= 100, "the stream runs between 10 and 20 Hz");
static_assert(kKeepaliveMs >= kPeriodMs, "a keepalive is not faster than the stream");

// The cap: a new client is refused when this many are already connected or
// being accepted.
inline bool full(size_t connected, size_t pending) { return connected + pending >= kMaxClients; }

struct Meta {
  uint8_t master;
  bool blackout;
  bool operator==(const Meta& o) const { return master == o.master && blackout == o.blackout; }
  bool operator!=(const Meta& o) const { return !(*this == o); }
};

enum class Send : uint8_t { Nothing, Delta, Key };

// The rate rule: true when a tick is due (wrap-safe).
inline bool tickDue(uint32_t now, uint32_t lastTick) { return uint32_t(now - lastTick) >= kPeriodMs; }

// The send rule: a key frame when asked for or when the last send is a second
// old; a delta when the frame or the meta changed; otherwise nothing.
inline Send decide(bool frameChanged, bool metaChanged, bool wantKey, uint32_t now, uint32_t lastSend) {
  if (wantKey || uint32_t(now - lastSend) >= kKeepaliveMs) return Send::Key;
  return (frameChanged || metaChanged) ? Send::Delta : Send::Nothing;
}

// ── Writer into a fixed buffer ───────────────────────────────────────────────
struct Out {
  char* p;
  size_t cap;
  size_t n;
  bool ok;
  void raw(const char* s, size_t len) {
    if (!ok || n + len >= cap) { ok = false; return; }
    memcpy(p + n, s, len);
    n += len;
    p[n] = 0;
  }
  void lit(const char* s) { raw(s, strlen(s)); }
  void num(uint32_t v) {
    char t[11];
    size_t k = 0;
    do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
    char r[11];
    for (size_t i = 0; i < k; i++) r[i] = t[k - 1 - i];
    raw(r, k);
  }
  void ch(char c) { raw(&c, 1); }
};

inline void writeMeta(Out& o, const Meta& m) {
  o.lit("\"m\":{\"master\":");
  o.num(m.master);
  o.lit(m.blackout ? ",\"blackout\":true" : ",\"blackout\":false");
  o.lit(",\"looks\":[],\"cues\":null}");
}

inline void writeHead(Out& o, uint32_t seq, uint32_t t, bool key) {
  o.lit("{\"s\":");
  o.num(seq);
  o.lit(",\"t\":");
  o.num(t);
  o.lit(key ? ",\"k\":1,\"d\":[" : ",\"k\":0,\"d\":[");
}

// One key frame: all 512 slots as one run, then the meta. Returns the length,
// or 0 when the buffer is too small.
inline size_t formatKey(char* dst, size_t cap, uint32_t seq, uint32_t t, const uint8_t* frame, const Meta& m) {
  Out o = {dst, cap, 0, true};
  if (cap) dst[0] = 0;
  writeHead(o, seq, t, true);
  o.lit("[0,0,[");
  for (uint16_t i = 0; i < kSlots; i++) {
    if (i) o.ch(',');
    o.num(frame[i]);
  }
  o.lit("]]],");
  writeMeta(o, m);
  o.ch('}');
  return o.ok ? o.n : 0;
}

// One delta against `prev`: the runs of changed slots (dmxstream.js deltaRuns:
// a slot that differs starts or extends a run when it is at most kGap after the
// run's last changed slot; the unchanged slots inside are sent too). `m` only
// when withMeta. Returns the length, or 0 when the buffer is too small (the
// caller then sends a key frame, which always fits).
inline size_t formatDelta(char* dst, size_t cap, uint32_t seq, uint32_t t, const uint8_t* prev,
                          const uint8_t* next, const Meta& m, bool withMeta) {
  Out o = {dst, cap, 0, true};
  if (cap) dst[0] = 0;
  writeHead(o, seq, t, false);
  int start = -1, last = -1;
  bool first = true;
  auto flush = [&]() {
    if (start < 0) return;
    if (!first) o.ch(',');
    first = false;
    o.lit("[0,");
    o.num(uint32_t(start));
    o.lit(",[");
    for (int i = start; i <= last; i++) {
      if (i != start) o.ch(',');
      o.num(next[i]);
    }
    o.lit("]]");
  };
  for (int i = 0; i < kSlots; i++) {
    if (prev[i] == next[i]) continue;
    if (start >= 0 && i - last <= kGap) { last = i; continue; }
    flush();
    start = i;
    last = i;
  }
  flush();
  o.ch(']');
  if (withMeta) { o.ch(','); writeMeta(o, m); }
  o.ch('}');
  return o.ok ? o.n : 0;
}

// ── The sender's state (loop() owns it; nothing here is shared) ──────────────
struct Streamer {
  uint8_t sent[kSlots];
  Meta sentMeta;
  bool primed;       // `sent` holds a frame a client has been given
  uint32_t seq;
  uint32_t lastTick;
  uint32_t lastSend;

  void reset() {
    memset(sent, 0, sizeof(sent));
    sentMeta = Meta{0, false};
    primed = false;
    seq = 0;
    lastTick = 0;
    lastSend = 0;
  }

  // Forget what clients were given: the next event is a key frame (no client,
  // a queue overflow, a heap short).
  void forget() { primed = false; }

  // One pass of loop(). Returns the length of the JSON in buf, 0 for nothing
  // to send (not due, nothing changed, or buf too small: state untouched).
  size_t step(uint32_t now, const uint8_t* frame, const Meta& meta, bool wantKey, char* buf, size_t cap) {
    if (primed && !tickDue(now, lastTick)) return 0;
    lastTick = now;
    const bool frameChanged = !primed || memcmp(sent, frame, kSlots) != 0;
    const bool metaChanged = !primed || meta != sentMeta;
    const Send what = decide(frameChanged, metaChanged, wantKey || !primed, now, lastSend);
    if (what == Send::Nothing) return 0;
    const uint32_t next = seq + 1;
    size_t n = 0;
    if (what == Send::Delta) n = formatDelta(buf, cap, next, now, sent, frame, meta, metaChanged);
    if (n == 0) n = formatKey(buf, cap, next, now, frame, meta);
    if (n == 0) return 0;
    seq = next;
    memcpy(sent, frame, kSlots);
    sentMeta = meta;
    primed = true;
    lastSend = now;
    return n;
  }
};

}  // namespace stream
}  // namespace desk
}  // namespace vizzz
