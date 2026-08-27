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
void test_slowwalker_optional_ua() {
  char args[] = "ua 31 period 750 byte 2";
  ParsedGeneratorParams p = parseGeneratorParams(args, "slowwalker");
  TEST_ASSERT_NULL(p.error_code);
  TEST_ASSERT_TRUE(p.has_UA);
  TEST_ASSERT_EQUAL_UINT8(31, p.UA);
  TEST_ASSERT_TRUE(p.has_period);
  TEST_ASSERT_EQUAL_UINT32(750, p.period_ms);
  TEST_ASSERT_TRUE(p.has_byte);
  TEST_ASSERT_EQUAL_UINT8(2, p.byte_idx);
}

void test_loopback_src_dst_mode() {
  char args[] = "ua 31 src_byte 2 src_bit 1 dst_byte 2 dst_bit 1 mode write_read";
  ParsedGeneratorParams p = parseGeneratorParams(args, "toggleoutfrominput");
  TEST_ASSERT_NULL(p.error_code);
  TEST_ASSERT_TRUE(p.has_UA);
  TEST_ASSERT_EQUAL_UINT8(31, p.UA);
  TEST_ASSERT_TRUE(p.has_src_byte);
  TEST_ASSERT_TRUE(p.has_src_bit);
  TEST_ASSERT_EQUAL_UINT8(2, p.src_byte);
  TEST_ASSERT_EQUAL_UINT8(1, p.src_bit);
  TEST_ASSERT_TRUE(p.has_dst_byte);
  TEST_ASSERT_TRUE(p.has_dst_bit);
  TEST_ASSERT_EQUAL_UINT8(2, p.dst_byte);
  TEST_ASSERT_EQUAL_UINT8(1, p.dst_bit);
  TEST_ASSERT_TRUE(p.has_loopback_mode);
  TEST_ASSERT_TRUE(p.loopback_mode_write_read);
}

// Keys and values are case-insensitive: a user typing "UA 31 PERIOD 750"
// should parse the same as "ua 31 period 750".
void test_keys_are_case_insensitive() {
  char args[] = "UA 31 PERIOD 750 BYTE 2";
  ParsedGeneratorParams p = parseGeneratorParams(args, "slowwalker");
  TEST_ASSERT_NULL(p.error_code);
  TEST_ASSERT_TRUE(p.has_UA);
  TEST_ASSERT_EQUAL_UINT8(31, p.UA);
  TEST_ASSERT_TRUE(p.has_period);
  TEST_ASSERT_EQUAL_UINT32(750, p.period_ms);
  TEST_ASSERT_TRUE(p.has_byte);
  TEST_ASSERT_EQUAL_UINT8(2, p.byte_idx);
}

// Mode values are also case-insensitive.
void test_mode_values_are_case_insensitive() {
  char args[] = "ua 31 src_byte 2 src_bit 1 dst_byte 2 dst_bit 1 MODE WRITE_READ";
  ParsedGeneratorParams p = parseGeneratorParams(args, "toggleoutfrominput");
  TEST_ASSERT_NULL(p.error_code);
  TEST_ASSERT_TRUE(p.has_loopback_mode);
  TEST_ASSERT_TRUE(p.loopback_mode_write_read);
}

void test_loopback_missing_dst_bit_rejected() {
  char args[] = "src_byte 2 src_bit 1 dst_byte 2";
  ParsedGeneratorParams p = parseGeneratorParams(args, "toggleoutfrominput");
  TEST_ASSERT_NOT_NULL(p.error_code);
  TEST_ASSERT_EQUAL_STRING("missingValue", p.error_code);
  TEST_ASSERT_EQUAL_STRING("dst_bit", p.error_val);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_fastwalker_period);
  RUN_TEST(test_stall_positional_and_keys);
  RUN_TEST(test_unknown_key_rejected);
  RUN_TEST(test_missing_value_rejected);
  RUN_TEST(test_slowwalker_optional_ua);
  RUN_TEST(test_keys_are_case_insensitive);
  RUN_TEST(test_mode_values_are_case_insensitive);
  RUN_TEST(test_loopback_src_dst_mode);
  RUN_TEST(test_loopback_missing_dst_bit_rejected);
  return UNITY_END();
}
