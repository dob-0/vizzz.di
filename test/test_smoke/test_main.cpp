#include <unity.h>

#include <string.h>

#include "vizzz_core.h"

void test_pack_universe_uses_15bit_layout() {
  TEST_ASSERT_EQUAL_UINT16(0x123, vizzz::packUniverse(0x01, 0x02, 0x03));
  TEST_ASSERT_EQUAL_UINT16(0x7ff, vizzz::packUniverse(0x07, 0x0f, 0x0f));
}

void test_apply_master_scales_levels() {
  TEST_ASSERT_EQUAL_UINT8(255, vizzz::applyMaster(255, 255));
  TEST_ASSERT_EQUAL_UINT8(0, vizzz::applyMaster(200, 0));
  TEST_ASSERT_EQUAL_UINT8(127, vizzz::applyMaster(255, 128));
  TEST_ASSERT_EQUAL_UINT8(50, vizzz::applyMaster(100, 128));
}

void test_clamp_level_applies_ceiling() {
  TEST_ASSERT_EQUAL_UINT8(120, vizzz::clampLevel(120, 200));
  TEST_ASSERT_EQUAL_UINT8(200, vizzz::clampLevel(255, 200));
}

void test_slew_toward_limits_step_size() {
  TEST_ASSERT_EQUAL_UINT8(110, vizzz::slewToward(100, 200, 10));
  TEST_ASSERT_EQUAL_UINT8(190, vizzz::slewToward(200, 100, 10));
  TEST_ASSERT_EQUAL_UINT8(200, vizzz::slewToward(195, 200, 10));
  TEST_ASSERT_EQUAL_UINT8(100, vizzz::slewToward(105, 100, 10));
  TEST_ASSERT_EQUAL_UINT8(200, vizzz::slewToward(100, 200, 0));
}

void test_wpa_passphrase_bounds() {
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase("", 0));
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase("abc", 3));
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase("1234567", 7));
  TEST_ASSERT_TRUE(vizzz::isValidWpaPassphrase("12345678", 8));
  char p63[64]; for (int i = 0; i < 63; i++) p63[i] = 'a'; p63[63] = '\0';
  TEST_ASSERT_TRUE(vizzz::isValidWpaPassphrase(p63, 63));
  char p64[65]; for (int i = 0; i < 64; i++) p64[i] = 'a'; p64[64] = '\0';
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase(p64, 64));
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase("pass\tword", 9));
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase("pass\x7fword", 9));
  TEST_ASSERT_TRUE(vizzz::isValidWpaPassphrase("with space~", 11));
  TEST_ASSERT_FALSE(vizzz::isValidWpaPassphrase(nullptr, 8));
}

void test_sanitize_host_label() {
  char out[32];
  TEST_ASSERT_EQUAL_size_t(15, vizzz::sanitizeHostLabel("vizzz.di-BA6FCC", out, sizeof(out), "vizzz-di"));
  TEST_ASSERT_EQUAL_STRING("vizzz-di-BA6FCC", out);
  vizzz::sanitizeHostLabel("--a b--", out, sizeof(out), "x");
  TEST_ASSERT_EQUAL_STRING("a-b", out);
  vizzz::sanitizeHostLabel("...", out, sizeof(out), "vizzz-di");
  TEST_ASSERT_EQUAL_STRING("vizzz-di", out);
  vizzz::sanitizeHostLabel("", out, sizeof(out), "vizzz-di");
  TEST_ASSERT_EQUAL_STRING("vizzz-di", out);
  char small[5];
  vizzz::sanitizeHostLabel("abcdefgh", small, sizeof(small), "x");
  TEST_ASSERT_EQUAL_STRING("abcd", small);
  vizzz::sanitizeHostLabel("abc-efgh", small, sizeof(small), "x");
  TEST_ASSERT_EQUAL_STRING("abc", small);  // cut lands on '-', trailing hyphen dropped
}

void test_parse_ipv4_strict() {
  uint8_t ip[4];
  TEST_ASSERT_TRUE(vizzz::parseIpv4("192.168.88.66", ip));
  TEST_ASSERT_EQUAL_UINT8(192, ip[0]);
  TEST_ASSERT_EQUAL_UINT8(66, ip[3]);
  TEST_ASSERT_TRUE(vizzz::parseIpv4("0.0.0.0", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("256.1.1.1", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("1.2.3", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("1.2.3.4.5", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("1.2.3.4 ", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("a.b.c.d", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4("0001.2.3.4", ip));
  TEST_ASSERT_FALSE(vizzz::parseIpv4(nullptr, ip));
}

void test_cue_timing() {
  TEST_ASSERT_EQUAL_UINT32(0, vizzz::clampCueTimeMs(-5, 600000));
  TEST_ASSERT_EQUAL_UINT32(1500, vizzz::clampCueTimeMs(1500, 600000));
  TEST_ASSERT_EQUAL_UINT32(600000, vizzz::clampCueTimeMs(3600000, 600000));
  TEST_ASSERT_EQUAL_UINT32(vizzz::CUE_MIN_STEP_MS, vizzz::cueStepMs(0, 0));
  TEST_ASSERT_EQUAL_UINT32(vizzz::CUE_MIN_STEP_MS, vizzz::cueStepMs(10, 20));
  TEST_ASSERT_EQUAL_UINT32(2000, vizzz::cueStepMs(500, 1500));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, vizzz::cueStepMs(0xFFFFFFF0u, 0x100u));
  TEST_ASSERT_FALSE(vizzz::cueStepDue(1049, 1000, 50));
  TEST_ASSERT_TRUE(vizzz::cueStepDue(1050, 1000, 50));
  // millis() wrap: started 20 ms before the wrap, 40 ms elapsed
  TEST_ASSERT_FALSE(vizzz::cueStepDue(20u, 0xFFFFFFECu, 50));
  TEST_ASSERT_TRUE(vizzz::cueStepDue(30u, 0xFFFFFFECu, 50));
}

void test_elapsed_at_least_wraps() {
  TEST_ASSERT_TRUE(vizzz::elapsedAtLeast(30000, 0, 30000));
  TEST_ASSERT_FALSE(vizzz::elapsedAtLeast(29999, 0, 30000));
  TEST_ASSERT_TRUE(vizzz::elapsedAtLeast(29000u, 0xFFFFF000u, 30000));   // 33096 ms across the wrap
  TEST_ASSERT_FALSE(vizzz::elapsedAtLeast(20000u, 0xFFFFF000u, 30000));  // 24096 ms
}

void test_sta_retry_backoff() {
  TEST_ASSERT_EQUAL_UINT32(0, vizzz::staRetryDelayMs(0));
  TEST_ASSERT_EQUAL_UINT32(30000, vizzz::staRetryDelayMs(1));
  TEST_ASSERT_EQUAL_UINT32(60000, vizzz::staRetryDelayMs(2));
  TEST_ASSERT_EQUAL_UINT32(120000, vizzz::staRetryDelayMs(3));
  TEST_ASSERT_EQUAL_UINT32(240000, vizzz::staRetryDelayMs(4));
  TEST_ASSERT_EQUAL_UINT32(300000, vizzz::staRetryDelayMs(5));
  TEST_ASSERT_EQUAL_UINT32(300000, vizzz::staRetryDelayMs(255));
}

void test_clamp_channel_range() {
  uint16_t a = 0, b = 0;
  vizzz::clampChannelRange(a, b, 512);
  TEST_ASSERT_EQUAL_UINT16(1, a); TEST_ASSERT_EQUAL_UINT16(1, b);
  a = 600; b = 700;
  vizzz::clampChannelRange(a, b, 512);
  TEST_ASSERT_EQUAL_UINT16(512, a); TEST_ASSERT_EQUAL_UINT16(512, b);
  a = 10; b = 5;
  vizzz::clampChannelRange(a, b, 512);
  TEST_ASSERT_EQUAL_UINT16(10, a); TEST_ASSERT_EQUAL_UINT16(10, b);
  a = 1; b = 0xFFFF;
  vizzz::clampChannelRange(a, b, 512);
  TEST_ASSERT_EQUAL_UINT16(1, a); TEST_ASSERT_EQUAL_UINT16(512, b);
  a = 17; b = 24;
  vizzz::clampChannelRange(a, b, 512);
  TEST_ASSERT_EQUAL_UINT16(17, a); TEST_ASSERT_EQUAL_UINT16(24, b);
}

// ── Art-Net 4 ────────────────────────────────────────────────────────────────
static size_t makeDiPoll(uint8_t* b) {  // same bytes as di's artnet.js buildPoll()
  const uint8_t poll[14] = {'A','r','t','-','N','e','t',0, 0x00,0x20, 0,14, 0,0};
  memcpy(b, poll, 14);
  return 14;
}

void test_artpoll_14_bytes_is_detected() {
  uint8_t b[32];
  size_t n = makeDiPoll(b);
  TEST_ASSERT_TRUE(vizzz::artnet::isArtPoll(b, n));
  TEST_ASSERT_FALSE(vizzz::artnet::isArtPoll(b, 13));
  b[8] = 0x00; b[9] = 0x50;
  TEST_ASSERT_FALSE(vizzz::artnet::isArtPoll(b, n));
  makeDiPoll(b); b[3] = 'x';
  TEST_ASSERT_FALSE(vizzz::artnet::isArtPoll(b, n));
}

void test_artdmx_parse_fields_and_limits() {
  uint8_t b[18 + 512];
  vizzz::artnet::buildDmxHeader(b, 0x0123, 7, 0, 512);
  for (int i = 0; i < 512; i++) b[18 + i] = uint8_t(i);
  vizzz::artnet::DmxView v{};
  TEST_ASSERT_TRUE(vizzz::artnet::parseDmx(b, sizeof(b), v));
  TEST_ASSERT_EQUAL_UINT16(0x0123, v.portAddress);
  TEST_ASSERT_EQUAL_UINT8(7, v.sequence);
  TEST_ASSERT_EQUAL_UINT16(512, v.length);
  TEST_ASSERT_EQUAL_UINT8(9, v.data[9]);
  TEST_ASSERT_FALSE(vizzz::artnet::parseDmx(b, 18 + 511, v));   // length past datagram
  b[11] = 13;
  TEST_ASSERT_FALSE(vizzz::artnet::parseDmx(b, sizeof(b), v));  // ProtVer < 14
  vizzz::artnet::buildDmxHeader(b, 0, 1, 0, 0);
  TEST_ASSERT_FALSE(vizzz::artnet::parseDmx(b, sizeof(b), v));  // Length 0
  vizzz::artnet::buildDmxHeader(b, 0x7FFF, 1, 0, 2);
  TEST_ASSERT_TRUE(vizzz::artnet::parseDmx(b, 20, v));
  TEST_ASSERT_EQUAL_UINT16(0x7FFF, v.portAddress);
}

void test_artdmx_sequence_skips_zero() {
  TEST_ASSERT_EQUAL_UINT8(1, vizzz::artnet::nextSequence(255));
  TEST_ASSERT_EQUAL_UINT8(2, vizzz::artnet::nextSequence(1));
}

void test_artpollreply_layout_matches_spec_and_di() {
  uint8_t o[239];
  vizzz::artnet::PollReplyInfo in{};
  const uint8_t ip[4] = {192, 168, 88, 66};
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xBA, 0x6F, 0xCC};
  memcpy(in.ip, ip, 4); memcpy(in.mac, mac, 6);
  in.net = 0x12; in.subnet = 0x3; in.uni = 0x4;
  in.shortName = "a-very-long-node-name-here";   // > 17 chars: truncated
  in.longName = "vizzz.di test";
  in.report = "ok";
  in.replyCount = 42;
  in.outputActive = true; in.dhcp = true; in.refreshHz = 43;
  TEST_ASSERT_EQUAL(239, (int)vizzz::artnet::buildPollReply(o, in));
  TEST_ASSERT_EQUAL_MEMORY("Art-Net\0", o, 8);
  TEST_ASSERT_EQUAL_UINT8(0x00, o[8]); TEST_ASSERT_EQUAL_UINT8(0x21, o[9]);   // OpPollReply LE
  TEST_ASSERT_EQUAL_MEMORY(ip, o + 10, 4);
  TEST_ASSERT_EQUAL_UINT8(0x36, o[14]); TEST_ASSERT_EQUAL_UINT8(0x19, o[15]); // 6454 LE
  TEST_ASSERT_EQUAL_UINT8(0x12, o[18]); TEST_ASSERT_EQUAL_UINT8(0x3, o[19]);
  TEST_ASSERT_EQUAL_STRING_LEN("a-very-long-node-", (const char*)o + 26, 17);
  TEST_ASSERT_EQUAL_UINT8(0, o[26 + 17]);                                      // NUL-terminated
  TEST_ASSERT_EQUAL_STRING("vizzz.di test", (const char*)o + 44);
  TEST_ASSERT_EQUAL_STRING("#0001 [0042] ok", (const char*)o + 108);
  TEST_ASSERT_EQUAL_UINT8(0, o[172]); TEST_ASSERT_EQUAL_UINT8(1, o[173]);     // NumPorts BE
  TEST_ASSERT_EQUAL_UINT8(0x80, o[174]);                                       // DMX output
  TEST_ASSERT_EQUAL_UINT8(0x80, o[182]);                                       // data out
  TEST_ASSERT_EQUAL_UINT8(0x4, o[190]);                                        // SwOut[0]
  TEST_ASSERT_EQUAL_UINT8(0x00, o[200]);                                       // StNode
  TEST_ASSERT_EQUAL_MEMORY(mac, o + 201, 6);
  TEST_ASSERT_EQUAL_MEMORY(ip, o + 207, 4);                                    // BindIp
  TEST_ASSERT_EQUAL_UINT8(1, o[211]);                                          // BindIndex
  TEST_ASSERT_EQUAL_UINT8(0x0F, o[212]);                                       // Status2
  TEST_ASSERT_EQUAL_UINT8(43, o[227]);
  // di's parsePollReply: universe = (net<<8)|(sub<<4)|SwOut
  TEST_ASSERT_EQUAL_UINT16(vizzz::packUniverse(0x12, 0x3, 0x4),
                           uint16_t((o[18] & 0x7F) << 8 | (o[19] & 0x0F) << 4 | (o[190] & 0x0F)));
}

static vizzz::artnet::Merger gMerge;

void test_artnet_merge_two_sources_htp_third_ignored() {
  using namespace vizzz::artnet;
  mergerReset(gMerge);
  uint8_t a[4] = {10, 200, 0, 5}, b[4] = {100, 20, 0}, c[2] = {255, 255}, out[512];
  TEST_ASSERT_TRUE(mergerAccept(gMerge, 1, 1, a, 4, 0) == MergeResult::Accepted);
  TEST_ASSERT_TRUE(mergerAccept(gMerge, 2, 1, b, 3, 0) == MergeResult::Accepted);
  TEST_ASSERT_TRUE(mergerAccept(gMerge, 3, 1, c, 2, 0) == MergeResult::ThirdSource);
  TEST_ASSERT_EQUAL_UINT8(2, mergerOutput(gMerge, out));
  TEST_ASSERT_EQUAL_UINT8(100, out[0]); TEST_ASSERT_EQUAL_UINT8(200, out[1]);
  TEST_ASSERT_EQUAL_UINT8(0, out[2]); TEST_ASSERT_EQUAL_UINT8(5, out[3]);
  // source 1 goes silent: dropped after 10 s, source 2 kept alive
  mergerAccept(gMerge, 2, 2, b, 3, 9000);
  mergerExpire(gMerge, 10001, kMergeTimeoutMs, 3000);
  TEST_ASSERT_EQUAL_UINT8(1, mergerSources(gMerge));
  // all silent past the loss timeout: everything goes
  mergerExpire(gMerge, 12001, kMergeTimeoutMs, 3000);
  TEST_ASSERT_EQUAL_UINT8(0, mergerSources(gMerge));
}

void test_artnet_merge_drops_out_of_order() {
  using namespace vizzz::artnet;
  mergerReset(gMerge);
  uint8_t hi[1] = {200}, lo[1] = {10}, out[512];
  mergerAccept(gMerge, 1, 50, hi, 1, 0);
  TEST_ASSERT_TRUE(mergerAccept(gMerge, 1, 49, lo, 1, 5) == MergeResult::OutOfOrder);
  mergerOutput(gMerge, out);
  TEST_ASSERT_EQUAL_UINT8(200, out[0]);
  TEST_ASSERT_TRUE(mergerAccept(gMerge, 1, 0, lo, 1, 6) == MergeResult::Accepted);   // 0 = off
  TEST_ASSERT_TRUE(mergerAccept(gMerge, 1, 49, lo, 1, 2000) == MergeResult::Accepted);
}

// ── sACN E1.31 ───────────────────────────────────────────────────────────────
// Same layout as di's serverXR/src/lighting/sacn.js buildPacket().
static size_t makeSacn(uint8_t* b, uint16_t uni, uint8_t prio, uint8_t seq, uint8_t opts,
                       uint8_t cidByte, const uint8_t* data, uint16_t len) {
  static const uint8_t pid[12] = {0x41,0x53,0x43,0x2D,0x45,0x31,0x2E,0x31,0x37,0,0,0};
  size_t total = 126 + len;
  memset(b, 0, total);
  b[1] = 0x10; memcpy(b + 4, pid, 12);
  b[16] = uint8_t(0x70 | ((total - 16) >> 8)); b[17] = uint8_t(total - 16);
  b[21] = 0x04; memset(b + 22, cidByte, 16);
  b[38] = uint8_t(0x70 | ((total - 38) >> 8)); b[39] = uint8_t(total - 38);
  b[43] = 0x02; memcpy(b + 44, "di", 2);
  b[108] = prio; b[111] = seq; b[112] = opts; b[113] = uint8_t(uni >> 8); b[114] = uint8_t(uni);
  b[115] = uint8_t(0x70 | ((total - 115) >> 8)); b[116] = uint8_t(total - 115);
  b[117] = 0x02; b[118] = 0xA1; b[122] = 0x01;
  b[123] = uint8_t((len + 1) >> 8); b[124] = uint8_t(len + 1);
  b[125] = 0x00; memcpy(b + 126, data, len);
  return total;
}

void test_sacn_universe_is_port_address_plus_one() {
  TEST_ASSERT_EQUAL_UINT16(1, vizzz::sacn::universeForPortAddress(0));
  TEST_ASSERT_EQUAL_UINT16(0x124, vizzz::sacn::universeForPortAddress(0x123));
  TEST_ASSERT_EQUAL_UINT16(32768, vizzz::sacn::universeForPortAddress(0x7FFF));
  uint8_t g[4];
  vizzz::sacn::multicastGroup(1, g);
  const uint8_t want[4] = {239, 255, 0, 1};
  TEST_ASSERT_EQUAL_MEMORY(want, g, 4);
}

void test_sacn_parse_layers() {
  uint8_t b[638], d[512];
  for (int i = 0; i < 512; i++) d[i] = uint8_t(255 - i);
  size_t n = makeSacn(b, 1, 100, 9, 0, 0xAB, d, 512);
  vizzz::sacn::DataView v{};
  TEST_ASSERT_TRUE(vizzz::sacn::parseData(b, n, v));
  TEST_ASSERT_EQUAL_UINT16(1, v.universe);
  TEST_ASSERT_EQUAL_UINT8(100, v.priority);
  TEST_ASSERT_EQUAL_UINT8(9, v.sequence);
  TEST_ASSERT_EQUAL_UINT16(512, v.length);
  TEST_ASSERT_EQUAL_UINT8(0xAB, v.cid[0]);
  TEST_ASSERT_EQUAL_UINT8(254, v.data[1]);
  TEST_ASSERT_FALSE(vizzz::sacn::parseData(b, n - 1, v));          // count past datagram
  makeSacn(b, 0, 100, 9, 0, 0xAB, d, 4);
  TEST_ASSERT_FALSE(vizzz::sacn::parseData(b, 130, v));             // universe 0 reserved
  makeSacn(b, 1, 100, 9, 0, 0xAB, d, 4); b[118] = 0xA0;
  TEST_ASSERT_FALSE(vizzz::sacn::parseData(b, 130, v));             // address/data type
  makeSacn(b, 1, 100, 9, 0, 0xAB, d, 4); b[21] = 0x08;
  TEST_ASSERT_FALSE(vizzz::sacn::parseData(b, 130, v));             // extended (sync/discovery)
  makeSacn(b, 1, 250, 9, 0, 0xAB, d, 4);
  TEST_ASSERT_TRUE(vizzz::sacn::parseData(b, 130, v));
  TEST_ASSERT_EQUAL_UINT8(200, v.priority);                         // clamped
}

static vizzz::sacn::Receiver gRx;

static vizzz::sacn::Result feed(uint8_t cid, uint8_t prio, uint8_t seq, uint8_t opts,
                                uint8_t level, uint32_t now) {
  uint8_t b[200], d[1] = {level};
  size_t n = makeSacn(b, 1, prio, seq, opts, cid, d, 1);
  vizzz::sacn::DataView v{};
  if (!vizzz::sacn::parseData(b, n, v)) return vizzz::sacn::Result::Ignored;
  return vizzz::sacn::receiverAccept(gRx, v, now);
}

void test_sacn_preview_terminated_priority_sequence() {
  using namespace vizzz::sacn;
  uint8_t out[512];
  receiverReset(gRx);
  TEST_ASSERT_TRUE(feed(1, 100, 1, kOptPreview, 255, 0) == Result::Preview);
  TEST_ASSERT_EQUAL_UINT8(0, receiverOutput(gRx, out));
  feed(1, 100, 1, 0, 50, 0);
  feed(2, 100, 1, 0, 80, 0);
  TEST_ASSERT_EQUAL_UINT8(2, receiverOutput(gRx, out));
  TEST_ASSERT_EQUAL_UINT8(80, out[0]);                               // HTP among equals
  feed(1, 150, 2, 0, 10, 10);
  TEST_ASSERT_EQUAL_UINT8(1, receiverOutput(gRx, out));
  TEST_ASSERT_EQUAL_UINT8(10, out[0]);                               // higher priority wins
  TEST_ASSERT_TRUE(feed(1, 150, 1, 0, 99, 20) == Result::OutOfOrder);
  TEST_ASSERT_TRUE(feed(3, 50, 1, 0, 255, 20) == Result::Ignored);   // third, lower priority
  TEST_ASSERT_TRUE(feed(1, 150, 3, kOptTerminated, 255, 30) == Result::Terminated);
  receiverOutput(gRx, out);
  TEST_ASSERT_EQUAL_UINT8(80, out[0]);                               // falls back to source 2
  receiverExpire(gRx, 2501);
  TEST_ASSERT_EQUAL_UINT8(0, receiverOutput(gRx, out));             // §6.7.1 2.5 s loss
}

// ── OSC ──────────────────────────────────────────────────────────────────────
void test_osc_float_arguments() {
  TEST_ASSERT_EQUAL_INT32(204, vizzz::oscFloatToLevel(0.8f));
  TEST_ASSERT_EQUAL_INT32(255, vizzz::oscFloatToLevel(1.0f));
  TEST_ASSERT_EQUAL_INT32(0, vizzz::oscFloatToLevel(0.0f));
  TEST_ASSERT_EQUAL_INT32(128, vizzz::oscFloatToLevel(128.0f));
  TEST_ASSERT_EQUAL_INT32(255, vizzz::oscFloatToLevel(1e30f));
  TEST_ASSERT_EQUAL_INT32(0, vizzz::oscFloatToLevel(NAN));
  TEST_ASSERT_EQUAL_INT32(3, vizzz::oscFloatToInt(2.6f));
  TEST_ASSERT_EQUAL_INT32(0, vizzz::oscFloatToInt(INFINITY));
  const uint8_t bundle[16] = {'#','b','u','n','d','l','e',0, 0,0,0,0,0,0,0,1};
  TEST_ASSERT_TRUE(vizzz::oscIsBundle(bundle, 16));
  TEST_ASSERT_FALSE(vizzz::oscIsBundle((const uint8_t*)"/ch/1\0\0\0,i\0\0\0\0\0\1", 16));
}

// 2-H2: the dangerous routes are POST + header; di's routes stay plain GET.
void test_route_table_kinds() {
  using vizzz::RouteKind;
  const char* consoleOnly[] = {"/reboot", "/factory-reset", "/wifi/set", "/wifi/forget",
    "/netmode/set", "/node/set", "/web/set", "/artnet/set", "/artout/set", "/artout/peer",
    "/mode/set", "/mode/fallback", "/safety/set", "/net/blackout", "/net/full",
    "/net/master", "/net/scene/recall", "/peer/cmd", "/scene/save", "/fixture/set",
    "/cue/set", "/group/set", "/color/set", "/fx/set", "/panic"};
  for (const char* p : consoleOnly) {
    int i = vizzz::findRoute(p);
    TEST_ASSERT_TRUE_MESSAGE(i >= 0, p);
    TEST_ASSERT_TRUE_MESSAGE(vizzz::kRoutes[i].kind == RouteKind::Console, p);
  }
  const char* diGet[] = {"/set", "/master", "/blackout", "/full"};
  for (const char* p : diGet)
    TEST_ASSERT_TRUE_MESSAGE(vizzz::kRoutes[vizzz::findRoute(p)].kind == RouteKind::Operator, p);
  const char* diRead[] = {"/status", "/page", "/manifest.json", "/node/manifest"};
  for (const char* p : diRead)
    TEST_ASSERT_TRUE_MESSAGE(vizzz::kRoutes[vizzz::findRoute(p)].kind == RouteKind::GetShared, p);
  TEST_ASSERT_EQUAL_INT(-1, vizzz::findRoute("/nope"));
  // ESPAsyncWebServer serves "<uri>/..." with the <uri> handler: no route may
  // be another route's prefix + "/" (it would be swallowed).
  for (size_t a = 0; a < vizzz::kRouteCount; a++) {
    const char* pa = vizzz::kRoutes[a].path;
    size_t la = strlen(pa);
    if (la <= 1) continue;
    for (size_t b = 0; b < vizzz::kRouteCount; b++) {
      const char* pb = vizzz::kRoutes[b].path;
      TEST_ASSERT_FALSE_MESSAGE(strncmp(pb, pa, la) == 0 && pb[la] == '/', pb);
    }
  }
  // No path twice.
  for (size_t a = 0; a < vizzz::kRouteCount; a++)
    for (size_t b = a + 1; b < vizzz::kRouteCount; b++)
      TEST_ASSERT_TRUE_MESSAGE(strcmp(vizzz::kRoutes[a].path, vizzz::kRoutes[b].path) != 0,
                               vizzz::kRoutes[a].path);
}

void test_route_gate_decisions() {
  using vizzz::Gate; using vizzz::HttpMethod; using vizzz::RouteKind;
  // Console: GET 405, POST without header 403, with header allowed, preflight 403.
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Console, HttpMethod::Get, true) == Gate::NotAllowed);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Console, HttpMethod::Post, false) == Gate::NeedHeader);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Console, HttpMethod::Post, true) == Gate::Allow);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Console, HttpMethod::Options, true) == Gate::PreflightDenied);
  // Operator: GET and POST, no header needed; anything else 405.
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Operator, HttpMethod::Get, false) == Gate::Allow);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Operator, HttpMethod::Post, false) == Gate::Allow);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Operator, HttpMethod::Other, false) == Gate::NotAllowed);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::Operator, HttpMethod::Options, false) == Gate::PreflightDenied);
  // Reads: GET only; shared reads answer a preflight.
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::GetShared, HttpMethod::Get, false) == Gate::Allow);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::GetShared, HttpMethod::Post, true) == Gate::NotAllowed);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::GetShared, HttpMethod::Options, false) == Gate::Preflight);
  TEST_ASSERT_TRUE(vizzz::gateRequest(RouteKind::GetLocal, HttpMethod::Options, false) == Gate::PreflightDenied);
  TEST_ASSERT_EQUAL_STRING("POST", vizzz::routeMethods(RouteKind::Console));
  TEST_ASSERT_EQUAL_STRING("GET, POST", vizzz::routeMethods(RouteKind::Operator));
  TEST_ASSERT_TRUE(vizzz::isConsoleHeaderValue("1"));
  TEST_ASSERT_FALSE(vizzz::isConsoleHeaderValue(""));
  TEST_ASSERT_FALSE(vizzz::isConsoleHeaderValue("11"));
  TEST_ASSERT_FALSE(vizzz::isConsoleHeaderValue(nullptr));
}

// 2-L7: WebSocket only from the node's own origin (or no Origin at all).
void test_ws_origin_rule() {
  TEST_ASSERT_TRUE(vizzz::wsOriginAllowed(nullptr, "192.168.4.1"));
  TEST_ASSERT_TRUE(vizzz::wsOriginAllowed("http://192.168.4.1", "192.168.4.1"));
  TEST_ASSERT_TRUE(vizzz::wsOriginAllowed("http://vizzz.local:80", "vizzz.local:80"));
  TEST_ASSERT_FALSE(vizzz::wsOriginAllowed("http://evil.example", "192.168.4.1"));
  TEST_ASSERT_FALSE(vizzz::wsOriginAllowed("https://192.168.4.1", "192.168.4.1"));
  TEST_ASSERT_FALSE(vizzz::wsOriginAllowed("http://192.168.4.1.evil", "192.168.4.1"));
}

// 2-M5: numbers are digits and in range, or rejected (no toInt() 0-fallback).
void test_parse_uint_arg_strict() {
  uint32_t v = 77;
  TEST_ASSERT_TRUE(vizzz::parseUintArg("1", 1, 512, &v));   TEST_ASSERT_EQUAL_UINT32(1, v);
  TEST_ASSERT_TRUE(vizzz::parseUintArg("512", 1, 512, &v)); TEST_ASSERT_EQUAL_UINT32(512, v);
  TEST_ASSERT_TRUE(vizzz::parseUintArg("0007", 0, 255, &v)); TEST_ASSERT_EQUAL_UINT32(7, v);
  TEST_ASSERT_TRUE(vizzz::parseUintArg("4294967295", 0, 4294967295u, &v));
  v = 77;
  const char* bad[] = {"", "abc", "1a", "a1", " 1", "1 ", "+1", "-1", "1.5", "0x10", "1e3",
                       "513", "0", "99999999999", "42949672960"};
  for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(vizzz::parseUintArg(b, 1, 512, &v), b);
  TEST_ASSERT_EQUAL_UINT32(77, v);  // untouched on failure
  TEST_ASSERT_FALSE(vizzz::parseUintArg(nullptr, 0, 1, &v));
}

void test_parse_int_arg_strict() {
  int32_t v = 5;
  TEST_ASSERT_TRUE(vizzz::parseIntArg("-12", -100, 100, &v)); TEST_ASSERT_EQUAL_INT32(-12, v);
  TEST_ASSERT_TRUE(vizzz::parseIntArg("100", -100, 100, &v)); TEST_ASSERT_EQUAL_INT32(100, v);
  TEST_ASSERT_TRUE(vizzz::parseIntArg("-2147483648", INT32_MIN, INT32_MAX, &v));
  TEST_ASSERT_EQUAL_INT32(INT32_MIN, v);
  const char* bad[] = {"-", "--1", "-a", "1-", "101", "-101", "2147483648", ""};
  for (const char* b : bad) TEST_ASSERT_FALSE_MESSAGE(vizzz::parseIntArg(b, -100, 100, &v), b);
}

// 2-M7: ACAO only on shared JSON reads.
void test_acao_only_on_shared_reads() {
  size_t shared = 0;
  for (size_t i = 0; i < vizzz::kRouteCount; i++) {
    bool acao = vizzz::routeSendsAcao(vizzz::kRoutes[i].kind);
    if (acao) shared++;
    if (vizzz::kRoutes[i].kind == vizzz::RouteKind::Console ||
        vizzz::kRoutes[i].kind == vizzz::RouteKind::Operator)
      TEST_ASSERT_FALSE_MESSAGE(acao, vizzz::kRoutes[i].path);
  }
  TEST_ASSERT_EQUAL_UINT32(12, shared);
  TEST_ASSERT_TRUE(vizzz::routeSendsAcao(vizzz::kRoutes[vizzz::findRoute("/status")].kind));
  TEST_ASSERT_TRUE(vizzz::routeSendsAcao(vizzz::kRoutes[vizzz::findRoute("/discover")].kind));
  TEST_ASSERT_FALSE(vizzz::routeSendsAcao(vizzz::kRoutes[vizzz::findRoute("/wifi/scan")].kind));
  TEST_ASSERT_FALSE(vizzz::routeSendsAcao(vizzz::kRoutes[vizzz::findRoute("/reboot")].kind));
}

// ── ENTTEC DMX USB PRO widget protocol (ENTTEC "DMX USB PRO API") ──────────
struct WidgetSink {
  int count = 0;
  uint8_t label[8] = {0};
  uint16_t len[8] = {0};
  uint8_t vals[512];
  uint16_t applied = 0;
  void operator()(uint8_t l, const uint8_t* d, uint16_t n) {
    if (count < 8) { label[count] = l; len[count] = n; }
    count++;
    if (l == vizzz::widget::kLabelSendDmx) applied = vizzz::widget::applyDmx(d, n, vals);
  }
};

static size_t widgetDmxFrame(uint8_t* out, uint16_t channels, uint8_t fill) {
  static uint8_t data[513];
  data[0] = 0;  // DMX start code
  for (uint16_t i = 1; i <= channels; i++) data[i] = uint8_t(fill + i);
  return vizzz::widget::buildFrame(out, 605, vizzz::widget::kLabelSendDmx, data, uint16_t(channels + 1));
}

void test_widget_full_frame_513_bytes_sets_channels_1_to_512() {
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  WidgetSink sink; memset(sink.vals, 0, sizeof(sink.vals));
  uint8_t f[605];
  size_t n = widgetDmxFrame(f, 512, 0);
  TEST_ASSERT_EQUAL_UINT32(518, n);
  TEST_ASSERT_EQUAL_HEX8(0x7E, f[0]);
  TEST_ASSERT_EQUAL_HEX8(0x01, f[2]);  // 513 = 0x0201, LSB first
  TEST_ASSERT_EQUAL_HEX8(0x02, f[3]);
  TEST_ASSERT_EQUAL_HEX8(0xE7, f[517]);
  vizzz::widget::feed(p, f, n, sink);
  TEST_ASSERT_EQUAL_INT(1, sink.count);
  TEST_ASSERT_EQUAL_UINT16(513, sink.len[0]);
  TEST_ASSERT_EQUAL_UINT16(512, sink.applied);
  TEST_ASSERT_EQUAL_UINT8(1, sink.vals[0]);      // channel 1
  TEST_ASSERT_EQUAL_UINT8(uint8_t(512), sink.vals[511]);  // channel 512
  TEST_ASSERT_EQUAL_UINT32(1, p.frames);
  TEST_ASSERT_EQUAL_UINT32(0, p.errors);
}

void test_widget_frame_split_across_reads() {
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  WidgetSink sink; memset(sink.vals, 0, sizeof(sink.vals));
  uint8_t f[605];
  size_t n = widgetDmxFrame(f, 512, 7);
  const size_t cuts[] = {1, 3, 4, 100, 517};
  size_t at = 0;
  for (size_t c : cuts) { vizzz::widget::feed(p, f + at, c - at, sink); TEST_ASSERT_EQUAL_INT(0, sink.count); at = c; }
  vizzz::widget::feed(p, f + at, n - at, sink);
  TEST_ASSERT_EQUAL_INT(1, sink.count);
  TEST_ASSERT_EQUAL_UINT8(8, sink.vals[0]);
}

void test_widget_garbage_and_cut_frame_do_not_cost_later_frames() {
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  WidgetSink sink; memset(sink.vals, 0, sizeof(sink.vals));
  // Boot-log text, a stray start byte, then a frame.
  const char* text = "vizzz boot ~ 0x7E\r\n";
  vizzz::widget::feed(p, (const uint8_t*)text, strlen(text), sink);
  const uint8_t junk[] = {0x00, 0x7E, 0x05, 0xFF};
  vizzz::widget::feed(p, junk, sizeof(junk), sink);
  uint8_t f[605];
  size_t n = widgetDmxFrame(f, 24, 0);
  vizzz::widget::feed(p, f, n, sink);
  // A frame cut off half way (host restarted), then two whole frames.
  vizzz::widget::feed(p, f, 12, sink);
  vizzz::widget::feed(p, f, n, sink);
  vizzz::widget::feed(p, f, n, sink);
  TEST_ASSERT_EQUAL_INT(3, sink.count);
  TEST_ASSERT_TRUE(p.errors >= 2);
  TEST_ASSERT_EQUAL_UINT16(24, sink.applied);
}

void test_widget_oversize_length_rejected() {
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  WidgetSink sink; memset(sink.vals, 0, sizeof(sink.vals));
  const uint8_t big[] = {0x7E, 0x06, 0x59, 0x02};  // 601 > 600
  vizzz::widget::feed(p, big, sizeof(big), sink);
  TEST_ASSERT_EQUAL_UINT32(1, p.errors);
  TEST_ASSERT_EQUAL_UINT16(0, p.rawLen);
  uint8_t f[605];
  size_t n = widgetDmxFrame(f, 24, 0);
  vizzz::widget::feed(p, f, n, sink);
  TEST_ASSERT_EQUAL_INT(1, sink.count);
  uint8_t out[700];
  static uint8_t data[601];
  TEST_ASSERT_EQUAL_UINT32(0, vizzz::widget::buildFrame(out, sizeof(out), 6, data, 601));
}

void test_widget_short_frame_leaves_rest_untouched() {
  uint8_t vals[512];
  memset(vals, 0x55, sizeof(vals));
  uint8_t data[25] = {0};
  for (int i = 1; i <= 24; i++) data[i] = 200;
  TEST_ASSERT_EQUAL_UINT16(24, vizzz::widget::applyDmx(data, 25, vals));
  TEST_ASSERT_EQUAL_UINT8(200, vals[23]);
  TEST_ASSERT_EQUAL_UINT8(0x55, vals[24]);
  TEST_ASSERT_EQUAL_UINT8(0x55, vals[511]);
  data[0] = 0xCC;  // a non-dimmer start code (e.g. text/test packet) is not applied
  TEST_ASSERT_EQUAL_UINT16(0, vizzz::widget::applyDmx(data, 25, vals));
  TEST_ASSERT_EQUAL_UINT16(0, vizzz::widget::applyDmx(data, 1, vals));
}

void test_widget_params_reply_layout() {
  vizzz::widget::Params prm{0x0102, 176, 12, 43};
  uint8_t out[700];
  const uint8_t req[] = {0, 0};  // what di's identify() sends
  size_t n = vizzz::widget::buildParamsReply(out, sizeof(out), prm, vizzz::widget::paramsRequestSize(req, 2));
  TEST_ASSERT_EQUAL_UINT32(10, n);
  const uint8_t want[] = {0x7E, 3, 5, 0, 0x02, 0x01, 16, 1, 40, 0xE7};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 10);
  // User-config bytes come back zero, at most 508.
  n = vizzz::widget::buildParamsReply(out, sizeof(out), prm, 1000);
  TEST_ASSERT_EQUAL_UINT32(5 + 5 + 508, n);
  TEST_ASSERT_EQUAL_HEX8(0xE7, out[n - 1]);
  TEST_ASSERT_EQUAL_UINT8(9, vizzz::widget::usToUnits(10, 9, 127));    // clamped up to the API floor
  TEST_ASSERT_EQUAL_UINT16(3, vizzz::widget::paramsRequestSize((const uint8_t[]){3, 0}, 2));
}

void test_widget_serial_reply_bcd() {
  uint8_t out[16];
  size_t n = vizzz::widget::buildSerialReply(out, sizeof(out), 12345678);
  TEST_ASSERT_EQUAL_UINT32(9, n);
  const uint8_t want[] = {0x7E, 10, 4, 0, 0x78, 0x56, 0x34, 0x12, 0xE7};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 9);
  // A reply fed back through the parser is one well-formed frame.
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  WidgetSink sink;
  vizzz::widget::feed(p, out, n, sink);
  TEST_ASSERT_EQUAL_INT(1, sink.count);
  TEST_ASSERT_EQUAL_UINT8(10, sink.label[0]);
}

void test_widget_stalled_frame_resync_replays() {
  // A cut frame that never completes is dropped by resync() (the firmware calls
  // it after a gap); a whole frame that arrived inside it is recovered.
  static vizzz::widget::Parser p; vizzz::widget::parserReset(p);
  WidgetSink sink; memset(sink.vals, 0, sizeof(sink.vals));
  uint8_t f[605];
  size_t n = widgetDmxFrame(f, 512, 0);
  vizzz::widget::feed(p, f, 200, sink);          // cut 512-ch frame
  const uint8_t q[] = {0x7E, 3, 2, 0, 0, 0, 0xE7};  // label 3 request inside its data
  vizzz::widget::feed(p, q, sizeof(q), sink);
  TEST_ASSERT_EQUAL_INT(0, sink.count);
  vizzz::widget::resync(p, sink);
  TEST_ASSERT_EQUAL_INT(1, sink.count);
  TEST_ASSERT_EQUAL_UINT8(3, sink.label[0]);
  (void)n;
}

// README "USB" modes table: USB is a third network-class source, HTP with Art-Net/sACN.
void test_merge_usb_obeys_modes_like_artnet() {
  using namespace vizzz::merge;
  // HTP among network sources; an idle source contributes nothing.
  TEST_ASSERT_EQUAL_UINT8(180, networkIn(true, 100, false, 250, true, 180));
  TEST_ASSERT_EQUAL_UINT8(250, networkIn(true, 100, true, 250, true, 180));
  TEST_ASSERT_EQUAL_UINT8(0, networkIn(false, 100, false, 250, false, 180));
  // USB alone (netActive true, netIn 180), web 50, held output 77.
  TEST_ASSERT_EQUAL_UINT8(50,  forMode(kWebOnly, true, 180, 50, 77, false));     // WEB_ONLY ignores USB
  TEST_ASSERT_EQUAL_UINT8(180, forMode(kArtnetOnly, true, 180, 50, 77, false));  // ARTNET_ONLY: USB alone
  TEST_ASSERT_EQUAL_UINT8(180, forMode(kHtp, true, 180, 50, 77, false));         // MERGE_HTP: max
  TEST_ASSERT_EQUAL_UINT8(200, forMode(kHtp, true, 180, 200, 77, false));
  // USB gone (2.5 s): ARTNET_ONLY holds or falls back; HTP returns to web.
  TEST_ASSERT_EQUAL_UINT8(77, forMode(kArtnetOnly, false, 0, 50, 77, false));
  TEST_ASSERT_EQUAL_UINT8(50, forMode(kArtnetOnly, false, 0, 50, 77, true));
  TEST_ASSERT_EQUAL_UINT8(50, forMode(kHtp, false, 0, 50, 77, false));
}

static int runAll() {
  UNITY_BEGIN();
  RUN_TEST(test_pack_universe_uses_15bit_layout);
  RUN_TEST(test_apply_master_scales_levels);
  RUN_TEST(test_clamp_level_applies_ceiling);
  RUN_TEST(test_slew_toward_limits_step_size);
  RUN_TEST(test_wpa_passphrase_bounds);
  RUN_TEST(test_sanitize_host_label);
  RUN_TEST(test_parse_ipv4_strict);
  RUN_TEST(test_cue_timing);
  RUN_TEST(test_elapsed_at_least_wraps);
  RUN_TEST(test_sta_retry_backoff);
  RUN_TEST(test_clamp_channel_range);
  RUN_TEST(test_artpoll_14_bytes_is_detected);
  RUN_TEST(test_artdmx_parse_fields_and_limits);
  RUN_TEST(test_artdmx_sequence_skips_zero);
  RUN_TEST(test_artpollreply_layout_matches_spec_and_di);
  RUN_TEST(test_artnet_merge_two_sources_htp_third_ignored);
  RUN_TEST(test_artnet_merge_drops_out_of_order);
  RUN_TEST(test_sacn_universe_is_port_address_plus_one);
  RUN_TEST(test_sacn_parse_layers);
  RUN_TEST(test_sacn_preview_terminated_priority_sequence);
  RUN_TEST(test_osc_float_arguments);
  RUN_TEST(test_route_table_kinds);
  RUN_TEST(test_route_gate_decisions);
  RUN_TEST(test_ws_origin_rule);
  RUN_TEST(test_parse_uint_arg_strict);
  RUN_TEST(test_parse_int_arg_strict);
  RUN_TEST(test_acao_only_on_shared_reads);
  RUN_TEST(test_widget_full_frame_513_bytes_sets_channels_1_to_512);
  RUN_TEST(test_widget_frame_split_across_reads);
  RUN_TEST(test_widget_garbage_and_cut_frame_do_not_cost_later_frames);
  RUN_TEST(test_widget_oversize_length_rejected);
  RUN_TEST(test_widget_short_frame_leaves_rest_untouched);
  RUN_TEST(test_widget_params_reply_layout);
  RUN_TEST(test_widget_serial_reply_bcd);
  RUN_TEST(test_widget_stalled_frame_resync_replays);
  RUN_TEST(test_merge_usb_obeys_modes_like_artnet);
  return UNITY_END();
}

#if defined(ARDUINO)
void setup() { runAll(); }
void loop() {}
#else
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  return runAll();
}
#endif
