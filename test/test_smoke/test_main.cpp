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

#if defined(ARDUINO)
void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_pack_universe_uses_15bit_layout);
  RUN_TEST(test_apply_master_scales_levels);
  RUN_TEST(test_clamp_level_applies_ceiling);
  RUN_TEST(test_slew_toward_limits_step_size);
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
  UNITY_END();
}

void loop() {}
#else
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_pack_universe_uses_15bit_layout);
  RUN_TEST(test_apply_master_scales_levels);
  RUN_TEST(test_clamp_level_applies_ceiling);
  RUN_TEST(test_slew_toward_limits_step_size);
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
  return UNITY_END();
}
#endif