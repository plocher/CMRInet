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
using CMRInet::examples::HostStatusPanel;
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
  // Sample at 1 s intervals with the cumulative poll count increasing
  // by 15 each second -> 15.0 c/s. The rate is computed from the delta
  // of the cumulative count over the delta of time, so it is correct at
  // any sampling cadence (the sketch samples at 150 ms, not per-poll).
  for (uint32_t t = 0; t < PollRate::kWindowMs; t += 1000) {
    r.sample(t, (t / 1000) * 15);
  }
  const float cps = r.cyclesPerSecondAt(PollRate::kWindowMs - 1);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 15.0f, cps);
}

static void test_poll_rate_zero_when_no_polls(void) {
  PollRate r;
  r.reset(0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.cyclesPerSecondAt(5000));
}

static void test_poll_rate_interval_is_inverse_of_rate(void) {
  PollRate r;
  r.reset(0);
  // 10 polls/sec -> 100 ms interval. Sample at 1 s intervals with
  // cumulative count increasing by 10 each second.
  for (uint32_t t = 0; t < PollRate::kWindowMs; t += 1000) {
    r.sample(t, (t / 1000) * 10);
  }
  const uint32_t interval = r.intervalMsAt(PollRate::kWindowMs - 1);
  TEST_ASSERT_EQUAL_UINT32(100, interval);
}

static void test_poll_rate_interval_zero_when_stalled(void) {
  PollRate r;
  r.reset(0);
  TEST_ASSERT_EQUAL_UINT32(0, r.intervalMsAt(5000));
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

// ------------------------------------------------------- HostStatusPanel

// The header alternates between cycles/sec and ms/cycle every 5 s.
static void test_panel_header_alternates_rate_then_interval(void) {
  HostStatusPanel p;
  p.reset();
  // Feed 100 polls across 10 s so both views have a real value.
  uint32_t polls = 0;
  for (uint32_t t = 0; t < 10000; t += 100) {
    ++polls;
    p.sample(t, polls, polls, nullptr, nullptr, 0);
  }
  char buf[16];
  // t=4999 is in the first 5 s window -> rate view (c/s)
  p.headerText(buf, sizeof(buf), 4999);
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "c/s") != nullptr,
                           "first 5 s window should show c/s");
  // t=5000 is in the second 5 s window -> interval view (ms)
  p.headerText(buf, sizeof(buf), 5000);
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "ms") != nullptr && strstr(buf, "c/s") == nullptr,
                           "second 5 s window should show ms, not c/s");
  // t=10000 wraps back to rate view
  p.headerText(buf, sizeof(buf), 10000);
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "c/s") != nullptr,
                           "third 5 s window should show c/s again");
}

static void test_panel_header_shows_stalled_when_no_polls(void) {
  HostStatusPanel p;
  p.reset();
  char buf[16];
  // No polls sampled; interval view (second 5 s window) should show the
  // stalled sentinel.
  p.headerText(buf, sizeof(buf), 5000);
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "---") != nullptr,
                           "stalled panel should show --- sentinel");
}

static void test_panel_node_row_online_omits_state_tag(void) {
  HostStatusPanel p;
  p.reset();
  uint32_t errs[] = {0};
  uint32_t misses[] = {0};
  p.sample(0, 0, 0, errs, misses, 1);
  char buf[24];
  // Online: no ON tag — counters are the proof the node is answering.
  p.nodeRowText(buf, sizeof(buf), 100, 0, 30, /*online=*/true, "ON ", 6);
  TEST_ASSERT_EQUAL_STRING("UA30    6ms   0m  0e", buf);
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "ON") == nullptr,
                           "online row must not carry a redundant ON tag");
}

static void test_panel_node_row_offline_keeps_tag_and_misses(void) {
  HostStatusPanel p;
  p.reset();
  uint32_t errs[] = {0};
  uint32_t misses[] = {0};
  p.sample(0, 0, 0, errs, misses, 1);
  // Two miss events in the window.
  misses[0] = 2;
  p.sample(100, 10, 8, errs, misses, 1);
  char buf[24];
  p.nodeRowText(buf, sizeof(buf), 100, 0, 31, /*online=*/false, "OFF", 273);
  // Offline keeps the compact tag; turnaround is blanked.
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "UA31") != nullptr, "UA missing");
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "OFF") != nullptr, "offline tag missing");
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "2m") != nullptr || strstr(buf, "  2m") != nullptr,
                           "miss count missing");
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "0e") != nullptr, "error count missing");
}

static void test_panel_host_totals_show_poll_reply_miss(void) {
  HostStatusPanel p;
  p.reset();
  uint32_t errs[] = {0};
  uint32_t misses[] = {0};
  p.sample(0, 0, 0, errs, misses, 1);
  misses[0] = 3;
  p.sample(150, 40, 37, errs, misses, 1);
  char buf[24];
  p.hostTotalsText(buf, sizeof(buf), 150);
  // Inter-sample deltas: P +40, R +37, m = rolling misses (3).
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "P:40") != nullptr, "poll delta missing");
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "R:37") != nullptr, "reply delta missing");
  TEST_ASSERT_TRUE_MESSAGE(strstr(buf, "m:3") != nullptr, "miss total missing");
}

// ------------------------------------------------------------------- runner

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_error_window_counts_only_recent_errors);
  RUN_TEST(test_error_window_returns_zero_when_quiet);
  RUN_TEST(test_poll_rate_computes_cycles_per_second);
  RUN_TEST(test_poll_rate_zero_when_no_polls);
  RUN_TEST(test_poll_rate_interval_is_inverse_of_rate);
  RUN_TEST(test_poll_rate_interval_zero_when_stalled);
  RUN_TEST(test_latency_text_fixed_width_right_justified);
  RUN_TEST(test_panel_header_alternates_rate_then_interval);
  RUN_TEST(test_panel_header_shows_stalled_when_no_polls);
  RUN_TEST(test_panel_node_row_online_omits_state_tag);
  RUN_TEST(test_panel_node_row_offline_keeps_tag_and_misses);
  RUN_TEST(test_panel_host_totals_show_poll_reply_miss);
  return UNITY_END();
}
