#include <string.h>
#include <string>

#include "unity.h"
// Use relative path since it's an example sketch
#include "../examples/XiaoHostTracer/GeneratorParser.h"

void setUp(void) {}
void tearDown(void) {}

void test_fastwalker_period() {
  char args[] = "period 500";
  ParsedGeneratorParams p = parseGeneratorParams(args, "fastwalker");
  TEST_ASSERT_NULL(p.error_code);
  TEST_ASSERT_TRUE(p.has_period);
  TEST_ASSERT_EQUAL_UINT32(500, p.period_ms);
  TEST_ASSERT_FALSE(p.has_byte);
}

void test_stall_positional_and_keys() {
  char args[] = "30 period 2000 mode busy";
  ParsedGeneratorParams p = parseGeneratorParams(args, "stall");
  TEST_ASSERT_NULL(p.error_code);
  TEST_ASSERT_TRUE(p.has_stall_ms);
  TEST_ASSERT_EQUAL_UINT32(30, p.stall_ms);
  TEST_ASSERT_TRUE(p.has_period);
  TEST_ASSERT_EQUAL_UINT32(2000, p.period_ms);
  TEST_ASSERT_TRUE(p.has_mode);
  TEST_ASSERT_TRUE(p.mode_busy);
}

void test_unknown_key_rejected() {
  char args[] = "unknown 123";
  ParsedGeneratorParams p = parseGeneratorParams(args, "fastwalker");
  TEST_ASSERT_NOT_NULL(p.error_code);
  TEST_ASSERT_EQUAL_STRING("unknownKey", p.error_code);
  TEST_ASSERT_EQUAL_STRING("unknown", p.error_val);
}

void test_missing_value_rejected() {
  char args[] = "period";
  ParsedGeneratorParams p = parseGeneratorParams(args, "slowwalker");
  TEST_ASSERT_NOT_NULL(p.error_code);
  TEST_ASSERT_EQUAL_STRING("missingValue", p.error_code);
  TEST_ASSERT_EQUAL_STRING("period", p.error_val);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fastwalker_period);
  RUN_TEST(test_stall_positional_and_keys);
  RUN_TEST(test_unknown_key_rejected);
  RUN_TEST(test_missing_value_rejected);
  return UNITY_END();
}
