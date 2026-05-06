#include <unity.h>

#include "vidili_core.h"

void test_pack_universe_uses_15bit_layout() {
  TEST_ASSERT_EQUAL_UINT16(0x123, vidili::packUniverse(0x01, 0x02, 0x03));
  TEST_ASSERT_EQUAL_UINT16(0x7ff, vidili::packUniverse(0x07, 0x0f, 0x0f));
}

void test_apply_master_scales_levels() {
  TEST_ASSERT_EQUAL_UINT8(255, vidili::applyMaster(255, 255));
  TEST_ASSERT_EQUAL_UINT8(0, vidili::applyMaster(200, 0));
  TEST_ASSERT_EQUAL_UINT8(127, vidili::applyMaster(255, 128));
  TEST_ASSERT_EQUAL_UINT8(50, vidili::applyMaster(100, 128));
}

#if defined(ARDUINO)
void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_pack_universe_uses_15bit_layout);
  RUN_TEST(test_apply_master_scales_levels);
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
  return UNITY_END();
}
#endif