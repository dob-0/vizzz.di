#pragma once

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

}  // namespace vizzz
