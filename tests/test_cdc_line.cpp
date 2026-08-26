// test_cdc_line.cpp — tests for the shared CDC line writer (#99): the
// chunked body write, the per-call room check, and the split time
// budget that reserves the terminator's slice.
//
// The console is a FakeCdcConsole: a capture buffer, a clock that
// advances on yieldMs(), and programmable room and accept schedules.
// Every assertion runs on the injected clock; no test sleeps.
//
// VALIDATION: #99 Done-when — the chunked-write / room-check /
// reserved-terminator-budget logic is exercised on the desktop,
// including the full-buffer case the #86 defect dropped.

#include <stdint.h>
#include <string.h>

#include <functional>
#include <string>

#include "testbed/CdcLineWriter.h"
#include "unity.h"

using CMRInet::testbed::CdcConsole;
using CMRInet::testbed::writeCdcLine;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------- fake console

/// CdcConsole double: a capture buffer, a clock that advances on
/// yieldMs(), and programmable room and accept schedules. Inert — no
/// unity asserts inside; it is infrastructure.
class FakeCdcConsole : public CdcConsole {
 public:
  bool open_ = true;
  uint32_t now_ = 0;
  uint32_t yieldAdvanceMs_ = 1;
  size_t room_ = 1024;
  std::function<size_t()> roomFn_;
  size_t acceptMax_ = SIZE_MAX;
  std::function<size_t(size_t)> acceptFn_;

  std::string capture;
  size_t writeCalls = 0;

  bool open() const override { return open_; }
  size_t availableForWrite() override { return roomFn_ ? roomFn_() : room_; }
  uint32_t nowMs() override { return now_; }
  void yieldMs() override { now_ += yieldAdvanceMs_; }
  size_t write(const uint8_t* data, size_t n) override {
    ++writeCalls;
    size_t acc = acceptFn_ ? acceptFn_(n) : (n < acceptMax_ ? n : acceptMax_);
    if (acc > n) acc = n;
    capture.append(reinterpret_cast<const char*>(data), acc);
    return acc;
  }
};

// ------------------------------------------------------------------- tests

// (a) happy path: body and terminator both land, one write each.
static void test_happy_path_writes_body_and_newline(void) {
  FakeCdcConsole c;
  const size_t n = writeCdcLine(c, "hello");
  TEST_ASSERT_EQUAL_size_t(6, n);  // 5 body + 1 newline
  TEST_ASSERT_EQUAL_STRING("hello\n", c.capture.c_str());
  TEST_ASSERT_EQUAL_size_t(2, c.writeCalls);
}

// (b) chunked body: the body is larger than room, so it splits across
// several write calls; the terminator still lands.
static void test_chunked_body_splits_across_writes(void) {
  FakeCdcConsole c;
  c.room_ = 4;
  const size_t n = writeCdcLine(c, "ABCDEFGH");  // 8 bytes
  TEST_ASSERT_EQUAL_size_t(9, n);  // 8 body + 1 newline
  TEST_ASSERT_EQUAL_STRING("ABCDEFGH\n", c.capture.c_str());
  TEST_ASSERT_GREATER_THAN_size_t(2, c.writeCalls);  // body took >1 write
}

// (c) the #86 case: the buffer is full (room == 0) through the whole
// body budget, then opens for the terminator. The terminator must still
// land — the room check and the reserved slice are exactly the fix.
static void test_full_buffer_terminator_still_lands(void) {
  FakeCdcConsole c;
  c.roomFn_ = [&c]() { return c.now_ >= 200 ? 1 : 0; };
  const size_t n = writeCdcLine(c, "body");
  TEST_ASSERT_EQUAL_size_t(1, n);  // body wrote nothing; terminator landed
  TEST_ASSERT_EQUAL_STRING("\n", c.capture.c_str());
  TEST_ASSERT_EQUAL_size_t(1, c.writeCalls);  // only the terminator write
}

// (d) a closed stream writes nothing.
static void test_closed_stream_writes_nothing(void) {
  FakeCdcConsole c;
  c.open_ = false;
  const size_t n = writeCdcLine(c, "hello");
  TEST_ASSERT_EQUAL_size_t(0, n);
  TEST_ASSERT_TRUE(c.capture.empty());
  TEST_ASSERT_EQUAL_size_t(0, c.writeCalls);
}

// (e) the body budget is exhausted before the body completes (room is
// available but the stream accepts nothing), so the body is truncated —
// yet the reserved terminator slice still runs, and with the stream now
// accepting the terminator lands.
static void test_body_budget_exhausted_terminator_reserved(void) {
  FakeCdcConsole c;
  c.acceptFn_ = [&c](size_t n) { return c.now_ >= 200 ? n : 0; };
  const size_t n = writeCdcLine(c, "ABCDEFGH");
  TEST_ASSERT_EQUAL_size_t(1, n);  // body truncated to 0; terminator landed
  TEST_ASSERT_EQUAL_STRING("\n", c.capture.c_str());
  // The body retried through its whole 200 ms budget before giving up.
  TEST_ASSERT_GREATER_THAN_size_t(200, c.writeCalls);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_happy_path_writes_body_and_newline);
  RUN_TEST(test_chunked_body_splits_across_writes);
  RUN_TEST(test_full_buffer_terminator_still_lands);
  RUN_TEST(test_closed_stream_writes_nothing);
  RUN_TEST(test_body_budget_exhausted_terminator_reserved);
  return UNITY_END();
}
