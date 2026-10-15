#include <unity.h>

#include <string.h>

#include "vizzz_core.h"

#if !defined(ARDUINO)
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "desk_ui_gz.h"
#endif

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

static void test_health_fields_key_list_and_values() {
  const vizzz::HealthFacts h{123456u, 65536u, 3u};
  char buf[128];
  int n = vizzz::healthJsonFields(buf, sizeof(buf), h);
  TEST_ASSERT_TRUE(n > 0 && size_t(n) < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("\"heap_free\":123456,\"heap_max_block\":65536,\"ws_clients\":3", buf);
  TEST_ASSERT_EQUAL_UINT(3, vizzz::kHealthKeyCount);
  const char* at = buf;
  for (size_t i = 0; i < vizzz::kHealthKeyCount; i++) {
    char q[40];
    snprintf(q, sizeof(q), "\"%s\":", vizzz::kHealthKeys[i]);
    const char* f = strstr(at, q);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, vizzz::kHealthKeys[i]);
    TEST_ASSERT_NULL_MESSAGE(strstr(f + 1, q), vizzz::kHealthKeys[i]);
    at = f;
  }
}

static void test_health_fields_truncation_reported() {
  const vizzz::HealthFacts h{4294967295u, 4294967295u, 4294967295u};
  char small[16];
  TEST_ASSERT_TRUE(size_t(vizzz::healthJsonFields(small, sizeof(small), h)) >= sizeof(small));
  char buf[96];
  TEST_ASSERT_TRUE(size_t(vizzz::healthJsonFields(buf, sizeof(buf), h)) < sizeof(buf));
}

#if !defined(ARDUINO)
// desk_ui.lock is JSON written by tools/fetch_desk_ui.py (one key per line, indent 2); `pio test` and the
// g++ build both run from the project root. A tiny scan is enough: find the file's "path", then the next keys.
static bool lockValue(const char* lock, const char* path, const char* key, char* out, size_t n) {
  char pat[96];
  snprintf(pat, sizeof(pat), "\"path\": \"%s\"", path);
  const char* p = strstr(lock, pat);
  if (!p) return false;
  snprintf(pat, sizeof(pat), "\"%s\": ", key);
  p = strstr(p, pat);
  if (!p) return false;
  p += strlen(pat);
  if (*p == '"') p++;
  size_t i = 0;
  while (*p && *p != '"' && *p != ',' && *p != '\n' && i + 1 < n) out[i++] = *p++;
  out[i] = 0;
  return true;
}

static char* readLock() {
  FILE* f = fopen("desk_ui.lock", "rb");
  if (!f) return nullptr;
  static char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

void test_desk_ui_header_matches_lock() {
  char* lock = readLock();
  TEST_ASSERT_NOT_NULL_MESSAGE(lock, "desk_ui.lock not readable from the project root");
  char v[80];
  TEST_ASSERT_TRUE(lockValue(lock, "index.html", "sha256", v, sizeof(v)));  // lock is parsable
  const char* c = strstr(lock, "\"commit\": \"");
  TEST_ASSERT_NOT_NULL(c);
  c += strlen("\"commit\": \"");
  TEST_ASSERT_EQUAL_INT(0, strncmp(c, DESK_UI_COMMIT, strlen(DESK_UI_COMMIT)));
  TEST_ASSERT_EQUAL_CHAR('"', c[strlen(DESK_UI_COMMIT)]);
  const char* cv = strstr(lock, "\"contract_version\": ");
  TEST_ASSERT_NOT_NULL(cv);
  TEST_ASSERT_EQUAL_INT(atoi(cv + strlen("\"contract_version\": ")), DESK_UI_CONTRACT_VERSION);
  for (size_t i = 0; i < kDeskUiFileCount; i++) {
    const DeskUiFile& f = kDeskUiFiles[i];
    TEST_ASSERT_EQUAL_CHAR('/', f.path[0]);
    TEST_ASSERT_TRUE_MESSAGE(lockValue(lock, f.path + 1, "gz_bytes", v, sizeof(v)), f.path);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)atol(v), (uint32_t)f.len, f.path);
    TEST_ASSERT_TRUE_MESSAGE(lockValue(lock, f.path + 1, "sha256", v, sizeof(v)), f.path);
    TEST_ASSERT_EQUAL_UINT32(16, strlen(f.etag));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(v, f.etag, 16), f.path);
    TEST_ASSERT_EQUAL_HEX8(0x1f, f.data[0]);  // gzip magic
    TEST_ASSERT_EQUAL_HEX8(0x8b, f.data[1]);
  }
}

void test_desk_ui_table_has_exactly_the_locked_files() {
  char* lock = readLock();
  TEST_ASSERT_NOT_NULL(lock);
  size_t inLock = 0;
  for (const char* p = lock; (p = strstr(p, "\"path\": \"")) != nullptr; p++) inLock++;
  TEST_ASSERT_EQUAL_UINT32(inLock, kDeskUiFileCount);
  for (size_t i = 0; i < kDeskUiFileCount; i++) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"path\": \"%s\"", kDeskUiFiles[i].path + 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(lock, pat), kDeskUiFiles[i].path);
    for (size_t j = i + 1; j < kDeskUiFileCount; j++)
      TEST_ASSERT_NOT_EQUAL(0, strcmp(kDeskUiFiles[i].path, kDeskUiFiles[j].path));
  }
}
#endif

// ── Desk bridge core (plan step V3, include/vizzz_desk.h) ─────────────────────
namespace dk = vizzz::desk;

static dk::NodeFixture mkFixture(const char* name, uint16_t start, const char* profile, uint8_t x, uint8_t y,
                                 bool en = true) {
  dk::NodeFixture f;
  memset(&f, 0, sizeof(f));
  strncpy(f.name, name, sizeof(f.name) - 1);
  f.start = start;
  int p = dk::profileIndex(profile);
  f.profile = p < 0 ? dk::kNoProfile : uint8_t(p);
  f.x = x;
  f.y = y;
  f.enabled = en;
  return f;
}

// The node's profiles, copied by hand from di.iiii serverXR/src/lighting/engine.js PROFILES
// @ 091a1a6 ("name cat: channels"). A change on either side that is not made on both fails here.
static void test_desk_profiles_match_di_engine() {
  static const char* const kDi[] = {
    "dimmer _GENERIC: dimmer", "ds _GENERIC: dimmer,strobe", "drgb _GENERIC: dimmer,r,g,b",
    "drgbs _GENERIC: dimmer,r,g,b,strobe", "drgbw _GENERIC: dimmer,r,g,b,w",
    "drgbwa _GENERIC: dimmer,r,g,b,w,a", "rgb _GENERIC: r,g,b", "rgba _GENERIC: r,g,b,a",
    "rgbw _GENERIC: r,g,b,w", "rgbaw _GENERIC: r,g,b,a,w", "dwwcw _GENERIC: dimmer,warm,cool",
    "ptdrgb _MOVING: pan,tilt,dimmer,r,g,b",
    "ppttdrgbw _MOVING: pan,panFine,tilt,tiltFine,dimmer,r,g,b,w",
  };
  TEST_ASSERT_EQUAL_UINT(sizeof(kDi) / sizeof(kDi[0]), dk::kProfileCount);
  for (uint8_t i = 0; i < dk::kProfileCount; i++) {
    const dk::Profile& p = dk::kProfiles[i];
    char line[160];
    size_t n = (size_t)snprintf(line, sizeof(line), "%s %s: ", p.name, p.cat);
    TEST_ASSERT_TRUE(p.count >= 1 && p.count <= dk::kMaxProfileChannels);
    for (uint8_t c = 0; c < p.count; c++)
      n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s", c ? "," : "", dk::roleName(p.roles[c]));
    TEST_ASSERT_EQUAL_STRING(kDi[i], line);
    for (uint8_t a = 0; a < p.count; a++)  // di: a profile never repeats a role
      for (uint8_t b = uint8_t(a + 1); b < p.count; b++) TEST_ASSERT_TRUE(p.roles[a] != p.roles[b]);
    TEST_ASSERT_EQUAL_INT(i, dk::profileIndex(p.name));
  }
  TEST_ASSERT_EQUAL_INT(-1, dk::profileIndex("spot"));
  // roles.js ROLE_DEFAULTS and roleKind() for the 13 roles the profiles use.
  static const char* const kRoleDi[] = {
    "dimmer 255 level", "r 255 emitter", "g 255 emitter", "b 255 emitter", "w 0 emitter", "a 0 emitter",
    "warm 255 emitter", "cool 0 emitter", "strobe 0 control", "pan 128 position", "panFine 0 fine",
    "tilt 128 position", "tiltFine 0 fine",
  };
  for (uint8_t r = 0; r < dk::kRoleCount; r++) {
    char line[48];
    snprintf(line, sizeof(line), "%s %u %s", dk::kRoles[r].name, dk::kRoles[r].def,
             dk::kKindNames[uint8_t(dk::kRoles[r].kind)]);
    TEST_ASSERT_EQUAL_STRING(kRoleDi[r], line);
    TEST_ASSERT_EQUAL_INT(r, dk::roleIndex(dk::kRoles[r].name));
  }
  TEST_ASSERT_EQUAL_INT(-1, dk::roleIndex("gobo"));
}

static void test_desk_fixture_roles_round_trip_through_web_layer() {
  uint8_t web[512];
  memset(web, 0, sizeof(web));
  const dk::NodeFixture f = mkFixture("par", 10, "drgbw", 0, 0);
  TEST_ASSERT_EQUAL_UINT16(14, dk::fixtureEnd(f));
  const dk::RoleValue vals[] = {{dk::Role::Dimmer, 200}, {dk::Role::R, 1}, {dk::Role::G, 2},
                                {dk::Role::B, 300}, {dk::Role::W, -5}, {dk::Role::Pan, 77}};
  dk::WriteCount c = dk::writeValues(web, f, vals, 6);
  TEST_ASSERT_EQUAL_UINT8(5, c.applied);
  TEST_ASSERT_EQUAL_UINT8(1, c.ignored);                 // drgbw has no pan
  TEST_ASSERT_EQUAL_UINT8(200, web[9]);                  // channel 10 = dimmer (offset 0)
  TEST_ASSERT_EQUAL_UINT8(1, web[10]);
  TEST_ASSERT_EQUAL_UINT8(2, web[11]);
  TEST_ASSERT_EQUAL_UINT8(255, web[12]);                 // clamped
  TEST_ASSERT_EQUAL_UINT8(0, web[13]);                   // clamped
  TEST_ASSERT_EQUAL_UINT8(0, web[8]);                    // neighbours untouched
  TEST_ASSERT_EQUAL_UINT8(0, web[14]);
  TEST_ASSERT_EQUAL_INT(200, dk::readRole(web, f, dk::Role::Dimmer));
  TEST_ASSERT_EQUAL_INT(255, dk::readRole(web, f, dk::Role::B));
  TEST_ASSERT_EQUAL_INT(-1, dk::readRole(web, f, dk::Role::Pan));
  // Same channels, another order: rgbw at 11 reads r,g,b,w from 11..14.
  const dk::NodeFixture g = mkFixture("bar", 11, "rgbw", 0, 0);
  TEST_ASSERT_EQUAL_INT(1, dk::readRole(web, g, dk::Role::R));
  TEST_ASSERT_EQUAL_INT(0, dk::readRole(web, g, dk::Role::W));
  // rgb has no dimmer: di's virtual dimmer cannot be stored, the write is refused.
  const dk::NodeFixture h = mkFixture("rgb", 100, "rgb", 0, 0);
  TEST_ASSERT_FALSE(dk::writeRole(web, h, dk::Role::Dimmer, 255));
  TEST_ASSERT_EQUAL_INT(-1, dk::readRole(web, h, dk::Role::Dimmer));
  // The end of the universe.
  TEST_ASSERT_TRUE(dk::fixtureFits(508, uint8_t(dk::profileIndex("drgbw"))));
  TEST_ASSERT_FALSE(dk::fixtureFits(509, uint8_t(dk::profileIndex("drgbw"))));
  TEST_ASSERT_FALSE(dk::fixtureFits(0, 0));
  TEST_ASSERT_FALSE(dk::fixtureFits(1, dk::kNoProfile));
  const dk::NodeFixture e = mkFixture("edge", 510, "drgbw", 0, 0);  // an unrepaired blob
  TEST_ASSERT_TRUE(dk::writeRole(web, e, dk::Role::G, 9));          // channel 512
  TEST_ASSERT_EQUAL_UINT8(9, web[511]);
  TEST_ASSERT_FALSE(dk::writeRole(web, e, dk::Role::B, 9));         // channel 513: refused
  TEST_ASSERT_EQUAL_INT(-1, dk::readRole(web, e, dk::Role::W));
  // 16-bit pan: coarse and fine are separate channels.
  const dk::NodeFixture m = mkFixture("head", 200, "ppttdrgbw", 0, 0);
  TEST_ASSERT_TRUE(dk::writeRole(web, m, dk::Role::PanFine, 42));
  TEST_ASSERT_EQUAL_UINT8(42, web[200]);
  TEST_ASSERT_EQUAL_INT(42, dk::readRole(web, m, dk::Role::PanFine));
}

static void test_desk_stage_position_round_trips_every_byte() {
  for (int v = 0; v < 256; v++) {
    uint16_t milli = dk::posToMilli(uint8_t(v));
    TEST_ASSERT_TRUE(milli <= 1000);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(v), dk::posFromMilli(milli));
  }
  TEST_ASSERT_EQUAL_UINT16(0, dk::posToMilli(0));
  TEST_ASSERT_EQUAL_UINT16(1000, dk::posToMilli(255));
  TEST_ASSERT_EQUAL_UINT8(255, dk::posFromMilli(5000));
}

static void test_desk_v1_fixture_migration() {
  bool exact = false;
  dk::NodeFixture f = dk::migrateV1Fixture("F1", 1, 3, 32, 64, true, &exact);
  TEST_ASSERT_TRUE(exact);
  TEST_ASSERT_EQUAL_STRING("rgb", dk::kProfiles[f.profile].name);
  TEST_ASSERT_EQUAL_UINT16(1, f.start);
  TEST_ASSERT_EQUAL_UINT16(3, dk::fixtureEnd(f));
  TEST_ASSERT_EQUAL_UINT8(32, f.x);
  TEST_ASSERT_EQUAL_UINT8(64, f.y);
  TEST_ASSERT_TRUE(f.enabled);
  static const char* const kByWidth[] = {nullptr, "dimmer", "ds", "rgb", "drgb", "drgbw", "drgbwa",
                                         nullptr, nullptr, "ppttdrgbw"};
  for (uint16_t w = 1; w < 10; w++) {
    f = dk::migrateV1Fixture("x", 100, uint16_t(100 + w - 1), 0, 0, true, &exact);
    TEST_ASSERT_EQUAL(kByWidth[w] != nullptr, exact);
    TEST_ASSERT_EQUAL_STRING(kByWidth[w] ? kByWidth[w] : "dimmer", dk::kProfiles[f.profile].name);
  }
  f = dk::migrateV1Fixture("a-very-long-fixture-name", 512, 600, 0, 0, false, &exact);
  TEST_ASSERT_FALSE(exact);
  TEST_ASSERT_EQUAL_UINT16(512, f.start);
  TEST_ASSERT_EQUAL_UINT8(15, strlen(f.name));
  TEST_ASSERT_TRUE(dk::fixtureFits(f.start, f.profile));
}

// Collects every chunk the writer hands over.
#if defined(ARDUINO)
constexpr size_t kLogCap = 4096;
#else
constexpr size_t kLogCap = 131072;
#endif
struct ChunkLog {
  char doc[kLogCap];
  size_t len;
  size_t chunks;
  size_t maxChunk;
  size_t stopAfter;   // 0 = never stop
};
static ChunkLog gLog;
static bool chunkSink(void* ctx, const char* d, size_t n) {
  ChunkLog& l = *static_cast<ChunkLog*>(ctx);
  TEST_ASSERT_TRUE(n > 0 && l.len + n <= sizeof(l.doc));
  memcpy(l.doc + l.len, d, n);
  l.len += n;
  l.chunks++;
  if (n > l.maxChunk) l.maxChunk = n;
  return l.stopAfter == 0 || l.chunks < l.stopAfter;
}
static void logReset(size_t stopAfter = 0) {
  gLog.len = 0; gLog.chunks = 0; gLog.maxChunk = 0; gLog.stopAfter = stopAfter;
  memset(gLog.doc, 0, sizeof(gLog.doc));
}

static void test_desk_writer_escapes_and_formats() {
  dk::JsonWriter w;
  logReset();
  w.begin(chunkSink, &gLog);
  w.obj();
  w.key("s"); w.str("a\"b\\c\nd\re\tf\x01g\x1fh/\xc3\xa9");
  w.key("n"); w.u(0);
  w.key("big"); w.u(4294967295u);
  w.key("neg"); w.i(-2147483647 - 1);
  w.key("p"); w.i(7);
  w.key("x"); w.arr(); w.unit255(0); w.unit255(128); w.unit255(255); w.unit255(51); w.endArr();
  w.key("id"); w.id('f', 32);
  w.key("t"); w.boolean(true);
  w.key("z"); w.null();
  w.key("e"); w.obj(); w.endObj();
  w.key("bounded"); w.str("abcdef", 3);
  w.endObj();
  TEST_ASSERT_TRUE(w.finish());
  TEST_ASSERT_EQUAL_STRING(
      "{\"s\":\"a\\\"b\\\\c\\nd\\re\\tf\\u0001g\\u001fh/\xc3\xa9\",\"n\":0,\"big\":4294967295,"
      "\"neg\":-2147483648,\"p\":7,\"x\":[0,0.502,1,0.2],\"id\":\"f32\",\"t\":true,\"z\":null,\"e\":{},"
      "\"bounded\":\"abc\"}",
      gLog.doc);
  TEST_ASSERT_EQUAL_UINT32(gLog.len, w.total);
}

static void test_desk_writer_chunk_bound_and_stop() {
  dk::JsonWriter w;
  logReset();
  w.begin(chunkSink, &gLog);
  w.arr();
  for (uint32_t k = 0; k < 3000; k++) w.u(k);
  w.endArr();
  TEST_ASSERT_TRUE(w.finish());
  TEST_ASSERT_TRUE(gLog.chunks > 20);
  TEST_ASSERT_TRUE(gLog.maxChunk <= dk::kChunk);
  TEST_ASSERT_TRUE(dk::kChunk <= 1024);
  TEST_ASSERT_EQUAL_UINT16(gLog.maxChunk, w.maxChunk);
  TEST_ASSERT_EQUAL_STRING_LEN("[0,1,2,", gLog.doc, 7);
  TEST_ASSERT_EQUAL_STRING(",2998,2999]", gLog.doc + gLog.len - 11);
  // A sink that refuses stops the writer: nothing more is handed over.
  logReset(2);
  w.begin(chunkSink, &gLog);
  w.arr();
  for (uint32_t k = 0; k < 3000; k++) w.u(k);
  w.endArr();
  TEST_ASSERT_FALSE(w.finish());
  TEST_ASSERT_EQUAL_size_t(2, gLog.chunks);
  TEST_ASSERT_TRUE(w.stopped);
  // A memory sink that is too small says so instead of truncating silently.
  char small[64];
  dk::MemorySink ms = {small, sizeof(small), 0, false};
  w.begin(dk::memorySink, &ms);
  w.arr();
  for (uint32_t k = 0; k < 3000; k++) w.u(k);
  w.endArr();
  TEST_ASSERT_FALSE(w.finish());
  TEST_ASSERT_TRUE(ms.overflow);
}

#if !defined(ARDUINO)
// ── A tiny JSON reader for the contract tests (validates and walks; no tree) ──
namespace jr {
static const char* ws(const char* p) {
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}
static const char* skipString(const char* p) {
  if (*p != '"') return nullptr;
  for (p++; *p && *p != '"'; p++) {
    if ((unsigned char)*p < 0x20) return nullptr;
    if (*p == '\\') {
      p++;
      if (*p == 'u') {
        for (int k = 1; k <= 4; k++)
          if (!isxdigit((unsigned char)p[k])) return nullptr;
        p += 4;
      } else if (!*p || !strchr("\"\\/bfnrt", *p)) {
        return nullptr;
      }
    }
  }
  return *p == '"' ? p + 1 : nullptr;
}
static const char* skipNumber(const char* p) {
  if (*p == '-') p++;
  if (*p == '0') p++;
  else if (*p >= '1' && *p <= '9') while (isdigit((unsigned char)*p)) p++;
  else return nullptr;
  if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return nullptr; while (isdigit((unsigned char)*p)) p++; }
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return nullptr;
    while (isdigit((unsigned char)*p)) p++;
  }
  return p;
}
static const char* skipValue(const char* p, int depth = 0) {
  p = ws(p);
  if (depth > 64) return nullptr;
  if (*p == '{' || *p == '[') {
    const char close = *p == '{' ? '}' : ']';
    const bool isObj = *p == '{';
    p = ws(p + 1);
    if (*p == close) return p + 1;
    for (;;) {
      if (isObj) {
        p = skipString(p);
        if (!p) return nullptr;
        p = ws(p);
        if (*p != ':') return nullptr;
        p++;
      }
      p = skipValue(p, depth + 1);
      if (!p) return nullptr;
      p = ws(p);
      if (*p == ',') { p = ws(p + 1); continue; }
      return *p == close ? p + 1 : nullptr;
    }
  }
  if (*p == '"') return skipString(p);
  if (!strncmp(p, "true", 4)) return p + 4;
  if (!strncmp(p, "false", 5)) return p + 5;
  if (!strncmp(p, "null", 4)) return p + 4;
  return skipNumber(p);
}
static bool valid(const char* doc) {
  const char* e = skipValue(doc);
  return e && *ws(e) == 0;
}
// i-th member of the object at obj: key copied out, value start returned.
static const char* nthMember(const char* obj, int i, char* key, size_t keyCap) {
  const char* p = ws(obj);
  if (*p != '{') return nullptr;
  p = ws(p + 1);
  if (*p == '}') return nullptr;
  for (int k = 0;; k++) {
    const char* ks = p;
    p = skipString(p);
    if (!p) return nullptr;
    const char* v = ws(ws(p) + 1);
    if (k == i) {
      size_t n = size_t(p - ks - 2);
      if (n >= keyCap) n = keyCap - 1;
      memcpy(key, ks + 1, n);
      key[n] = 0;
      return v;
    }
    p = ws(skipValue(v));
    if (*p != ',') return nullptr;
    p = ws(p + 1);
  }
}
static const char* member(const char* obj, const char* want) {
  char key[64];
  for (int i = 0;; i++) {
    const char* v = nthMember(obj, i, key, sizeof(key));
    if (!v) return nullptr;
    if (!strcmp(key, want)) return v;
  }
}
static const char* element(const char* arr, int idx) {
  const char* p = ws(arr);
  if (*p != '[') return nullptr;
  p = ws(p + 1);
  if (*p == ']') return nullptr;
  for (int k = 0;; k++) {
    if (k == idx) return p;
    p = ws(skipValue(p));
    if (*p != ',') return nullptr;
    p = ws(p + 1);
  }
}
// Dotted path; a numeric segment indexes an array.
static const char* at(const char* doc, const char* path) {
  const char* v = ws(doc);
  char seg[64];
  while (v && *path) {
    size_t n = strcspn(path, ".");
    memcpy(seg, path, n);
    seg[n] = 0;
    path += n + (path[n] == '.');
    v = (*v == '[') ? element(v, atoi(seg)) : member(v, seg);
  }
  return v;
}
static const char* typeOf(const char* v) {
  v = ws(v);
  switch (*v) {
    case '{': return "object";
    case '[': return "array";
    case '"': return "string";
    case 't': case 'f': return "boolean";
    case 'n': return "null";
    default: return "number";
  }
}
// want is one type or "a|b" as the contract writes it.
static bool typeMatches(const char* v, const char* want) {
  const char* t = typeOf(v);
  size_t tl = strlen(t);
  for (const char* p = want; *p;) {
    size_t n = strcspn(p, "|");
    if (n == tl && !strncmp(p, t, n)) return true;
    p += n + (p[n] == '|');
  }
  return false;
}
static void str(const char* v, char* out, size_t cap) {  // plain strings only (the contract's types)
  v = ws(v);
  size_t n = 0;
  for (v++; *v && *v != '"' && n + 1 < cap; v++) out[n++] = *v;
  out[n] = 0;
}
static int count(const char* arr) {
  int n = 0;
  while (element(arr, n)) n++;
  return n;
}
static long num(const char* v) { return strtol(ws(v), nullptr, 10); }
}  // namespace jr

// FIPS 180-4 SHA-256, to hold the vendored contract to the sha256 below.
static void sha256Hex(const unsigned char* data, size_t len, char out[65]) {
  static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  const uint64_t bits = uint64_t(len) * 8;
  const size_t total = ((len + 9 + 63) / 64) * 64;
  for (size_t off = 0; off < total; off += 64) {
    unsigned char blk[64];
    for (size_t k = 0; k < 64; k++) {
      size_t i = off + k;
      blk[k] = i < len ? data[i] : (i == len ? 0x80 : 0);
      if (i >= total - 8) blk[k] = (unsigned char)(bits >> (8 * (total - 1 - i)));
    }
    uint32_t w[64];
    for (int t = 0; t < 16; t++)
      w[t] = (uint32_t(blk[4 * t]) << 24) | (uint32_t(blk[4 * t + 1]) << 16) | (uint32_t(blk[4 * t + 2]) << 8) | blk[4 * t + 3];
    for (int t = 16; t < 64; t++) {
      uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
      uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; t++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  for (int k = 0; k < 8; k++) snprintf(out + 8 * k, 9, "%08x", (unsigned)h[k]);
}

// test/fixtures/api-contract.v1.json is a byte copy of di.iiii
// serverXR/src/lighting/api-contract.json at commit 091a1a6 (PR #941, plan step D2):
//   sha256 b4365121c8914c3c9be311dbfbd9c0b0a3aef7816676835787e70dded7d7790d
// A di bump replaces the file, this hash and DESK_UI_CONTRACT_VERSION together.
static const char* const kContractSha256 = "b4365121c8914c3c9be311dbfbd9c0b0a3aef7816676835787e70dded7d7790d";

static char* readFile(const char* path, size_t* lenOut) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = (char*)malloc(size_t(n) + 1);
  if (buf && fread(buf, 1, size_t(n), f) != size_t(n)) { free(buf); buf = nullptr; }
  fclose(f);
  if (buf) buf[n] = 0;
  if (lenOut) *lenOut = size_t(n);
  return buf;
}

static char* readContract() {
  size_t n = 0;
  char* c = readFile("test/fixtures/api-contract.v1.json", &n);
  TEST_ASSERT_NOT_NULL_MESSAGE(c, "test/fixtures/api-contract.v1.json not readable from the project root");
  char hex[65];
  sha256Hex((const unsigned char*)c, n, hex);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(kContractSha256, hex, "vendored contract differs from di 091a1a6");
  TEST_ASSERT_TRUE(jr::valid(c));
  return c;
}

// A node at V6 scale: 32 fixtures (one of every profile, then rgb), 8 groups,
// 8 scene slots of which 6 saved, a name with every character that needs escaping.
struct BigNode {
  uint8_t web[512];
  uint8_t out[512];
  dk::NodeFixture fixtures[32];
  dk::GroupRange groups[8];
  uint8_t snaps[8][512];
  const uint8_t* scenePtr[8];
  const char* sceneNames[8];
  uint8_t chase[16];
  dk::NetIface ifaces[2];
  const char* bcast[2];
  const char* targets[1];
  dk::DeskFacts facts;
};
static BigNode gNode;
static const char* const kNodeFx[] = {"none", "strobe", "chase", "pulse", "sine", "sparkle", "comet", "bars", "glitch", "radar"};

static void buildBigNode(uint8_t fixtureSlots = 32) {
  BigNode& n = gNode;
  memset(&n, 0, sizeof(n));
  for (int c = 0; c < 512; c++) { n.web[c] = uint8_t(c * 7); n.out[c] = uint8_t(255 - c % 256); }
  uint16_t at = 1;
  for (uint8_t k = 0; k < 32; k++) {
    const char* prof = k < dk::kProfileCount ? dk::kProfiles[k].name : "rgb";
    char nm[16];
    snprintf(nm, sizeof(nm), "F%u", k + 1u);
    n.fixtures[k] = mkFixture(nm, at, prof, uint8_t(k * 8), uint8_t(255 - k * 8));
    at = uint16_t(at + dk::fixtureWidth(n.fixtures[k]));
  }
  strncpy(n.fixtures[3].name, "q\"b\\s\n\x02", sizeof(n.fixtures[3].name) - 1);
  for (uint8_t g = 0; g < 8; g++) {
    snprintf(n.groups[g].name, sizeof(n.groups[g].name), "G%u", g + 1u);
    n.groups[g].start = uint16_t(1 + g * 16);
    n.groups[g].end = uint16_t(16 + g * 16);
    n.groups[g].enabled = g != 7;
  }
  for (uint8_t s = 0; s < 8; s++) {
    for (int c = 0; c < 512; c++) n.snaps[s][c] = uint8_t(c + s * 31);
    n.scenePtr[s] = (s == 2 || s == 5) ? nullptr : n.snaps[s];
    n.sceneNames[s] = s == 1 ? "warm \"wash\"" : nullptr;
  }
  for (uint8_t k = 0; k < 16; k++) n.chase[k] = uint8_t(k % 8);
  n.ifaces[0] = {"ap", "10.0.0.1", "255.255.255.0"};
  n.ifaces[1] = {"sta", "192.168.1.20", "255.255.255.0"};
  n.bcast[0] = "10.0.0.255";
  n.bcast[1] = "192.168.1.255";
  n.targets[0] = "10.0.0.2";
  dk::DeskFacts& d = n.facts;
  d.web = n.web; d.dmxOut = n.out;
  d.fixtures = n.fixtures; d.fixtureSlots = fixtureSlots;
  d.groups = n.groups; d.groupSlots = 8;
  d.scenes = n.scenePtr; d.sceneNames = n.sceneNames; d.sceneSlots = 8; d.activeScene = 1; d.sceneFadeMs = 1000;
  d.master = 200; d.blackout = false;
  d.fxMode = "chase"; d.fxBpm = 128; d.fxDepth = 255; d.fxEnabled = true;
  d.fxModes = kNodeFx; d.fxModeCount = 10;
  d.chaseScenes = n.chase; d.chaseCount = 16; d.chaseRunning = true; d.chaseHoldMs = 2000; d.chaseFadeMs = 800;
  d.outputDriver = "artnet"; d.outputMode = "unicast"; d.outputTargets = n.targets; d.outputTargetCount = 1;
  d.outputEnabled = true; d.refreshHz = 40;
  d.interfaces = n.ifaces; d.interfaceCount = 2; d.broadcast = n.bcast; d.broadcastCount = 2;
  d.packetsSent = 123456; d.lastError = nullptr; d.nodeName = "node-1";
}

static size_t emit(dk::ReplyFn fn, const dk::DeskFacts& d) {
  static dk::JsonWriter w;
  logReset();
  w.begin(chunkSink, &gLog);
  TEST_ASSERT_TRUE(fn(w, d));
  TEST_ASSERT_TRUE_MESSAGE(gLog.maxChunk <= dk::kChunk, "a chunk passed kChunk");
  TEST_ASSERT_TRUE_MESSAGE(jr::valid(gLog.doc), gLog.doc);
  return gLog.len;
}

// Every state_keys.required and nested_required key, with the contract's type.
static void assertStateMatchesContract(const char* contract, const char* state) {
  const char* req = jr::at(contract, "state_keys.required");
  TEST_ASSERT_NOT_NULL(req);
  char key[64], want[64], msg[256];
  int checked = 0;
  for (int i = 0;; i++) {
    const char* tv = jr::nthMember(req, i, key, sizeof(key));
    if (!tv) break;
    jr::str(tv, want, sizeof(want));
    const char* v = jr::member(state, key);
    snprintf(msg, sizeof(msg), "api/state.%s missing or not %s", key, want);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, msg);
    TEST_ASSERT_TRUE_MESSAGE(jr::typeMatches(v, want), msg);
    checked++;
  }
  TEST_ASSERT_EQUAL_INT(13, checked);
  const char* nested = jr::at(contract, "state_keys.nested_required");
  TEST_ASSERT_NOT_NULL(nested);
  char parent[64];
  int parents = 0;
  for (int i = 0;; i++) {
    const char* obj = jr::nthMember(nested, i, parent, sizeof(parent));
    if (!obj) break;
    parents++;
    const char* pv = jr::member(state, parent);
    TEST_ASSERT_NOT_NULL_MESSAGE(pv, parent);
    for (int k = 0;; k++) {
      const char* tv = jr::nthMember(obj, k, key, sizeof(key));
      if (!tv) break;
      jr::str(tv, want, sizeof(want));
      const char* v = jr::member(pv, key);
      snprintf(msg, sizeof(msg), "api/state.%s.%s missing or not %s", parent, key, want);
      TEST_ASSERT_NOT_NULL_MESSAGE(v, msg);
      TEST_ASSERT_TRUE_MESSAGE(jr::typeMatches(v, want), msg);
      checked++;
    }
  }
  TEST_ASSERT_EQUAL_INT(4, parents);
  TEST_ASSERT_EQUAL_INT(13 + 7 + 5 + 4 + 4, checked);
}

// The contract entry for method+path, or nullptr.
static const char* contractEntry(const char* contract, const char* method, const char* path) {
  const char* entries = jr::at(contract, "entries");
  char m[16], p[64];
  for (int i = 0;; i++) {
    const char* e = jr::element(entries, i);
    if (!e) return nullptr;
    jr::str(jr::member(e, "method"), m, sizeof(m));
    jr::str(jr::member(e, "path"), p, sizeof(p));
    if (!strcmp(m, method) && !strcmp(p, path)) return e;
  }
}

// The entry's reply keys (and, for class b, its node body keys) with their types.
static void assertReplyMatchesContract(const char* contract, const char* path, const char* doc) {
  const char* e = contractEntry(contract, "GET", path);
  TEST_ASSERT_NOT_NULL_MESSAGE(e, path);
  const char* shapes[2] = {jr::member(e, "reply"), jr::at(e, "node.body")};
  char key[64], want[64], msg[256];
  for (const char* shape : shapes) {
    if (!shape) continue;
    for (int i = 0;; i++) {
      const char* tv = jr::nthMember(shape, i, key, sizeof(key));
      if (!tv) break;
      jr::str(tv, want, sizeof(want));
      if (!strcmp(key, "error")) continue;   // only on refused writes
      const char* v = jr::member(doc, key);
      snprintf(msg, sizeof(msg), "%s.%s missing or not %s", path, key, want);
      TEST_ASSERT_NOT_NULL_MESSAGE(v, msg);
      TEST_ASSERT_TRUE_MESSAGE(jr::typeMatches(v, want), msg);
    }
  }
}

static void test_desk_state_matches_vendored_contract() {
  char* contract = readContract();
  TEST_ASSERT_EQUAL_INT(DESK_UI_CONTRACT_VERSION, (int)jr::num(jr::at(contract, "contract_version")));
  buildBigNode();
  size_t bytes = emit(dk::writeState, gNode.facts);
  const char* s = gLog.doc;
  assertStateMatchesContract(contract, s);
  // The class-b keys of plan §2(b), as empty values of the right type.
  static const char* const kEmptyArrays[] = {"looks", "layers", "lfos", "sets", "status.nodes"};
  for (const char* k : kEmptyArrays) {
    const char* v = jr::at(s, k);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, k);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("array", jr::typeOf(v), k);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jr::count(v), k);
  }
  TEST_ASSERT_EQUAL_STRING_LEN("{}", jr::at(s, "raw"), 2);
  TEST_ASSERT_EQUAL_STRING("object", jr::typeOf(jr::at(s, "audioCfg")));
  TEST_ASSERT_EQUAL_STRING_LEN("false", jr::at(s, "audioCfg.enabled"), 5);
  TEST_ASSERT_EQUAL_STRING_LEN("null", jr::at(s, "status.serial"), 4);
  TEST_ASSERT_EQUAL_STRING_LEN("[0]", jr::at(s, "status.universes"), 3);
  TEST_ASSERT_EQUAL_STRING_LEN("false", jr::at(s, "caps.midi"), 5);
  // Content: 32 fixtures, 7 groups, 6 saved scenes, all 13 profiles, the node's own fx modes.
  TEST_ASSERT_EQUAL_INT(32, jr::count(jr::at(s, "fixtures")));
  TEST_ASSERT_EQUAL_INT(7, jr::count(jr::at(s, "groups")));
  TEST_ASSERT_EQUAL_INT(6, jr::count(jr::at(s, "scenes")));
  TEST_ASSERT_EQUAL_INT(10, jr::count(jr::at(s, "fxModes")));
  TEST_ASSERT_EQUAL_INT(16, jr::count(jr::at(s, "chase.sceneIds")));
  char v[64];
  jr::str(jr::at(s, "fixtures.3.id"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("f4", v);
  {
    const char* want = "\"q\\\"b\\\\s\\n\\u0002\"";
    TEST_ASSERT_EQUAL_STRING_LEN(want, jr::at(s, "fixtures.3.name"), strlen(want));
  }
  jr::str(jr::at(s, "fixtures.0.profile"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("dimmer", v);
  TEST_ASSERT_EQUAL_INT(0, jr::num(jr::at(s, "fixtures.12.universe")));
  jr::str(jr::at(s, "profiles.ppttdrgbw.label"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("ppttdrgbw (9 Channels)", v);
  jr::str(jr::at(s, "profiles.dimmer.label"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("dimmer (1 Channel)", v);
  jr::str(jr::at(s, "activeScene"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("s2", v);
  {
    const char* want = "\"warm \\\"wash\\\"\"";
    TEST_ASSERT_EQUAL_STRING_LEN(want, jr::at(s, "scenes.1.name"), strlen(want));
  }
  jr::str(jr::at(s, "scenes.2.id"), v, sizeof(v));  // slot 3 is empty: the third listed is s4
  TEST_ASSERT_EQUAL_STRING("s4", v);
  jr::str(jr::at(s, "scenes.2.name"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("Scene 4", v);
  // Group 1 covers channels 1..16: fixtures 1..5 end at 1,3,7,12,17 → f1 f2 f3 f4.
  TEST_ASSERT_EQUAL_INT(4, jr::count(jr::at(s, "groups.0.ids")));
  printf("desk: api/state at 32 fixtures, 6 scenes = %u bytes in %u chunks (max %u)\n", (unsigned)bytes,
         (unsigned)gLog.chunks, (unsigned)gLog.maxChunk);
  // Today's 12 slots, for the plan §3 comparison.
  buildBigNode(12);
  bytes = emit(dk::writeState, gNode.facts);
  assertStateMatchesContract(contract, gLog.doc);
  printf("desk: api/state at 12 fixtures, 6 scenes = %u bytes\n", (unsigned)bytes);
  // A zeroed DeskFacts (nothing patched, no pointers) still has the whole shape.
  dk::DeskFacts empty;
  memset(&empty, 0, sizeof(empty));
  emit(dk::writeState, empty);
  assertStateMatchesContract(contract, gLog.doc);
  TEST_ASSERT_EQUAL_INT(0, jr::count(jr::at(gLog.doc, "fixtures")));
  free(contract);
}

static void test_desk_fixture_and_scene_values_in_state() {
  buildBigNode();
  BigNode& n = gNode;
  // f5 is drgbw at channel 13 (1+1+2+4+5... see buildBigNode): read its address back first.
  emit(dk::writeState, n.facts);
  const char* f5 = jr::at(gLog.doc, "fixtures.4");
  long addr = jr::num(jr::member(f5, "address"));
  TEST_ASSERT_EQUAL_INT(n.fixtures[4].start, addr);
  char v[32];
  jr::str(jr::member(f5, "profile"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("drgbw", v);
  // Live values come from the web layer at address-1+offset, in profile order.
  const char* vals = jr::member(f5, "values");
  static const char* const kRoles5[] = {"dimmer", "r", "g", "b", "w"};
  for (int k = 0; k < 5; k++) {
    char key[16];
    TEST_ASSERT_NOT_NULL(jr::nthMember(vals, k, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING(kRoles5[k], key);
    TEST_ASSERT_EQUAL_INT(n.web[addr - 1 + k], jr::num(jr::member(vals, key)));
  }
  TEST_ASSERT_NULL(jr::nthMember(vals, 5, v, sizeof(v)));
  // Position: 0..255 on the node, 0..1 in di.
  TEST_ASSERT_EQUAL_STRING_LEN("0.125", jr::member(f5, "x"), 5);   // 32/255
  // Scene s1: the same fixture, values read from the snapshot, not the web layer.
  const char* sf = jr::at(gLog.doc, "scenes.0.fixtures.4");
  jr::str(jr::member(sf, "id"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("f5", v);
  for (int k = 0; k < 5; k++)
    TEST_ASSERT_EQUAL_INT(n.snaps[0][addr - 1 + k], jr::num(jr::member(jr::member(sf, "values"), kRoles5[k])));
  TEST_ASSERT_EQUAL_INT(32, jr::num(jr::at(gLog.doc, "scenes.0.live")));
  TEST_ASSERT_EQUAL_INT(0, jr::num(jr::at(gLog.doc, "scenes.0.missing")));
  TEST_ASSERT_EQUAL_INT(1000, jr::num(jr::at(gLog.doc, "scenes.0.fadeMs")));
  // A write through the profile shows up in the next read; the scene does not move.
  const dk::RoleValue rv[] = {{dk::Role::W, 7}};
  dk::writeValues(n.web, n.fixtures[4], rv, 1);
  emit(dk::writeState, n.facts);
  TEST_ASSERT_EQUAL_INT(7, jr::num(jr::at(gLog.doc, "fixtures.4.values.w")));
  TEST_ASSERT_EQUAL_INT(n.snaps[0][addr - 1 + 4], jr::num(jr::at(gLog.doc, "scenes.0.fixtures.4.values.w")));
  // A disabled slot is not listed anywhere.
  n.fixtures[0].enabled = false;
  emit(dk::writeState, n.facts);
  TEST_ASSERT_EQUAL_INT(31, jr::count(jr::at(gLog.doc, "fixtures")));
  TEST_ASSERT_EQUAL_INT(31, jr::count(jr::at(gLog.doc, "scenes.0.fixtures")));
  jr::str(jr::at(gLog.doc, "fixtures.0.id"), v, sizeof(v));
  TEST_ASSERT_EQUAL_STRING("f2", v);   // ids are slots, stable across a disable
}

static bool writeCuesFn(dk::JsonWriter& w, const dk::DeskFacts&) { return dk::writeCues(w); }
static bool writeLooksFn(dk::JsonWriter& w, const dk::DeskFacts&) { return dk::writeLooks(w); }

static void test_desk_small_replies_match_vendored_contract() {
  char* contract = readContract();
  buildBigNode();
  size_t bytes = emit(dk::writeDmx, gNode.facts);
  assertReplyMatchesContract(contract, "api/dmx", gLog.doc);
  TEST_ASSERT_EQUAL_INT(512, jr::count(jr::at(gLog.doc, "dmx.0")));
  TEST_ASSERT_EQUAL_INT(gNode.out[100], jr::num(jr::at(gLog.doc, "dmx.0.100")));
  printf("desk: api/dmx = %u bytes\n", (unsigned)bytes);
  gNode.facts.dmxOut = nullptr;   // falls back to the web layer
  emit(dk::writeDmx, gNode.facts);
  TEST_ASSERT_EQUAL_INT(gNode.web[100], jr::num(jr::at(gLog.doc, "dmx.0.100")));
  emit(writeCuesFn, gNode.facts);
  assertReplyMatchesContract(contract, "api/cues", gLog.doc);
  TEST_ASSERT_EQUAL_INT(0, jr::num(jr::at(gLog.doc, "cues.n")));
  emit(dk::writeShow, gNode.facts);
  assertReplyMatchesContract(contract, "api/show", gLog.doc);
  emit(writeLooksFn, gNode.facts);
  assertReplyMatchesContract(contract, "api/looks", gLog.doc);
  TEST_ASSERT_EQUAL_INT(0, jr::count(jr::at(gLog.doc, "looks")));
  free(contract);
}

// The pull adaptor rebuilds the same document for any pull size.
static void test_desk_window_pull_reassembles_state() {
  buildBigNode();
  emit(dk::writeState, gNode.facts);
  static char full[131072];
  const size_t fullLen = gLog.len;
  memcpy(full, gLog.doc, fullLen);
  static char again[131072];
  static dk::JsonWriter w;
  const size_t sizes[] = {1, 511, 512, 513, 1436, 5744};
  for (size_t sz : sizes) {
    if (sz == 1 && fullLen > 20000) {
      // one byte per pull is quadratic over the whole document; check a prefix and the tail only
      uint8_t b;
      for (size_t at = 0; at < 300; at++) {
        TEST_ASSERT_EQUAL_size_t(1, dk::pullWindow(dk::writeState, gNode.facts, w, &b, 1, at));
        TEST_ASSERT_EQUAL_CHAR(full[at], (char)b);
      }
      TEST_ASSERT_EQUAL_size_t(1, dk::pullWindow(dk::writeState, gNode.facts, w, &b, 1, fullLen - 1));
      TEST_ASSERT_EQUAL_CHAR('}', (char)b);
      TEST_ASSERT_EQUAL_size_t(0, dk::pullWindow(dk::writeState, gNode.facts, w, &b, 1, fullLen));
      continue;
    }
    size_t at = 0, pulls = 0;
    uint64_t generated = 0;
    for (;;) {
      size_t got = dk::pullWindow(dk::writeState, gNode.facts, w, (uint8_t*)again + at, sz, at);
      generated += w.total;
      pulls++;
      if (got == 0) break;
      TEST_ASSERT_TRUE(got <= sz);
      at += got;
      TEST_ASSERT_TRUE(at <= fullLen);
    }
    TEST_ASSERT_EQUAL_size_t(fullLen, at);
    TEST_ASSERT_EQUAL_INT(0, memcmp(full, again, fullLen));
    if (sz == 1436 || sz == 5744)
      printf("desk: pull %u B: %u pulls, %llu bytes formatted for a %u-byte reply (x%.1f)\n", (unsigned)sz,
             (unsigned)pulls, (unsigned long long)generated, (unsigned)fullLen, double(generated) / double(fullLen));
  }
}

static void test_desk_struct_sizes() {
  printf("desk: sizeof NodeFixture=%u GroupRange=%u Profile=%u kProfiles=%u RoleInfo=%u kRoles=%u "
         "JsonWriter=%u DeskFacts=%u WindowSink=%u (host, %u-bit pointers)\n",
         (unsigned)sizeof(dk::NodeFixture), (unsigned)sizeof(dk::GroupRange), (unsigned)sizeof(dk::Profile),
         (unsigned)sizeof(dk::kProfiles), (unsigned)sizeof(dk::RoleInfo), (unsigned)sizeof(dk::kRoles),
         (unsigned)sizeof(dk::JsonWriter), (unsigned)sizeof(dk::DeskFacts), (unsigned)sizeof(dk::WindowSink),
         (unsigned)(sizeof(void*) * 8));
  TEST_ASSERT_EQUAL_UINT(22, sizeof(dk::NodeFixture));
  TEST_ASSERT_EQUAL_UINT(22, sizeof(dk::GroupRange));
  TEST_ASSERT_TRUE(sizeof(dk::JsonWriter) <= dk::kChunk + 64);
  TEST_ASSERT_TRUE(32 * sizeof(dk::NodeFixture) <= 704);
}
#endif

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
  RUN_TEST(test_health_fields_key_list_and_values);
  RUN_TEST(test_health_fields_truncation_reported);
  RUN_TEST(test_desk_profiles_match_di_engine);
  RUN_TEST(test_desk_fixture_roles_round_trip_through_web_layer);
  RUN_TEST(test_desk_stage_position_round_trips_every_byte);
  RUN_TEST(test_desk_v1_fixture_migration);
  RUN_TEST(test_desk_writer_escapes_and_formats);
  RUN_TEST(test_desk_writer_chunk_bound_and_stop);
#if !defined(ARDUINO)
  RUN_TEST(test_desk_ui_header_matches_lock);
  RUN_TEST(test_desk_ui_table_has_exactly_the_locked_files);
  RUN_TEST(test_desk_state_matches_vendored_contract);
  RUN_TEST(test_desk_fixture_and_scene_values_in_state);
  RUN_TEST(test_desk_small_replies_match_vendored_contract);
  RUN_TEST(test_desk_window_pull_reassembles_state);
  RUN_TEST(test_desk_struct_sizes);
#endif
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
