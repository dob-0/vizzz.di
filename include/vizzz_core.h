#pragma once

#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace vizzz {

constexpr uint16_t packUniverse(uint8_t artNet, uint8_t artSubnet, uint8_t artUni) {
  return (uint16_t(artNet) << 8) | (uint16_t(artSubnet) << 4) | (artUni & 0x0F);
}

constexpr uint8_t applyMaster(uint8_t value, uint8_t master) {
  return master >= 255 ? value : uint8_t((uint16_t(value) * master) >> 8);
}

constexpr uint8_t clampLevel(uint8_t value, uint8_t ceiling) {
  return value > ceiling ? ceiling : value;
}

inline uint8_t slewToward(uint8_t current, uint8_t target, uint8_t step) {
  if (step == 0 || current == target) return target;
  if (target > current) {
    uint16_t next = uint16_t(current) + step;
    return next > target ? target : uint8_t(next);
  }
  uint16_t down = uint16_t(target) + step;
  return down < current ? uint8_t(current - step) : target;
}

// ── DMX channel ranges ────────────────────────────────────────────────────────
// 1-based inclusive range repaired into 1..maxCh with end >= start (the
// re-clamp loadVj() applies to groups; used for stored fixtures).
inline void clampChannelRange(uint16_t& start, uint16_t& end, uint16_t maxCh) {
  if (start < 1) start = 1;
  if (start > maxCh) start = maxCh;
  if (end < start) end = start;
  if (end > maxCh) end = maxCh;
}

// ── Wi-Fi credentials ─────────────────────────────────────────────────────────
// IEEE 802.11i-2004 Annex H.4.1 (carried into IEEE 802.11-2020 Annex J.4.1): a
// WPA/WPA2-Personal pass-phrase is 8..63 ASCII characters, each in 32..126.
// arduino-esp32 2.0.17 WiFi.softAP() returns false for 1..7 chars
// (WiFiAP.cpp:145) and opens the AP for an empty one; neither may be stored.
inline bool isValidWpaPassphrase(const char* s, size_t len) {
  if (!s || len < 8 || len > 63) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 32 || c > 126) return false;
  }
  return true;
}

// ── Host name ─────────────────────────────────────────────────────────────────
// RFC 1123 §2.1 / RFC 952 host label: letters, digits and '-', no leading or
// trailing '-'. Everything else becomes '-'. Output is NUL-terminated and at
// most outSize-1 chars; an empty result becomes `fallback`. Returns the length.
inline size_t sanitizeHostLabel(const char* in, char* out, size_t outSize, const char* fallback) {
  if (!out || outSize == 0) return 0;
  size_t w = 0;
  for (size_t i = 0; in && in[i] != '\0' && w + 1 < outSize; i++) {
    char c = in[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    char o = ok ? c : '-';
    if (w == 0 && o == '-') continue;  // no leading hyphen
    out[w++] = o;
  }
  while (w > 0 && out[w - 1] == '-') w--;  // no trailing hyphen
  out[w] = '\0';
  if (w == 0 && fallback) {
    for (; fallback[w] != '\0' && w + 1 < outSize; w++) out[w] = fallback[w];
    out[w] = '\0';
  }
  return w;
}

// ── IPv4 text ─────────────────────────────────────────────────────────────────
// Strict dotted quad "a.b.c.d", each 0..255 in decimal, nothing else.
inline bool parseIpv4(const char* s, uint8_t out[4]) {
  if (!s) return false;
  for (int part = 0; part < 4; part++) {
    if (*s < '0' || *s > '9') return false;
    uint16_t v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
      v = uint16_t(v * 10 + (*s - '0'));
      if (++digits > 3 || v > 255) return false;
      s++;
    }
    out[part] = uint8_t(v);
    if (part < 3) {
      if (*s != '.') return false;
      s++;
    }
  }
  return *s == '\0';
}

// ── Cue timing ────────────────────────────────────────────────────────────────
// A cue step shorter than this would re-trigger a scene (an NVS read and a new
// fade) on every loop() pass — scenes alternating at frame rate (audit 1-M1).
constexpr uint32_t CUE_MIN_STEP_MS = 50;

// Clamp a requested dwell or fade (ms, possibly negative from toInt()) to 0..maxMs.
constexpr uint32_t clampCueTimeMs(long requested, uint32_t maxMs) {
  return requested <= 0 ? 0u : (uint32_t(requested) > maxMs ? maxMs : uint32_t(requested));
}

// Length of one cue step: fade + dwell, never below CUE_MIN_STEP_MS, never wraps.
constexpr uint32_t cueStepMs(uint32_t fadeMs, uint32_t dwellMs) {
  return (uint64_t(fadeMs) + dwellMs) < CUE_MIN_STEP_MS
             ? CUE_MIN_STEP_MS
             : ((uint64_t(fadeMs) + dwellMs) > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(fadeMs + dwellMs));
}

// True when at least `ms` have passed since `since`. Delta form, so a
// millis() wrap (every 49.7 days) is harmless.
constexpr bool elapsedAtLeast(uint32_t now, uint32_t since, uint32_t ms) {
  return uint32_t(now - since) >= ms;
}

// True when the running cue step is over.
constexpr bool cueStepDue(uint32_t now, uint32_t startedMs, uint32_t stepMs) {
  return elapsedAtLeast(now, startedMs, stepMs);
}

// ── STA re-join back-off ──────────────────────────────────────────────────────
// In AP+STA the soft-AP follows the STA's channel (ESP-IDF Wi-Fi guide,
// "Station/AP coexistence"), so every STA scan for an absent router moves the
// AP and drops the operator's phone (audit 1-M9). After a failed join, wait
// 30 s, then 60, 120, capped at 300 s, before the next attempt.
constexpr uint32_t STA_RETRY_BASE_MS = 30000;
constexpr uint32_t STA_RETRY_MAX_MS  = 300000;
inline uint32_t staRetryDelayMs(uint8_t failedJoins) {
  if (failedJoins == 0) return 0;
  uint32_t d = STA_RETRY_BASE_MS;
  for (uint8_t i = 1; i < failedJoins && d < STA_RETRY_MAX_MS; i++) d <<= 1;
  return d > STA_RETRY_MAX_MS ? STA_RETRY_MAX_MS : d;
}

// ── Art-Net 4 (Artistic Licence, "Art-Net 4 Specification", rev. 1.4) ─────────
// Pure packet parsing and building, no Arduino headers, so test/test_smoke
// covers it natively. Field offsets are the spec's packet tables.
namespace artnet {

constexpr uint16_t kPort        = 6454;    // 0x1936
constexpr uint16_t kOpPoll      = 0x2000;
constexpr uint16_t kOpPollReply = 0x2100;
constexpr uint16_t kOpDmx       = 0x5000;
constexpr uint16_t kProtVer     = 14;
constexpr size_t   kPollMinLen  = 14;      // Art-Net 4: ArtPoll is 14..22 bytes
constexpr size_t   kPollReplyLen = 239;
constexpr size_t   kDmxHeaderLen = 18;
// Our own Art OUT carries this in ArtDmx "Physical" (byte 13). A node whose Art
// OUT is on drops tagged packets, so two re-emitting nodes cannot latch.
constexpr uint8_t  kVizzzPhysicalTag = 0x76;  // 'v'

inline bool hasId(const uint8_t* p, size_t n) {
  static const uint8_t id[8] = {'A','r','t','-','N','e','t',0};
  return n >= 10 && memcmp(p, id, 8) == 0;
}

// OpCode (little-endian, bytes 8-9), or 0 when the packet is not Art-Net.
inline uint16_t opcode(const uint8_t* p, size_t n) {
  return hasId(p, n) ? uint16_t(p[8] | (uint16_t(p[9]) << 8)) : 0;
}

inline uint16_t protVer(const uint8_t* p) { return uint16_t((uint16_t(p[10]) << 8) | p[11]); }

inline bool isArtPoll(const uint8_t* p, size_t n) {
  return n >= kPollMinLen && opcode(p, n) == kOpPoll;
}

struct DmxView {
  uint16_t portAddress;   // 15-bit: Net(7) SubNet(4) Universe(4)
  uint8_t  sequence;      // 0 = sequencing off
  uint8_t  physical;
  const uint8_t* data;
  uint16_t length;
};

// ArtDmx: ID, OpDmx, ProtVer >= 14, Length 1..512 and inside the datagram.
inline bool parseDmx(const uint8_t* p, size_t n, DmxView& out) {
  if (n < kDmxHeaderLen || opcode(p, n) != kOpDmx) return false;
  if (protVer(p) < kProtVer) return false;
  uint16_t len = uint16_t((uint16_t(p[16]) << 8) | p[17]);
  if (len == 0 || len > 512 || kDmxHeaderLen + len > n) return false;
  out.sequence    = p[12];
  out.physical    = p[13];
  out.portAddress = uint16_t(p[14] | (uint16_t(p[15] & 0x7F) << 8));
  out.data        = p + kDmxHeaderLen;
  out.length      = len;
  return true;
}

// Builds the 18-byte ArtDmx header; data (len bytes) follows at out + 18.
inline void buildDmxHeader(uint8_t* out, uint16_t portAddress, uint8_t sequence,
                           uint8_t physical, uint16_t len) {
  static const uint8_t id[8] = {'A','r','t','-','N','e','t',0};
  memcpy(out, id, 8);
  out[8] = uint8_t(kOpDmx & 0xFF); out[9] = uint8_t(kOpDmx >> 8);
  out[10] = 0; out[11] = kProtVer;
  out[12] = sequence; out[13] = physical;
  out[14] = uint8_t(portAddress & 0xFF); out[15] = uint8_t((portAddress >> 8) & 0x7F);
  out[16] = uint8_t(len >> 8); out[17] = uint8_t(len & 0xFF);
}

// ArtDmx Sequence is 1..255 and skips 0 ("sequencing off").
inline uint8_t nextSequence(uint8_t s) { return s >= 255 ? 1 : uint8_t(s + 1); }

struct PollReplyInfo {
  uint8_t ip[4];
  uint8_t mac[6];
  uint8_t net, subnet, uni;     // node's Port-Address parts
  const char* shortName;        // up to 17 chars kept
  const char* longName;         // up to 63 chars kept
  const char* report;           // NodeReport text after "#0001 [nnnn] "
  uint16_t replyCount;          // NodeReport counter, wraps at 9999
  bool outputActive;            // GoodOutputA bit7: DMX is being transmitted
  bool merging;                 // GoodOutputA bit3: output merges two sources
  bool sacnSource;              // GoodOutputA bit0: output comes from sACN
  bool dhcp;                    // Status2 bit1: IP came from DHCP
  uint8_t refreshHz;            // max DMX refresh rate
};

// Art-Net 4 ArtPollReply, 239 bytes, field offsets per the spec's table.
inline size_t buildPollReply(uint8_t* o, const PollReplyInfo& in) {
  static const uint8_t id[8] = {'A','r','t','-','N','e','t',0};
  memset(o, 0, kPollReplyLen);
  memcpy(o, id, 8);
  o[8] = uint8_t(kOpPollReply & 0xFF); o[9] = uint8_t(kOpPollReply >> 8);
  memcpy(o + 10, in.ip, 4);                       // IP Address
  o[14] = uint8_t(kPort & 0xFF); o[15] = uint8_t(kPort >> 8);  // Port 0x1936, LE
  o[16] = 0; o[17] = 1;                           // VersInfo (firmware revision)
  o[18] = uint8_t(in.net & 0x7F);                 // NetSwitch
  o[19] = uint8_t(in.subnet & 0x0F);              // SubSwitch
  o[20] = 0x00; o[21] = 0xFF;                     // Oem: OemUnknown 0x00FF
  o[22] = 0;                                      // Ubea version
  o[23] = 0xE0;                                   // Status1: indicators normal, Port-Address set by network/web
  o[24] = 0xF0; o[25] = 0x7F;                     // EstaMan 0x7FF0 (ESTA prototype range), LE
  strncpy(reinterpret_cast<char*>(o + 26), in.shortName ? in.shortName : "", 17);
  strncpy(reinterpret_cast<char*>(o + 44), in.longName ? in.longName : "", 63);
  char* rep = reinterpret_cast<char*>(o + 108);   // NodeReport "#xxxx [yyyy] text"
  unsigned cnt = in.replyCount % 10000u;
  rep[0] = '#'; rep[1] = '0'; rep[2] = '0'; rep[3] = '0'; rep[4] = '1';  // RcPowerOk
  rep[5] = ' '; rep[6] = '[';
  rep[7] = char('0' + cnt / 1000); rep[8] = char('0' + cnt / 100 % 10);
  rep[9] = char('0' + cnt / 10 % 10); rep[10] = char('0' + cnt % 10);
  rep[11] = ']'; rep[12] = ' ';
  strncpy(rep + 13, in.report ? in.report : "", 63 - 13);
  o[172] = 0; o[173] = 1;                         // NumPorts = 1 (hi, lo)
  o[174] = 0x80;                                  // PortTypes[0]: output from Art-Net, DMX512
  o[178] = 0x08;                                  // GoodInput[0]: input disabled
  o[182] = uint8_t((in.outputActive ? 0x80 : 0) | (in.merging ? 0x08 : 0) |
                   (in.sacnSource ? 0x01 : 0));   // GoodOutputA[0]; merge mode HTP
  o[186] = uint8_t(in.uni & 0x0F);                // SwIn[0]
  o[190] = uint8_t(in.uni & 0x0F);                // SwOut[0]
  o[200] = 0x00;                                  // Style: StNode
  memcpy(o + 201, in.mac, 6);                     // MAC hi..lo
  memcpy(o + 207, in.ip, 4);                      // BindIp = own IP (root device)
  o[211] = 1;                                     // BindIndex: root
  o[212] = uint8_t(0x01 | 0x04 | 0x08 | (in.dhcp ? 0x02 : 0));  // Status2: web config, DHCP capable, 15-bit
  o[213] = 0x80;                                  // GoodOutputB[0]: RDM disabled
  o[217] = 0x40;                                  // Status3: failsafe = network layer to zero
  o[226] = 0; o[227] = in.refreshHz;              // RefreshRate (hi, lo)
  return kPollReplyLen;
}

// Two-source merge (Art-Net 4, "Data Merging"): a node merges two sources
// keyed by IP, ignores a third, and cancels a source after 10 s of silence.
// Sequence check uses the E1.31-2018 §6.7.2 window: drop when
// -20 < (new - last) <= 0 (8-bit signed), only if both are non-zero.
constexpr uint32_t kMergeTimeoutMs = 10000;

struct MergeSlot {
  uint32_t ip;
  uint32_t lastMs;
  uint8_t  seq;
  bool     used;
  uint8_t  data[512];
};

struct Merger {
  MergeSlot slot[2];
};

inline void mergerReset(Merger& m) { m.slot[0].used = false; m.slot[1].used = false; }

// Drops sources silent for longer than mergeTimeoutMs; drops all when every
// source is older than lossTimeoutMs (the node's network-loss failsafe).
inline void mergerExpire(Merger& m, uint32_t nowMs, uint32_t mergeTimeoutMs, uint32_t lossTimeoutMs) {
  bool anyFresh = false;
  for (auto& s : m.slot) {
    if (!s.used) continue;
    uint32_t age = nowMs - s.lastMs;
    if (age > mergeTimeoutMs) s.used = false;
    else if (age < lossTimeoutMs) anyFresh = true;
  }
  if (!anyFresh) mergerReset(m);
}

inline bool sequenceIsStale(uint8_t last, uint8_t now) {
  if (last == 0 || now == 0) return false;
  int8_t diff = int8_t(uint8_t(now - last));
  return diff <= 0 && diff > -20;
}

enum class MergeResult : uint8_t { Accepted, ThirdSource, OutOfOrder };

// Takes one ArtDmx frame from source ip; data shorter than 512 is zero-filled.
inline MergeResult mergerAccept(Merger& m, uint32_t ip, uint8_t seq, const uint8_t* data,
                                uint16_t len, uint32_t nowMs) {
  MergeSlot* s = nullptr;
  for (auto& c : m.slot) if (c.used && c.ip == ip) s = &c;
  if (s) {
    if (sequenceIsStale(s->seq, seq) && (nowMs - s->lastMs) < 1000) return MergeResult::OutOfOrder;
  } else {
    for (auto& c : m.slot) if (!c.used) { s = &c; break; }
    if (!s) return MergeResult::ThirdSource;
    s->used = true;
    s->ip = ip;
  }
  if (len > 512) len = 512;
  memcpy(s->data, data, len);
  if (len < 512) memset(s->data + len, 0, 512 - len);
  s->seq = seq;
  s->lastMs = nowMs;
  return MergeResult::Accepted;
}

inline uint8_t mergerSources(const Merger& m) {
  return uint8_t((m.slot[0].used ? 1 : 0) + (m.slot[1].used ? 1 : 0));
}

// HTP of the live sources into out[512]; returns the number of sources.
inline uint8_t mergerOutput(const Merger& m, uint8_t* out) {
  memset(out, 0, 512);
  uint8_t n = 0;
  for (const auto& s : m.slot) {
    if (!s.used) continue;
    n++;
    for (int i = 0; i < 512; i++) if (s.data[i] > out[i]) out[i] = s.data[i];
  }
  return n;
}

}  // namespace artnet

// ── sACN, ANSI E1.31-2018 ─────────────────────────────────────────────────────
namespace sacn {

constexpr uint16_t kPort = 5568;
constexpr uint32_t kLossTimeoutMs = 2500;   // §6.7.1 network data loss
constexpr uint8_t  kOptPreview    = 0x80;   // §6.2.6 Preview_Data
constexpr uint8_t  kOptTerminated = 0x40;   // §6.2.6 Stream_Terminated
constexpr uint8_t  kMaxPriority   = 200;    // §6.2.3
constexpr size_t   kHeaderLen     = 126;    // through the DMX start code

// Universe numbers 1..63999 are data universes; 0 and 64000+ are reserved
// (§6.2.7). The node listens on Art-Net Port-Address N, which di and most
// consoles map to sACN universe N + 1 (desk universe 1 = Art-Net 0 = sACN 1).
constexpr uint16_t universeForPortAddress(uint16_t portAddress) {
  return uint16_t((portAddress & 0x7FFF) + 1);
}

// IPv4 multicast group for a universe: 239.255.<hi>.<lo> (§9.3.1).
inline void multicastGroup(uint16_t universe, uint8_t out[4]) {
  out[0] = 239; out[1] = 255; out[2] = uint8_t(universe >> 8); out[3] = uint8_t(universe & 0xFF);
}

struct DataView {
  const uint8_t* cid;        // 16 bytes, root layer
  uint8_t  priority;         // clamped to 0..200
  uint16_t syncAddress;
  uint8_t  sequence;
  uint8_t  options;
  uint16_t universe;
  uint8_t  startCode;
  const uint8_t* data;       // slots after the start code
  uint16_t length;           // 0..512
};

inline uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// E1.31 data packet: root layer (§5), framing layer (§6), DMP layer (§7).
inline bool parseData(const uint8_t* p, size_t n, DataView& out) {
  static const uint8_t pid[12] = {0x41,0x53,0x43,0x2D,0x45,0x31,0x2E,0x31,0x37,0x00,0x00,0x00};
  if (n < kHeaderLen) return false;
  if (be16(p) != 0x0010 || be16(p + 2) != 0x0000) return false;     // preamble / postamble
  if (memcmp(p + 4, pid, 12) != 0) return false;                    // ACN packet identifier
  if (be32(p + 18) != 0x00000004) return false;                     // VECTOR_ROOT_E131_DATA
  if (be32(p + 40) != 0x00000002) return false;                     // VECTOR_E131_DATA_PACKET
  if (p[117] != 0x02) return false;                                 // VECTOR_DMP_SET_PROPERTY
  if (p[118] != 0xA1) return false;                                 // address & data type
  if (be16(p + 119) != 0x0000 || be16(p + 121) != 0x0001) return false;  // first address, increment
  uint16_t count = be16(p + 123);                                   // includes the start code
  if (count < 1 || count > 513 || size_t(125) + count > n) return false;
  uint16_t uni = be16(p + 113);
  if (uni == 0 || uni >= 64000) return false;                       // §6.2.7 reserved
  out.cid = p + 22;
  out.priority = p[108] > kMaxPriority ? kMaxPriority : p[108];
  out.syncAddress = be16(p + 109);
  out.sequence = p[111];
  out.options = p[112];
  out.universe = uni;
  out.startCode = p[125];
  out.data = p + 126;
  out.length = uint16_t(count - 1);
  return true;
}

// Source arbitration: two source slots keyed by CID. The highest priority
// wins (§6.2.3); sources at the same priority are merged HTP (§6.2.3.1 leaves
// the method to the receiver). A source is dropped after 2.5 s (§6.7.1) or at
// once on Stream_Terminated (§6.2.6). Out-of-order packets are discarded with
// the §6.7.2 sequence window. Preview packets never reach live output (§6.2.6).
struct Source {
  uint8_t  cid[16];
  uint32_t lastMs;
  uint8_t  seq;
  uint8_t  priority;
  bool     used;
  uint8_t  data[512];
};

struct Receiver {
  Source src[2];
};

enum class Result : uint8_t { Accepted, Preview, Terminated, OutOfOrder, NotDmx, Ignored };

inline void receiverReset(Receiver& r) { r.src[0].used = false; r.src[1].used = false; }

inline uint8_t receiverSources(const Receiver& r) {
  return uint8_t((r.src[0].used ? 1 : 0) + (r.src[1].used ? 1 : 0));
}

inline void receiverExpire(Receiver& r, uint32_t nowMs) {
  for (auto& s : r.src) if (s.used && (nowMs - s.lastMs) > kLossTimeoutMs) s.used = false;
}

inline bool sequenceIsStale(uint8_t last, uint8_t now) {
  int8_t diff = int8_t(uint8_t(now - last));
  return diff <= 0 && diff > -20;
}

inline Result receiverAccept(Receiver& r, const DataView& v, uint32_t nowMs) {
  if (v.options & kOptPreview) return Result::Preview;
  Source* s = nullptr;
  for (auto& c : r.src) if (c.used && memcmp(c.cid, v.cid, 16) == 0) s = &c;
  if (s && sequenceIsStale(s->seq, v.sequence)) return Result::OutOfOrder;
  if (v.options & kOptTerminated) {                 // data in this packet is not used
    if (s) s->used = false;
    return Result::Terminated;
  }
  if (v.startCode != 0x00) {                        // only NULL start code is levels
    if (s) { s->seq = v.sequence; s->lastMs = nowMs; }
    return Result::NotDmx;
  }
  if (!s) {
    for (auto& c : r.src) if (!c.used) { s = &c; break; }
    if (!s) {                                       // full: replace a lower-priority slot
      Source* low = &r.src[0];
      if (r.src[1].priority < low->priority) low = &r.src[1];
      if (v.priority <= low->priority) return Result::Ignored;
      s = low;
    }
    memcpy(s->cid, v.cid, 16);
    s->used = true;
  }
  uint16_t len = v.length > 512 ? 512 : v.length;
  memcpy(s->data, v.data, len);
  if (len < 512) memset(s->data + len, 0, 512 - len);
  s->seq = v.sequence;
  s->priority = v.priority;
  s->lastMs = nowMs;
  return Result::Accepted;
}

// Output of the winning priority (HTP among equals) into out[512]; returns
// the number of sources merged (0 = no live sACN).
inline uint8_t receiverOutput(const Receiver& r, uint8_t* out) {
  int top = -1;
  for (const auto& s : r.src) if (s.used && int(s.priority) > top) top = s.priority;
  memset(out, 0, 512);
  if (top < 0) return 0;
  uint8_t n = 0;
  for (const auto& s : r.src) {
    if (!s.used || s.priority != top) continue;
    n++;
    for (int i = 0; i < 512; i++) if (s.data[i] > out[i]) out[i] = s.data[i];
  }
  return n;
}

}  // namespace sacn

// ── ENTTEC DMX USB PRO widget protocol (USB serial in) ──────────────────────
// Method: the public "ENTTEC DMX USB PRO API" (widget API) document, ENTTEC Pty Ltd.
// Every message is: 0x7E start, label, data length LSB, data length MSB,
// data[length], 0xE7 end. Labels implemented here, as the document defines them:
//   3  Get Widget Parameters  request: user-config size LSB/MSB.
//                             reply:   firmware version LSB, MSB, DMX output break
//                                      time (10.67 us units, 9..127), mark-after-break
//                                      time (10.67 us units, 1..127), output rate
//                                      (packets/s, 1..40), then user-config bytes.
//   4  Set Widget Parameters  user-config size LSB/MSB, break, MAB, rate, user data.
//                             Accepted and counted; nothing is stored (the node's DMX
//                             timing is its own driver's, see README "USB").
//   6  Output Only Send DMX   DMX start code, then channel 1..N values.
//   10 Get Widget Serial No.  reply: 4 bytes BCD, least significant digit pair first.
// Any other label is ignored. A frame whose length is over kMaxData or whose end byte
// is not 0xE7 is dropped and the bytes after its start byte are scanned again for the
// next 0x7E, so garbage or a cut-off frame never costs the frames that follow it.
// Pure code: no Arduino. Native tests in test/test_smoke.
namespace widget {

constexpr uint8_t  kStart = 0x7E;
constexpr uint8_t  kEnd   = 0xE7;
constexpr uint16_t kMaxData = 600;              // API: largest message data length
constexpr uint16_t kMaxRaw  = kMaxData + 5;     // start, label, len x2, data, end
constexpr uint8_t  kLabelGetParams = 3;
constexpr uint8_t  kLabelSetParams = 4;
constexpr uint8_t  kLabelSendDmx   = 6;
constexpr uint8_t  kLabelGetSerial = 10;
constexpr uint16_t kMaxUserConfig  = 508;       // API: user configuration size limit
constexpr uint16_t kChannels       = 512;

struct Parser {
  uint8_t  raw[kMaxRaw];      // the frame in progress, raw[0] == kStart
  uint16_t rawLen;
  uint8_t  scratch[kMaxRaw];  // replay buffer for resync
  uint32_t frames;            // complete, well-formed frames (any label)
  uint32_t errors;            // frames dropped: oversize length or bad end byte
  uint32_t skipped;           // bytes outside any frame (log text, noise)
};

inline void parserReset(Parser& p) {
  p.rawLen = 0; p.frames = 0; p.errors = 0; p.skipped = 0;
}

enum class Step : uint8_t { None, Frame, Bad };

inline uint8_t  frameLabel(const Parser& p) { return p.raw[1]; }
inline uint16_t frameLen(const Parser& p) { return uint16_t(p.raw[2] | (uint16_t(p.raw[3]) << 8)); }
inline const uint8_t* frameData(const Parser& p) { return p.raw + 4; }

inline Step step(Parser& p, uint8_t b) {
  if (p.rawLen == 0) {
    if (b == kStart) p.raw[p.rawLen++] = b; else p.skipped++;
    return Step::None;
  }
  p.raw[p.rawLen++] = b;
  if (p.rawLen == 4 && frameLen(p) > kMaxData) return Step::Bad;
  if (p.rawLen >= 4 && p.rawLen == 5 + frameLen(p)) {
    if (b != kEnd) return Step::Bad;
    p.frames++;
    return Step::Frame;
  }
  return Step::None;
}

// Drop the frame in progress and re-scan every byte after its start byte.
// Each nested failure restarts one byte after that frame's own start, so the
// scan always moves forward and ends. onFrame(label, data, len) as in feed().
template <class F>
inline void resync(Parser& p, F&& onFrame) {
  if (p.rawLen == 0) return;
  p.errors++;
  const uint16_t m = p.rawLen;
  memcpy(p.scratch, p.raw, m);
  p.rawLen = 0;
  uint16_t i = 1, st = 1;
  while (i < m) {
    if (p.rawLen == 0) st = i;
    Step s = step(p, p.scratch[i]);
    if (s == Step::Frame) {
      onFrame(frameLabel(p), frameData(p), frameLen(p));
      p.rawLen = 0;
    } else if (s == Step::Bad) {
      p.errors++;
      p.rawLen = 0;
      i = uint16_t(st + 1);
      continue;
    }
    i++;
  }
}

// Feed one byte. onFrame(label, data, len) runs for every complete frame; data
// is valid only during the call.
template <class F>
inline void feed(Parser& p, uint8_t b, F&& onFrame) {
  Step s = step(p, b);
  if (s == Step::Frame) {
    onFrame(frameLabel(p), frameData(p), frameLen(p));
    p.rawLen = 0;
  } else if (s == Step::Bad) {
    resync(p, onFrame);
  }
}

template <class F>
inline void feed(Parser& p, const uint8_t* buf, size_t n, F&& onFrame) {
  for (size_t i = 0; i < n; i++) feed(p, buf[i], onFrame);
}

// Label 6: data[0] is the DMX start code, data[1..] channels 1..N. Only start
// code 0 (dimmer data) is applied. Channels beyond the frame keep their value,
// so a short frame (the API minimum is 24 channels) touches only its own.
// Returns the number of channels written; 0 for a non-zero start code.
inline uint16_t applyDmx(const uint8_t* data, uint16_t len, uint8_t* vals512) {
  if (len < 2 || data[0] != 0x00) return 0;
  uint16_t ch = uint16_t(len - 1);
  if (ch > kChannels) ch = kChannels;
  memcpy(vals512, data + 1, ch);
  return ch;
}

inline size_t buildFrame(uint8_t* out, size_t cap, uint8_t label, const uint8_t* data, uint16_t len) {
  if (len > kMaxData || cap < size_t(len) + 5) return 0;
  out[0] = kStart;
  out[1] = label;
  out[2] = uint8_t(len & 0xff);
  out[3] = uint8_t(len >> 8);
  if (len) memcpy(out + 4, data, len);
  out[4 + len] = kEnd;
  return size_t(len) + 5;
}

struct Params {
  uint16_t firmware;   // reported as LSB, MSB
  uint16_t breakUs;    // the node's real DMX break, microseconds
  uint16_t mabUs;      // the node's real mark-after-break, microseconds
  uint16_t rateHz;     // the node's real DMX refresh, packets per second
};

// API units: 10.67 us per count, rounded to nearest.
inline uint8_t usToUnits(uint16_t us, uint8_t lo, uint8_t hi) {
  uint32_t u = (uint32_t(us) * 100 + 533) / 1067;
  if (u < lo) u = lo;
  if (u > hi) u = hi;
  return uint8_t(u);
}

// Label 3 reply. userSize is what the request asked for (clamped to the API's
// 508); the node keeps no user configuration, so those bytes are zero.
inline size_t buildParamsReply(uint8_t* out, size_t cap, const Params& prm, uint16_t userSize) {
  if (userSize > kMaxUserConfig) userSize = kMaxUserConfig;
  const uint16_t len = uint16_t(5 + userSize);
  if (cap < size_t(len) + 5) return 0;
  uint8_t* d = out + 4;
  d[0] = uint8_t(prm.firmware & 0xff);
  d[1] = uint8_t(prm.firmware >> 8);
  d[2] = usToUnits(prm.breakUs, 9, 127);
  d[3] = usToUnits(prm.mabUs, 1, 127);
  d[4] = uint8_t(prm.rateHz < 1 ? 1 : (prm.rateHz > 40 ? 40 : prm.rateHz));
  memset(d + 5, 0, userSize);
  out[0] = kStart; out[1] = kLabelGetParams;
  out[2] = uint8_t(len & 0xff); out[3] = uint8_t(len >> 8);
  out[4 + len] = kEnd;
  return size_t(len) + 5;
}

// Requested user-config size of a label 3 request (0 when absent).
inline uint16_t paramsRequestSize(const uint8_t* data, uint16_t len) {
  return len >= 2 ? uint16_t(data[0] | (uint16_t(data[1]) << 8)) : 0;
}

// 8 decimal digits as 4 BCD bytes, least significant digit pair first.
inline void toBcd(uint32_t decimal, uint8_t out[4]) {
  decimal %= 100000000UL;
  for (int i = 0; i < 4; i++) {
    uint8_t lo = uint8_t(decimal % 10); decimal /= 10;
    uint8_t hi = uint8_t(decimal % 10); decimal /= 10;
    out[i] = uint8_t((hi << 4) | lo);
  }
}

// Label 10 reply.
inline size_t buildSerialReply(uint8_t* out, size_t cap, uint32_t serialDecimal) {
  uint8_t bcd[4];
  toBcd(serialDecimal, bcd);
  return buildFrame(out, cap, kLabelGetSerial, bcd, 4);
}

}  // namespace widget

// ── Output merge (computeOutput_locked) ─────────────────────────────────────
// Network-class sources (Art-Net, sACN, USB widget) merge HTP among themselves;
// the mode then decides what reaches the output. Mode numbers match main.cpp's
// enum Mode: 0 WEB_ONLY, 1 ARTNET_ONLY, 2 MERGE_HTP.
namespace merge {
constexpr uint8_t kWebOnly = 0, kArtnetOnly = 1, kHtp = 2;

inline uint8_t networkIn(bool aOn, uint8_t a, bool sOn, uint8_t sv, bool uOn, uint8_t u) {
  uint8_t v = 0;
  if (aOn && a > v) v = a;
  if (sOn && sv > v) v = sv;
  if (uOn && u > v) v = u;
  return v;
}

// held = the channel's current output (ARTNET_ONLY holds it while every network
// source is idle, unless fallbackToWeb).
inline uint8_t forMode(uint8_t mode, bool netActive, uint8_t netIn, uint8_t web, uint8_t held,
                       bool fallbackToWeb) {
  switch (mode) {
    case kWebOnly:    return web;
    case kArtnetOnly: return netActive ? netIn : (fallbackToWeb ? web : held);
    default:          return netActive ? (web > netIn ? web : netIn) : web;
  }
}
}  // namespace merge

// ── OSC 1.0 argument conversion ───────────────────────────────────────────────
// Level addresses take ints 0..255, or floats: 0.0..1.0 is normalised (TouchOSC's
// default fader range) and larger values are taken as levels. NaN/Inf and
// out-of-range values are clamped before conversion ([conv.fpint] makes an
// out-of-range float-to-int conversion undefined).
inline int32_t oscFloatToLevel(float f) {
  if (!isfinite(f) || f <= 0.0f) return 0;
  if (f <= 1.0f) return int32_t(lroundf(f * 255.0f));
  if (f >= 255.0f) return 255;
  return int32_t(lroundf(f));
}

// Index/flag addresses take the float rounded and clamped to int32 range.
inline int32_t oscFloatToInt(float f) {
  if (!isfinite(f)) return 0;
  if (f >= 2147483520.0f) return 2147483520;
  if (f <= -2147483520.0f) return -2147483520;
  return int32_t(lroundf(f));
}

// OSC 1.0 bundle: "#bundle\0", 8-byte time tag, then (int32 size, element)*.
inline bool oscIsBundle(const uint8_t* p, size_t n) {
  static const uint8_t tag[8] = {'#','b','u','n','d','l','e',0};
  return n >= 16 && memcmp(p, tag, 8) == 0;
}

// ── HTTP argument parsing (audit 2026-10-15 2-M5) ────────────────────────────
// Arduino's String::toInt() answers 0 for "abc" and the old routes then
// clamped, so /set?ch=abc&v=255 wrote channel 1. These accept only decimal
// digits (and one leading '-' in parseIntArg, for the few arguments where a
// negative value is legal), at most 10 digits, inside [lo, hi]. Anything else
// returns false and leaves *out untouched; the route answers 400.
inline bool parseUintArg(const char* s, uint32_t lo, uint32_t hi, uint32_t* out) {
  if (!s || !*s) return false;
  uint64_t v = 0;
  size_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || ++n > 10) return false;
    v = v * 10 + uint64_t(*s - '0');
  }
  if (v < lo || v > hi) return false;
  *out = uint32_t(v);
  return true;
}

inline bool parseIntArg(const char* s, int32_t lo, int32_t hi, int32_t* out) {
  if (!s || !*s) return false;
  bool neg = (*s == '-');
  uint32_t mag = 0;
  if (!parseUintArg(neg ? s + 1 : s, 0, neg ? 2147483648u : 2147483647u, &mag)) return false;
  int64_t v = neg ? -int64_t(mag) : int64_t(mag);
  if (v < lo || v > hi) return false;
  *out = int32_t(v);
  return true;
}

// ── HTTP route table (audit 2026-10-15 2-H2, 2-M4, 2-M7) ─────────────────────
// ONE list of every HTTP route. setupWeb() registers only through it and the
// manifest is generated from it, so neither can drift from the other
// (tools/check_routes.py diffs it against main.cpp and README.md).
//
// Kinds (method rules after OWASP CSRF Prevention Cheat Sheet, "custom request
// headers" and "use of SameSite"; RFC 9110 §9.2.1 safe methods):
//   GetLocal  GET only, no Access-Control-Allow-Origin: console pages and
//             /wifi/scan (nearby network names stay on the node's own origin).
//   GetShared GET only, ACAO "*": JSON reads other origins may read (di reads
//             /status cross-origin).
//   Static    GET only, no ACAO: di's desk UI files served from flash (V2) and
//             the 301s that replace the old console pages. Same gate as GetLocal.
//   Operator  GET and POST, no header, no ACAO: what di.iiii and phones send
//             as no-cors simple requests (/set, /master, /blackout, ...).
//             A cross-site page CAN fire these. Kept on purpose: they are
//             the show controls di needs; a real login is the owner's call.
//   Console   POST only and the header X-Vizzz-Console: 1. A cross-site page
//             cannot add a custom header without a CORS preflight, and the
//             node answers every preflight on these routes with 403 and no
//             ACAO, so the browser never sends the request.
enum class RouteKind : uint8_t { GetLocal, GetShared, Operator, Console, Static };

struct RouteSpec {
  const char* path;
  RouteKind kind;
};

constexpr const char* CONSOLE_HEADER = "X-Vizzz-Console";

static constexpr RouteSpec kRoutes[] = {
  // Static: one entry per file in kDeskUiFiles ("/" is index.html), then the
  // 301s of kDeskRedirects below. tools/check_routes.py and a native test keep
  // the three in step.
  {"/", RouteKind::Static}, {"/app.js", RouteKind::Static},
  {"/style.css", RouteKind::Static}, {"/qr.js", RouteKind::Static},
  {"/from.js", RouteKind::Static}, {"/show.js", RouteKind::Static},
  {"/caps.js", RouteKind::Static},
  {"/control", RouteKind::Static}, {"/patch", RouteKind::Static},
  {"/scenes", RouteKind::Static}, {"/vj", RouteKind::Static},
  {"/network", RouteKind::Static}, {"/system", RouteKind::Static},
  {"/performance", RouteKind::Static},
  {"/wifi/scan", RouteKind::GetLocal},

  {"/status", RouteKind::GetShared}, {"/monitor", RouteKind::GetShared},
  {"/safety/status", RouteKind::GetShared}, {"/node/manifest", RouteKind::GetShared},
  {"/manifest.json", RouteKind::GetShared}, {"/page", RouteKind::GetShared},
  {"/discover", RouteKind::GetShared}, {"/peers", RouteKind::GetShared},
  {"/groups", RouteKind::GetShared}, {"/fixtures", RouteKind::GetShared},
  {"/cue/status", RouteKind::GetShared}, {"/fx/status", RouteKind::GetShared},

  {"/set", RouteKind::Operator}, {"/master", RouteKind::Operator},
  {"/blackout", RouteKind::Operator}, {"/full", RouteKind::Operator},
  {"/scene/recall", RouteKind::Operator}, {"/cue/run", RouteKind::Operator},
  {"/cue/next", RouteKind::Operator}, {"/fx/tap", RouteKind::Operator},

  {"/panic", RouteKind::Console}, {"/reboot", RouteKind::Console},
  {"/factory-reset", RouteKind::Console}, {"/wifi/set", RouteKind::Console},
  {"/wifi/forget", RouteKind::Console}, {"/netmode/set", RouteKind::Console},
  {"/node/set", RouteKind::Console}, {"/web/set", RouteKind::Console},
  {"/artnet/set", RouteKind::Console}, {"/artout/set", RouteKind::Console},
  {"/artout/peer", RouteKind::Console}, {"/mode/set", RouteKind::Console},
  {"/mode/fallback", RouteKind::Console}, {"/safety/set", RouteKind::Console},
  {"/scene/save", RouteKind::Console}, {"/fixture/set", RouteKind::Console},
  {"/group/set", RouteKind::Console}, {"/group/apply", RouteKind::Console},
  {"/cue/count", RouteKind::Console}, {"/cue/set", RouteKind::Console},
  {"/fx/set", RouteKind::Console}, {"/color/set", RouteKind::Console},
  {"/peer/cmd", RouteKind::Console}, {"/announce", RouteKind::Console},
  {"/net/blackout", RouteKind::Console}, {"/net/full", RouteKind::Console},
  {"/net/master", RouteKind::Console}, {"/net/scene/recall", RouteKind::Console},
};
constexpr size_t kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);

// The old console pages, now 301s into di's desk (hash = a page name in
// ui/index.html: setup, control, touch, fader, midi). di has no patch, scenes
// or VJ page of those names, so those go to "/". /network, /system and
// /performance served the same removed page; they go to "/" as well.
struct RedirectSpec {
  const char* path;
  const char* location;
};
static constexpr RedirectSpec kDeskRedirects[] = {
  {"/control", "/#control"}, {"/patch", "/"}, {"/scenes", "/"}, {"/vj", "/"},
  {"/network", "/"}, {"/system", "/"}, {"/performance", "/"},
};
constexpr size_t kDeskRedirectCount = sizeof(kDeskRedirects) / sizeof(kDeskRedirects[0]);

inline const char* redirectTarget(const char* path) {
  if (!path) return nullptr;
  for (size_t i = 0; i < kDeskRedirectCount; i++)
    if (strcmp(kDeskRedirects[i].path, path) == 0) return kDeskRedirects[i].location;
  return nullptr;
}

// A route path -> the kDeskUiFiles path it serves: "/" is the desk page.
inline const char* deskFilePath(const char* route) {
  return (route && strcmp(route, "/") == 0) ? "/index.html" : route;
}

// If-None-Match against our strong ETag (RFC 9110 §13.1.2): "*" matches, else
// any entry of the comma list equal to "<etag>" (a W/ prefix compares weakly,
// which the weak comparison for If-None-Match allows). `etag` is the bare hex.
inline bool etagMatches(const char* inm, const char* etag) {
  if (!inm || !etag || !*etag) return false;
  const size_t n = strlen(etag);
  const char* p = inm;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (p[0] == '*' && (p[1] == 0 || p[1] == ',' || p[1] == ' ')) return true;
    if (p[0] == 'W' && p[1] == '/') p += 2;
    if (p[0] == '"' && strncmp(p + 1, etag, n) == 0 && p[1 + n] == '"') return true;
    while (*p && *p != ',') p++;
  }
  return false;
}

inline int findRoute(const char* path) {
  if (!path) return -1;
  for (size_t i = 0; i < kRouteCount; i++)
    if (strcmp(kRoutes[i].path, path) == 0) return int(i);
  return -1;
}

inline const char* routeMethods(RouteKind k) {
  switch (k) {
    case RouteKind::Operator: return "GET, POST";
    case RouteKind::Console:  return "POST";
    default:                  return "GET";
  }
}

inline bool routeSendsAcao(RouteKind k) { return k == RouteKind::GetShared; }

enum class HttpMethod : uint8_t { Get, Post, Options, Other };
enum class Gate : uint8_t {
  Allow,            // run the handler
  Preflight,        // 204 + ACAO (GetShared only)
  PreflightDenied,  // 403, no ACAO: the browser drops the real request
  NotAllowed,       // 405 + Allow: routeMethods(kind)
  NeedHeader,       // 403: POST without X-Vizzz-Console: 1
};

inline Gate gateRequest(RouteKind k, HttpMethod m, bool consoleHeader) {
  if (m == HttpMethod::Options)
    return k == RouteKind::GetShared ? Gate::Preflight : Gate::PreflightDenied;
  switch (k) {
    case RouteKind::GetLocal:
    case RouteKind::GetShared:
    case RouteKind::Static:
      return m == HttpMethod::Get ? Gate::Allow : Gate::NotAllowed;
    case RouteKind::Operator:
      return (m == HttpMethod::Get || m == HttpMethod::Post) ? Gate::Allow : Gate::NotAllowed;
    case RouteKind::Console:
      if (m != HttpMethod::Post) return Gate::NotAllowed;
      return consoleHeader ? Gate::Allow : Gate::NeedHeader;
  }
  return Gate::NotAllowed;
}

// The header must be exactly "1": an empty or other value is not the console.
inline bool isConsoleHeaderValue(const char* v) { return v && v[0] == '1' && v[1] == 0; }

// WebSocket Origin rule (2-L7): no Origin (curl, tools) or Origin equal to
// "http://" + Host (the console itself). Any other page is refused.
inline bool wsOriginAllowed(const char* origin, const char* host) {
  if (!origin || !*origin) return true;
  if (!host || !*host) return false;
  if (strncmp(origin, "http://", 7) != 0) return false;
  return strcmp(origin + 7, host) == 0;
}

// Node health facts (plan step V0). /status and the manifest both print these
// three keys from one HealthFacts, via healthJsonFields(), so they cannot
// drift. heap_max_block is the largest single allocation that still succeeds
// (fragmentation), ws_clients the live /ws sockets.
struct HealthFacts {
  uint32_t heapFree;
  uint32_t heapMaxBlock;
  uint32_t wsClients;
};

constexpr const char* kHealthKeys[] = {"heap_free", "heap_max_block", "ws_clients"};
constexpr size_t kHealthKeyCount = sizeof(kHealthKeys) / sizeof(kHealthKeys[0]);

// Writes `"heap_free":N,"heap_max_block":N,"ws_clients":N` (no braces, no
// trailing comma) so the caller can splice it into an object. Returns the
// snprintf result: >= n means it was truncated.
inline int healthJsonFields(char* dst, size_t n, const HealthFacts& h) {
  return snprintf(dst, n, "\"%s\":%lu,\"%s\":%lu,\"%s\":%lu",
                  kHealthKeys[0], (unsigned long)h.heapFree,
                  kHealthKeys[1], (unsigned long)h.heapMaxBlock,
                  kHealthKeys[2], (unsigned long)h.wsClients);
}

}  // namespace vizzz
