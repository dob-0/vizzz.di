#include <unity.h>

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

void test_sta_retry_backoff() {
  TEST_ASSERT_EQUAL_UINT32(0, vizzz::staRetryDelayMs(0));
  TEST_ASSERT_EQUAL_UINT32(30000, vizzz::staRetryDelayMs(1));
  TEST_ASSERT_EQUAL_UINT32(60000, vizzz::staRetryDelayMs(2));
  TEST_ASSERT_EQUAL_UINT32(120000, vizzz::staRetryDelayMs(3));
  TEST_ASSERT_EQUAL_UINT32(240000, vizzz::staRetryDelayMs(4));
  TEST_ASSERT_EQUAL_UINT32(300000, vizzz::staRetryDelayMs(5));
  TEST_ASSERT_EQUAL_UINT32(300000, vizzz::staRetryDelayMs(255));
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
  RUN_TEST(test_sta_retry_backoff);
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
