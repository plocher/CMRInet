// test_host_display_metrics.cpp — tests for the pure display-metrics
// helpers the SimpleHost OLED uses: rolling error count over a window,
// polling rate over a window, and the fixed-width latency text. These are
// deliberately factored as plain free functions (not tied to the display
// or the host) so they are verifiable under a deterministic mock clock.
//
// The helpers live in the shared display-metrics header the example sketch
// uses; the desktop test links them directly with no Arduino dependency.

#include <string.h>

#include "unity.h"
#include "SimpleHostMetrics.h"

void setUp(void) {}
void tearDown(void) {}

using CMRInet::examples::ErrorWindow;
using CMRInet::examples::PollRate;
using CMRInet::examples::latencyText;

// ------------------------------------------------------- rolling error count

static void test_error_window_counts_only_recent_errors(void) {
  ErrorWindow w;
  w.reset();
  // t=0,1,2 : three errors land
  w.onEvent(0);
  w.onEvent(1000);
  w.onEvent(2000);
  // t=3000 : query with a 2000 ms window — the t=0 error has expired,
  // the t=1000/t=2000 ones are still in the window.
  TEST_ASSERT_EQUAL_UINT32(2, w.countInLastMs(3000, 2000));
}

static void test_error_window_returns_zero_when_quiet(void) {
  ErrorWindow w;
  w.reset();
  w.onEvent(0);
  // Far past the window.
  TEST_ASSERT_EQUAL_UINT32(0, w.countInLastMs(10000, 2000));
}

// ----------------------------------------------------------- polling rate

static void test_poll_rate_computes_cycles_per_second(void) {
  PollRate r;
  r.reset(0);
  // Spread polls across the rate window at roughly 15/sec. The window is
  // tens of seconds long (see kWindowMs), so the rate is smooth rather than
  // flickering on every redraw; assert within a tolerance that absorbs the
  // window-boundary effect.
  for (uint32_t t = 10; t < PollRate::kWindowMs; t += 66) {
    r.onPoll(t);
  }
  const float cps = r.cyclesPerSecondAt(PollRate::kWindowMs + 10);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 15.0f, cps);
}

static void test_poll_rate_zero_when_no_polls(void) {
  PollRate r;
  r.reset(0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.cyclesPerSecondAt(5000));
}

// ----------------------------------------------------------- latency text

static void test_latency_text_fixed_width_right_justified(void) {
  char buf[16];
  latencyText(buf, sizeof(buf), 6);     // "   6ms"
  TEST_ASSERT_EQUAL_STRING("   6ms", buf);
  latencyText(buf, sizeof(buf), 273);   // " 273ms"
  TEST_ASSERT_EQUAL_STRING(" 273ms", buf);
  latencyText(buf, sizeof(buf), 1200);  // "1200ms"
  TEST_ASSERT_EQUAL_STRING("1200ms", buf);
}

// ------------------------------------------------------------------- runner

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_error_window_counts_only_recent_errors);
  RUN_TEST(test_error_window_returns_zero_when_quiet);
  RUN_TEST(test_poll_rate_computes_cycles_per_second);
  RUN_TEST(test_poll_rate_zero_when_no_polls);
  RUN_TEST(test_latency_text_fixed_width_right_justified);
  return UNITY_END();
}
