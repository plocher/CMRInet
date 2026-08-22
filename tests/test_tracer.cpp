// test_tracer.cpp — tests for the shared TracerShell: the I/T
// packet trace telemetry (D7 onTrace), the output verbs that make T
// bench-exercisable, the outputs hex field, and error lines.
//
// The engine is driven against a minimal FakePort + SerialCMRITransport
// + CMRIHost (address 5, 2 input / 3 output bytes). The fake port's
// character time is 1 us so wire-time drains in ~1 ms and the I->T->P
// schedule advances in single-millisecond ticks. Every assertion runs
// on the injected clock; no test sleeps. JSON lines are captured through
// the engine's LineWriter callback and asserted by substring.
//
// VALIDATION: map issue #30 scope — onTrace I/T visibility and output
// verbs are the AFK software slice the bench validation depends on.

#include <string.h>

#include <string>
#include <vector>

#include "CMRInet.h"
#include "testbed/TracerShell.h"
#include "unity.h"

using CMRInet::CMRIHost;
using CMRInet::CMRIHostConfig;
using CMRInet::CMRIPacket;
using CMRInet::CMRISerialPort;
using CMRInet::encodeFrame;
using CMRInet::kUaOffset;
using CMRInet::RemoteNodeConfig;
using CMRInet::RemoteNodeHandle;
using CMRInet::SerialCMRITransport;
using CMRInet::testbed::TracerShell;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------- minimal port

/// Byte-port double with an RX queue and a TX capture. Character time is
/// 1 us so SerialCMRITransport's wire-time floor drains in ~1 ms; the
/// port always reports drained so sendComplete() advances on the next
/// tick. No unity asserts inside — it is inert infrastructure.
class FakePort : public CMRISerialPort {
 public:
  void begin() override {}
  int readByte() override {
    if (rxHead_ >= rxCount_) return -1;
    return rx_[rxHead_++];
  }
  size_t writeBytes(const uint8_t* b, size_t n) override {
    size_t accepted = n;
    if (txCount_ + accepted > sizeof(tx_)) accepted = sizeof(tx_) - txCount_;
    if (accepted != 0) {
      memcpy(tx_ + txCount_, b, accepted);
      txCount_ += accepted;
    }
    return accepted;
  }
  bool transmitDrained() const override { return true; }
  void setTransmitEnable(bool e) override { txen_ = e; }
  uint32_t byteDurationMicros() const override { return 1; }
  uint32_t hardwareErrorCount() const override { return 0; }

  void queueRx(const uint8_t* b, size_t n) {
    memcpy(rx_ + rxCount_, b, n);
    rxCount_ += n;
  }
  bool txen() const { return txen_; }

 private:
  uint8_t rx_[256] = {0};
  size_t rxHead_ = 0;
  size_t rxCount_ = 0;
  uint8_t tx_[256] = {0};
  size_t txCount_ = 0;
  bool txen_ = false;
};

// ------------------------------------------------------------------- rig

/// One shell + host + transport + node, capturing every telemetry line.
struct TracerRig {
  FakePort port;
  SerialCMRITransport transport;
  CMRIHost host;
  RemoteNodeHandle* node = nullptr;
  TracerShell shell;
  std::vector<std::string> lines;

  explicit TracerRig(uint16_t inBytes = 2, uint16_t outBytes = 3)
      : transport(port), host(transport, fastConfig()) {
    host.addRemoteNode(5, inBytes, outBytes);
    node = host.node(5);
    TEST_ASSERT_NOT_NULL_MESSAGE(node, "addRemoteNode failed in rig");
    shell.bind(host, transport, *node, "test", "0.0", &TracerRig::writeLine_,
              this);
    host.begin();
  }

  /// Tick one millisecond: refresh the shell clock, then drive the host.
  void tick(uint32_t t) {
    shell.setNow(t);
    host.tick(t);
  }

  void run(uint32_t fromMs, uint32_t toMs) {
    for (uint32_t t = fromMs; t <= toMs; ++t) tick(t);
  }

  /// Dispatch a verb. Runs at the shell's current clock (the value
  /// the last tick handed it); for content assertions ts is irrelevant.
  TracerShell::VerbResult verb(const char* v) {
    return shell.handleVerb(v);
  }

 private:
  static CMRIHostConfig fastConfig() {
    CMRIHostConfig c;
    c.postInitSettleMs = 0;  // I -> T with no settle delay
    c.postTxGapMs = 0;       // T -> idle with no gap
    c.pollPacingMs = 0;      // back-to-back exchanges
    c.replyTimeoutMs = 1;    // a poll times out one tick after send
    return c;
  }
  static void writeLine_(void* ctx, const char* line) {
    TracerRig& self = *static_cast<TracerRig*>(ctx);
    self.lines.push_back(std::string(line));
  }
};

// ------------------------------------------------------------- line helpers

static bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

static int countContaining(const std::vector<std::string>& lines,
                           const char* needle) {
  int c = 0;
  for (const auto& s : lines) {
    if (contains(s, needle)) ++c;
  }
  return c;
}

static const std::string* findContaining(
    const std::vector<std::string>& lines, const char* needle) {
  for (const auto& s : lines) {
    if (contains(s, needle)) return &s;
  }
  return nullptr;
}

// CPNODE 'C' I-body for 2 input / 3 output bytes (interop E3):
// 'C' dH dL opts1 opts2 NI NO 0xFF x6 -> 43 00 00 00 00 02 03 FF FF FF FF FF FF
static const char* kIBodyHex = "43000000000203FFFFFFFFFFFF";

// ----------------------------------------------- onTrace I/T/P/R visibility

// The first exchange sends I; the engine must emit a trace line with
// dir=tx, mt=I, and the CPNODE 'C' body (acceptance #2).
static void test_trace_emits_tx_init_with_C_body(void) {
  TracerRig rig;
  rig.tick(0);
  const std::string* line = findContaining(rig.lines, "\"event\":\"trace\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no trace line emitted for I");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"dir\":\"tx\""), "I trace not tx");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"mt\":\"I\""), "I trace wrong mt");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"body\":\""),
                           "I trace missing body");
  char expect[64];
  snprintf(expect, sizeof(expect), "\"body\":\"%s\"", kIBodyHex);
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, expect),
                           "I trace body does not match CPNODE C dialect");
}

// I is followed by a full-image T (interop 2.3.1); the trace line must
// carry the configured output image (acceptance #3).
static void test_trace_emits_tx_transmit_with_output_image(void) {
  TracerRig rig;
  const uint8_t image[] = {0x01, 0x02, 0xA0};
  TEST_ASSERT_TRUE(rig.node->setOutputs(image, 3));
  rig.run(0, 3);  // I@0; the wait->idle step costs a tick, so T sends @3
  const std::string* line =
      findContaining(rig.lines, "\"mt\":\"T\",\"body\":\"0102A0\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no T trace line carrying the image");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"dir\":\"tx\""),
                           "T trace not tx");
}

// After I and T, the next slot sends P; the trace line must show mt=P
// with an empty body.
static void test_trace_emits_tx_poll(void) {
  TracerRig rig;
  rig.run(0, 6);  // I@0, T@3, P@6 (the wait->idle step costs a tick)
  const std::string* line =
      findContaining(rig.lines, "\"mt\":\"P\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no P trace line");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"dir\":\"tx\""),
                           "P trace not tx");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"body\":\"\""),
                           "P trace body not empty");
}

// A verified R reply must surface as a trace line with dir=rx, mt=R,
// and the reply body (acceptance #2/#3 visibility for the reply pair).
static void test_trace_emits_rx_reply(void) {
  TracerRig rig;
  rig.run(0, 5);  // I, T, P outstanding at tick 5 (gate due at 6)
  const uint8_t body[] = {0xA5, 0x01};
  const CMRIPacket reply = [&] {
    CMRIPacket p;
    p.ua = 5 + kUaOffset;
    p.mt = 'R';
    p.setBody(body, sizeof(body));
    return p;
  }();
  uint8_t wire[16];
  const size_t n = encodeFrame(reply, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN_size_t(0, n);
  rig.port.queueRx(wire, n);
  rig.tick(6);  // transport pumps RX before the gate check
  const std::string* line =
      findContaining(rig.lines, "\"dir\":\"rx\",\"mt\":\"R\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no rx R trace line");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"body\":\"A501\""),
                           "R trace body wrong");
}

// High-rate trace lines should not carry static image/version identity
// metadata; those fields are reserved for epoch/status lines.
static void test_trace_omits_image_and_version(void) {
  TracerRig rig;
  rig.tick(0);
  const std::string* line = findContaining(rig.lines, "\"event\":\"trace\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no trace line emitted for I");
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"image\":\""),
                            "trace should omit image");
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"version\":\""),
                            "trace should omit version");
}

// Event lines generated by host progression (e.g. miss) should omit
// static identity metadata.
static void test_event_line_omits_image_and_version(void) {
  TracerRig rig;
  rig.run(0, 8);  // reaches poll timeout path and emits a miss event
  const std::string* line = findContaining(rig.lines, "\"event\":\"miss\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no miss event line emitted");
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"image\":\""),
                            "event line should omit image");
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"version\":\""),
                            "event line should omit version");
}

// --------------------------------------------------- output verbs (setbit)

// setbit <n> <0|1> mutates the output image and marks it dirty so the
// engine sends a full T on the next slot (acceptance #3 exercisable).
static void test_verb_setbit_mutates_output_and_marks_dirty(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("setbit 0 1"));
  TEST_ASSERT_TRUE(rig.node->outputBit(0));
  rig.run(0, 3);  // I at 0, T (dirty) at 2
  const std::string* line =
      findContaining(rig.lines, "\"mt\":\"T\",\"body\":\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no T after setbit");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"body\":\"010000\""),
                           "T body did not carry the set bit");
}

// A value other than 0 or 1 is rejected with an error line; the image
// is unchanged.
static void test_verb_setbit_bad_value_emits_error(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("setbit 0 2"));
  TEST_ASSERT_FALSE(rig.node->outputBit(0));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "bad value did not emit an error line");
}

// A bit beyond the output image is rejected with an error line.
static void test_verb_setbit_out_of_range_emits_error(void) {
  TracerRig rig;  // outputBytes = 3 -> 24 bits
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("setbit 100 1"));
  TEST_ASSERT_FALSE(rig.node->outputBit(100));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "out-of-range bit did not emit an error line");
}

// ----------------------------------------------- output verbs (writeoutputs)

// writeoutputs <hex> copies the bytes into the output image and marks it
// dirty; the next T carries them.
static void test_verb_writeoutputs_mutates_image(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("writeoutputs 0102A0"));
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->outputByte(0));
  TEST_ASSERT_EQUAL_HEX8(0x02, rig.node->outputByte(1));
  TEST_ASSERT_EQUAL_HEX8(0xA0, rig.node->outputByte(2));
  rig.run(0, 3);
  const std::string* line =
      findContaining(rig.lines, "\"mt\":\"T\",\"body\":\"0102A0\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no T carrying the written image");
}

// Non-hex characters are rejected with an error line; the image is
// unchanged.
static void test_verb_writeoutputs_bad_hex_emits_error(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("writeoutputs 0G12"));
  TEST_ASSERT_EQUAL_HEX8(0x00, rig.node->outputByte(0));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "bad hex did not emit an error line");
}

// An odd number of hex digits is rejected with an error line.
static void test_verb_writeoutputs_odd_length_emits_error(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("writeoutputs 010"));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "odd hex length did not emit an error line");
}

// ----------------------------------------------- output verbs (forcetx)

// forcetx re-marks the image dirty so a full T is re-sent even when no
// output bit changed (e.g. after a suspected node reset).
static void test_verb_forcetx_remarks_dirty(void) {
  TracerRig rig;
  rig.run(0, 5);  // I, T (empty image), P outstanding; dirty cleared
  TEST_ASSERT_EQUAL_INT(1, countContaining(rig.lines, "\"mt\":\"T\""));
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("forcetx"));
  rig.tick(6);  // P gate expires (miss) -> next slot sends the forced T
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      2, countContaining(rig.lines, "\"mt\":\"T\""),
      "forcetx did not schedule a second full T");
}

// ----------------------------------------------- telemetry outputs hex field

// The main telemetry line carries the output image as hex alongside the
// inputs, so the bench sees what the Host is commanding.
static void test_telemetry_line_carries_outputs_hex(void) {
  TracerRig rig;
  rig.verb("setbit 0 1");
  rig.verb("status");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"event\":\"status\""),
                           "status did not emit a status line");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"outputs\":\"010000\""),
                           "status line missing/incorrect outputs hex");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"inputs\":\"0000\""),
                           "status line missing/incorrect inputs hex");
}

// Identity metadata stays available on status lines for tooling that
// validates firmware image and version.
static void test_status_line_includes_image_and_version(void) {
  TracerRig rig;
  rig.verb("status");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"event\":\"status\""),
                           "status did not emit a status line");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"image\":\"test\""),
                           "status should include image");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"version\":\"0.0\""),
                           "status should include version");
}

// Identity metadata stays available on epoch lines for stream anchors.
static void test_epoch_line_includes_image_and_version(void) {
  TracerRig rig;
  rig.shell.setNow(42);
  rig.shell.emitEpoch("bootMs", "42");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"event\":\"epoch\""),
                           "epoch did not emit an epoch line");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"image\":\"test\""),
                           "epoch should include image");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"version\":\"0.0\""),
                           "epoch should include version");
}

// ----------------------------------------------- unknown verb (pin behavior)

static void test_unknown_verb_emits_error(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("frobnicate"));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "unknown verb did not emit an error line");
  TEST_ASSERT_TRUE_MESSAGE(
      contains(rig.lines.back(), "\"unknownVerb\":\"frobnicate\""),
      "error line did not echo the unknown verb");
}

// ------------------------------------------------------------------- main

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_trace_emits_tx_init_with_C_body);
  RUN_TEST(test_trace_emits_tx_transmit_with_output_image);
  RUN_TEST(test_trace_emits_tx_poll);
  RUN_TEST(test_trace_emits_rx_reply);
  RUN_TEST(test_trace_omits_image_and_version);
  RUN_TEST(test_event_line_omits_image_and_version);
  RUN_TEST(test_verb_setbit_mutates_output_and_marks_dirty);
  RUN_TEST(test_verb_setbit_bad_value_emits_error);
  RUN_TEST(test_verb_setbit_out_of_range_emits_error);
  RUN_TEST(test_verb_writeoutputs_mutates_image);
  RUN_TEST(test_verb_writeoutputs_bad_hex_emits_error);
  RUN_TEST(test_verb_writeoutputs_odd_length_emits_error);
  RUN_TEST(test_verb_forcetx_remarks_dirty);
  RUN_TEST(test_telemetry_line_carries_outputs_hex);
  RUN_TEST(test_status_line_includes_image_and_version);
  RUN_TEST(test_epoch_line_includes_image_and_version);
  RUN_TEST(test_unknown_verb_emits_error);
  return UNITY_END();
}
