// test_time.cpp — tests for the injected-time helpers in CMRITime.h.
//
// The mock clock makes the 25-day and 50-day boundaries testable in
// microseconds: these tests cross uint32_t wrap points that real-time
// tests can never reach.

#include "CMRITime.h"
#include "unity.h"

using CMRInet::Age;
using CMRInet::Deadline;
using CMRInet::timeReached;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------ timeReached

static void test_time_reached_basic(void) {
  TEST_ASSERT_TRUE(timeReached(5, 5));
  TEST_ASSERT_TRUE(timeReached(6, 5));
  TEST_ASSERT_FALSE(timeReached(4, 5));
}

// Wrap-safe: a deadline just past rollover is reached by a clock that
// has wrapped, and is not reached by the pre-wrap clock.
static void test_time_reached_across_wrap(void) {
  const uint32_t beforeWrap = 0xFFFFFFF0u;
  const uint32_t afterWrap = 0x00000010u;
  TEST_ASSERT_TRUE(timeReached(afterWrap, beforeWrap));
  TEST_ASSERT_FALSE(timeReached(beforeWrap, afterWrap));
}

// --------------------------------------------------------------- Deadline

static void test_deadline_default_disarmed(void) {
  Deadline d;
  TEST_ASSERT_FALSE(d.armed());
  TEST_ASSERT_FALSE(d.due(0));
  TEST_ASSERT_FALSE(d.due(0xFFFFFFFFu));
}

static void test_deadline_fires_at_due_time(void) {
  Deadline d;
  d.armAt(100);
  TEST_ASSERT_TRUE(d.armed());
  TEST_ASSERT_FALSE(d.due(99));
  TEST_ASSERT_TRUE(d.due(100));
  TEST_ASSERT_TRUE(d.armed());  // due does not disarm
}

static void test_deadline_arm_in_is_relative(void) {
  Deadline d;
  d.armIn(1000, 250);
  TEST_ASSERT_FALSE(d.due(1249));
  TEST_ASSERT_TRUE(d.due(1250));
}

// The fail-safe latch: once observed due, the deadline stays due even
// at clock values whose raw wrap math would read "not yet". A missed
// window fires late, never never.
static void test_deadline_due_is_sticky_across_wrap(void) {
  Deadline d;
  d.armAt(100);
  TEST_ASSERT_TRUE(d.due(100));
  // 2^31 ms later the raw signed comparison flips negative.
  TEST_ASSERT_FALSE(timeReached(100 + 0x80000000u, 100));
  TEST_ASSERT_TRUE(d.due(100 + 0x80000000u));
}

static void test_deadline_disarm_clears(void) {
  Deadline d;
  d.armAt(100);
  TEST_ASSERT_TRUE(d.due(100));
  d.disarm();
  TEST_ASSERT_FALSE(d.armed());
  TEST_ASSERT_FALSE(d.due(200));
}

static void test_deadline_rearm_resets_latch(void) {
  Deadline d;
  d.armAt(100);
  TEST_ASSERT_TRUE(d.due(100));
  d.armAt(200);
  TEST_ASSERT_FALSE(d.due(150));
  TEST_ASSERT_TRUE(d.due(200));
}

// -------------------------------------------------------------------- Age

static void test_age_never_marked_is_named_sentinel(void) {
  Age a;
  TEST_ASSERT_FALSE(a.marked());
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, a.ms(0));
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, a.ms(123456));
  // No data is stale data: every threshold check passes.
  TEST_ASSERT_TRUE(a.atLeast(0, 250));
  TEST_ASSERT_FALSE(a.exceeded());
}

static void test_age_measures_from_mark(void) {
  Age a;
  a.mark(1000);
  TEST_ASSERT_TRUE(a.marked());
  TEST_ASSERT_EQUAL_UINT32(0, a.ms(1000));
  TEST_ASSERT_EQUAL_UINT32(250, a.ms(1250));
  TEST_ASSERT_TRUE(a.atLeast(1250, 250));
  TEST_ASSERT_FALSE(a.atLeast(1250, 251));
}

static void test_age_clear_returns_to_never_marked(void) {
  Age a;
  a.mark(1000);
  a.clear();
  TEST_ASSERT_FALSE(a.marked());
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, a.ms(1001));
  TEST_ASSERT_TRUE(a.atLeast(1001, 1));
}

// Within the first window the boundary is self-detected, poll or not.
static void test_age_saturates_at_capacity(void) {
  Age a;
  a.mark(0);
  TEST_ASSERT_EQUAL_UINT32(Age::kCapacityMs, a.ms(Age::kCapacityMs));
  TEST_ASSERT_EQUAL_UINT32(Age::kExceededCapacity, a.ms(Age::kCapacityMs + 1));
  TEST_ASSERT_TRUE(a.atLeast(Age::kCapacityMs + 1, 0xF0000000u));
}

// The poll latch survives full wrap: without it, an age of 2^32 + 10 ms
// aliases to a fresh-looking 10 ms.
static void test_age_poll_latches_across_wrap(void) {
  Age a;
  a.mark(0);
  a.poll(0x80000001u);  // observed past capacity: latch
  TEST_ASSERT_TRUE(a.exceeded());
  // The clock has wrapped; raw subtraction would report 10 ms.
  TEST_ASSERT_EQUAL_UINT32(Age::kExceededCapacity, a.ms(10));
  TEST_ASSERT_TRUE(a.atLeast(10, 0xF0000000u));
}

static void test_age_remark_clears_saturation(void) {
  Age a;
  a.mark(0);
  a.poll(0x80000001u);
  TEST_ASSERT_TRUE(a.exceeded());
  a.mark(20);
  TEST_ASSERT_FALSE(a.exceeded());
  TEST_ASSERT_EQUAL_UINT32(10, a.ms(30));
}

// ------------------------------------------------------------------- main

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_time_reached_basic);
  RUN_TEST(test_time_reached_across_wrap);
  RUN_TEST(test_deadline_default_disarmed);
  RUN_TEST(test_deadline_fires_at_due_time);
  RUN_TEST(test_deadline_arm_in_is_relative);
  RUN_TEST(test_deadline_due_is_sticky_across_wrap);
  RUN_TEST(test_deadline_disarm_clears);
  RUN_TEST(test_deadline_rearm_resets_latch);
  RUN_TEST(test_age_never_marked_is_named_sentinel);
  RUN_TEST(test_age_measures_from_mark);
  RUN_TEST(test_age_clear_returns_to_never_marked);
  RUN_TEST(test_age_saturates_at_capacity);
  RUN_TEST(test_age_poll_latches_across_wrap);
  RUN_TEST(test_age_remark_clears_saturation);
  return UNITY_END();
}
