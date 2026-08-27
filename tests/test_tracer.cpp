// test_tracer.cpp — tests for the shared TracerShell: the I/T
// packet trace telemetry (D7 onTrace), the output verbs that make T
// bench-exercisable, the outputs hex field, and error lines.
//
// The engine is driven against a minimal FakePort + SerialCMRITransport
// + CMRIHost (UA 5, 2 input / 3 output bytes). The fake port's
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
using CMRInet::kWireUAOffset;
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

/// One shell + host + transport, capturing every telemetry line.
///
/// The rig holds no node either: `node()` resolves it, so a test that
/// deletes UA 5 cannot accidentally keep asserting through a stale
/// handle (Design v1.2 D5).
struct TracerRig {
  FakePort port;
  SerialCMRITransport transport;
  CMRIHost host;
  TracerShell shell;
  std::vector<std::string> lines;

  /// `replyTimeoutMs` defaults to 1 tick, which suits tests that want a
  /// poll to time out immediately. A test that needs to land a reply
  /// inside the gate should widen it rather than try to hit a one-tick
  /// window -- the schedule's step count is an implementation detail,
  /// and timing a reply against it is how a test goes green for the
  /// wrong reason.
  explicit TracerRig(uint16_t inBytes = 2, uint16_t outBytes = 3,
                     uint32_t replyTimeoutMs = 1)
      : transport(port), host(transport, fastConfig(replyTimeoutMs)) {
    host.addRemoteNode(5, inBytes, outBytes);
    TEST_ASSERT_NOT_NULL_MESSAGE(host.node(5), "addRemoteNode failed in rig");
    shell.bind(host, transport, "test", "0.0", &TracerRig::writeLine_, this);
    host.begin();
  }

  /// The rig's node, at the point of use. Asserts it is still there.
  RemoteNodeHandle* node() {
    RemoteNodeHandle* n = host.node(5);
    TEST_ASSERT_NOT_NULL_MESSAGE(n, "node 5 is gone");
    return n;
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
  static CMRIHostConfig fastConfig(uint32_t replyTimeoutMs) {
    CMRIHostConfig c;
    c.postInitSettleMs = 0;  // I -> T with no settle delay
    c.postTxGapMs = 0;       // T -> idle with no gap
    c.pollPacingMs = 0;      // back-to-back exchanges
    c.replyTimeoutMs = replyTimeoutMs;
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
  TEST_ASSERT_TRUE(rig.node()->setOutputs(image, 3));
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
    p.wireUA = 5 + kWireUAOffset;
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
                    rig.verb("setbit 5 0 1"));
  TEST_ASSERT_TRUE(rig.node()->outputBit(0));
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
                    rig.verb("setbit 5 0 2"));
  TEST_ASSERT_FALSE(rig.node()->outputBit(0));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "bad value did not emit an error line");
}

// A bit beyond the output image is rejected with an error line.
static void test_verb_setbit_out_of_range_emits_error(void) {
  TracerRig rig;  // outputBytes = 3 -> 24 bits
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("setbit 5 100 1"));
  TEST_ASSERT_FALSE(rig.node()->outputBit(100));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "out-of-range bit did not emit an error line");
}

// ----------------------------------------------- output verbs (writeoutputs)

// writeoutputs <hex> copies the bytes into the output image and marks it
// dirty; the next T carries them.
static void test_verb_writeoutputs_mutates_image(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("writeoutputs 5 0102A0"));
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node()->outputByte(0));
  TEST_ASSERT_EQUAL_HEX8(0x02, rig.node()->outputByte(1));
  TEST_ASSERT_EQUAL_HEX8(0xA0, rig.node()->outputByte(2));
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
                    rig.verb("writeoutputs 5 0G12"));
  TEST_ASSERT_EQUAL_HEX8(0x00, rig.node()->outputByte(0));
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"event\":\"error\""),
                           "bad hex did not emit an error line");
}

// An odd number of hex digits is rejected with an error line.
static void test_verb_writeoutputs_odd_length_emits_error(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("writeoutputs 5 010"));
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
                    rig.verb("forcetx 5"));
  rig.tick(6);  // P gate expires (miss) -> next slot sends the forced T
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      2, countContaining(rig.lines, "\"mt\":\"T\""),
      "forcetx did not schedule a second full T");
}

// ----------------------------------------------- telemetry outputs hex field

// A per-node status line carries the output image as hex alongside the
// inputs, so the bench sees what the Host is commanding.
static void test_node_status_line_carries_outputs_hex(void) {
  TracerRig rig;
  rig.verb("setbit 5 0 1");
  rig.verb("status 5");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"event\":\"status\""),
                           "status did not emit a status line");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"ua\":5"),
                           "node status line missing its UA");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"outputs\":\"010000\""),
                           "status line missing/incorrect outputs hex");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"inputs\":\"0000\""),
                           "status line missing/incorrect inputs hex");
}

// The two status scopes carry different fields, because the fields have
// different owners. Host scope has no "the" node to report an image for;
// bare `status` used to answer for whatever node happened to be bound at
// startup, which is only meaningful with exactly one node.
static void test_host_status_reports_the_table_not_one_node(void) {
  TracerRig rig;
  rig.verb("status");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"nodes\":1"),
                           "host status did not report the live node count");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"orphaned\":0"),
                           "host status did not surface orphanedExchanges");
  TEST_ASSERT_FALSE_MESSAGE(contains(line, "\"inputs\":"),
                            "host status leaked a per-node image");
  // The roster is what makes membership legible, and it speaks the UA.
  TEST_ASSERT_TRUE_MESSAGE(
      contains(line, "\"roster\":[{\"ua\":5,"),
      "host status roster missing or not keyed by the UA");
}

// The trace line now carries a uniform field set: wireUA, legal,
// and UA. For a legal packet (UA 5, wire UA 70), the line shows
// the decoded ordinal and the raw wire byte alongside it, with
// legal=true. The old lowercase "ua" field name is gone from trace
// lines (it stays on status/roster lines, which are a different
// surface).
static void test_telemetry_speaks_the_ua_not_the_wire_byte(void) {
  TracerRig rig;
  rig.tick(0);  // emits the I trace
  const std::string* line = findContaining(rig.lines, "\"event\":\"trace\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line, "no trace line emitted");
  // The decoded semantic UA is in the "UA" field.
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"UA\":5,"),
                           "trace line does not report the semantic UA");
  // The wire byte is now explicitly carried in "wireUA".
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"wireUA\":70,"),
                           "trace line missing the wireUA field");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"legal\":true"),
                           "trace line missing the legal flag");
  // The old lowercase "ua" field name is gone from trace lines.
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"ua\":"),
                            "trace line still carries the old ua field name");
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"address\":"),
                            "trace line still carries the retired address key");
}

// An illegal wire-UA byte produces a trace line with legal=false and
// UA=null. The raw wire byte is carried in wireUA for diagnosis.
// Per ADR-0001: raw views are not decoded telemetry.
static void test_trace_line_for_illegal_wire_ua(void) {
  TracerRig rig;
  rig.run(0, 3);  // I, T, settle past the preamble
  rig.lines.clear();

  // Encode a frame with an illegal wire-UA byte (10 < 65).
  CMRIPacket illegal;
  illegal.wireUA = 10;
  illegal.mt = 'R';
  static const uint8_t kBody[] = {0xA5, 0x01};
  illegal.setBody(kBody, sizeof(kBody));
  uint8_t wire[CMRInet::kMaxWireFrame];
  const size_t n = encodeFrame(illegal, wire, sizeof(wire));
  TEST_ASSERT_TRUE_MESSAGE(n > 0, "encodeFrame rejected the illegal-UA packet");
  rig.port.queueRx(wire, n);

  // Tick enough for the transport to pump the bytes through the
  // decoder and hand the packet up. The trace listener fires before
  // the gate, so the trace line is emitted regardless.
  rig.run(4, 10);

  const std::string* line =
      findContaining(rig.lines, "\"legal\":false");
  TEST_ASSERT_NOT_NULL_MESSAGE(line,
                              "no illegal-UA trace line emitted");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"wireUA\":10,"),
                           "illegal trace line missing the wireUA field");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"UA\":null"),
                           "illegal trace line should have UA:null");
  TEST_ASSERT_FALSE_MESSAGE(contains(*line, "\"legal\":true"),
                            "illegal trace line marked legal=true");
}

// --------------------------------------------- runtime mutation verbs (D5)

// A verb naming a UA with no live node must say so. After runtime delete
// this is an ordinary outcome, and a verb that silently did nothing would
// misreport its own failure -- the #82 shape.
static void test_verb_on_unknown_ua_reports_no_such_node(void) {
  TracerRig rig;
  const char* verbs[] = {"status 9", "quiesce 9", "resume 9", "forcetx 9",
                         "setbit 9 0 1", "writeoutputs 9 01",
                         "node enable 9", "node disable 9"};
  for (const char* v : verbs) {
    rig.lines.clear();
    TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled, rig.verb(v));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, static_cast<int>(rig.lines.size()), v);
    TEST_ASSERT_TRUE_MESSAGE(
        contains(rig.lines.back(), "\"noSuchNode\""), v);
  }
}

// node add / node delete move the table at runtime, and the roster is
// the evidence.
static void test_node_add_and_delete_verbs_move_the_roster(void) {
  TracerRig rig;
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("node add 9 1 2"));
  TEST_ASSERT_NOT_NULL(rig.host.node(9));
  TEST_ASSERT_TRUE_MESSAGE(
      contains(rig.lines.back(), "\"event\":\"node_add\""),
      "node add did not report itself");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"ua\":9"),
                           "node add did not name the UA");

  rig.verb("status");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"nodes\":2"),
                           "roster did not grow");

  // Adding the same UA again is a reported rejection, not a no-op.
  rig.lines.clear();
  rig.verb("node add 9 1 2");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"addFailed\""),
                           "duplicate add did not report a failure");

  rig.lines.clear();
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("node delete 9"));
  TEST_ASSERT_NULL(rig.host.node(9));
  const std::string& gone = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(gone, "\"event\":\"node_delete\""),
                           "node delete did not report itself");
  // Named but absent: the distinction a reader needs after a delete.
  TEST_ASSERT_TRUE_MESSAGE(contains(gone, "\"ua\":9"),
                           "node delete did not name the departed UA");
  TEST_ASSERT_TRUE_MESSAGE(contains(gone, "\"present\":false"),
                           "node delete did not mark the UA absent");
  TEST_ASSERT_TRUE_MESSAGE(contains(gone, "\"nodes\":1"),
                           "roster did not shrink");

  // Deleting it twice is a reported failure.
  rig.lines.clear();
  rig.verb("node delete 9");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"deleteFailed\""),
                           "a second delete did not report a failure");
}

// node geometry re-announces NI/NO, which is the whole reason it forces
// a re-init.
static void test_node_geometry_verb_reannounces_ni_no(void) {
  TracerRig rig;
  rig.run(0, 6);  // I, T, P for the original 2/3 geometry
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("node geometry 5 4 1"));
  TEST_ASSERT_EQUAL_size_t(4, rig.node()->inputLength());
  TEST_ASSERT_EQUAL_size_t(1, rig.node()->outputLength());

  rig.lines.clear();
  rig.run(7, 20);
  // The new I body carries NI=04 NO=01 in the CPNODE 'C' dialect.
  const std::string* line =
      findContaining(rig.lines, "\"body\":\"43000000000401FFFFFFFFFFFF\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      line, "geometry change did not re-announce the new NI/NO in an I");
}

// The geometry-change event line carries the previous NI/NO alongside the
// new (in/out), so a reader can tell what changed without dereferencing
// the handle (issue #91). The event is the ack now: the verb handler
// emits nothing on success, and onHostEvent_ renders this line.
static void test_geometry_event_line_carries_previous_geometry(void) {
  TracerRig rig(2, 3, /*replyTimeoutMs=*/50);  // declares 2 in / 3 out
  rig.run(0, 6);                               // I, T, P for the original geometry
  rig.lines.clear();
  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("node geometry 5 4 1"));
  const std::string* line =
      findContaining(rig.lines, "\"event\":\"node_geometry\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(line,
                               "geometry change emitted no node_geometry line");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"ua\":5"),
                           "geometry line missing its UA");
  // New geometry on the node line as in/out.
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"in\":4,\"out\":1"),
                           "geometry line missing the new in/out");
  // Previous geometry carried by value in the event.
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"previousIn\":2"),
                           "geometry line missing previousIn");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"previousOut\":3"),
                           "geometry line missing previousOut");
}

// A direct API delete (no C&C verb) renders the node_delete host-scoped
// line through the bound shell, because the engine fires kNodeDeleted
// regardless of the trigger (issue #91). This is the #88 case: a sketch
// mutates the table in code, and the capture still sees it.
static void test_direct_api_delete_renders_node_delete_line(void) {
  TracerRig rig;
  rig.run(0, 3);  // let the engine settle so lastTickMs_ is set
  rig.lines.clear();
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    rig.host.deleteRemoteNode(5));
  const std::string* line =
      findContaining(rig.lines, "\"event\":\"node_delete\"");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      line, "direct-API delete rendered no node_delete line");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"ua\":5"),
                           "node_delete line missing the departed UA");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"present\":false"),
                           "node_delete line did not mark the UA absent");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"nodes\":0"),
                           "node_delete line did not reflect the empty table");
  TEST_ASSERT_TRUE_MESSAGE(contains(*line, "\"roster\":[]"),
                           "node_delete line did not render the empty roster");
}

// A mutation under live traffic orphans the outstanding exchange, and the
// host-scope counter is where a bench operator can see it happened. A
// nonzero value is the signal that mutation is happening mid-traffic --
// precisely when the subtle failures occur.
static void test_host_status_surfaces_an_orphaned_exchange(void) {
  TracerRig rig;
  // Tick until the P is actually on the wire rather than assuming which
  // tick sends it: the schedule's step count is an implementation
  // detail, and guessing it is how this test first went green for the
  // wrong reason.
  bool polled = false;
  for (uint32_t t = 0; t <= 50 && !polled; ++t) {
    rig.tick(t);
    polled = (findContaining(rig.lines, "\"mt\":\"P\"") != nullptr);
  }
  TEST_ASSERT_TRUE_MESSAGE(polled, "node 5 was never polled");

  TEST_ASSERT_EQUAL(TracerShell::VerbResult::kHandled,
                    rig.verb("node delete 5"));
  rig.verb("status");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"orphaned\":1"),
                           "an orphaned exchange was invisible in status");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"nodes\":0"),
                           "the table should now be empty");
  TEST_ASSERT_TRUE_MESSAGE(contains(rig.lines.back(), "\"roster\":[]"),
                           "an empty table should render an empty roster");
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

// ------------------------------------------- conformance surface (#85)

// Before any reply the node line reports all three stored axes, no
// observed geometry, and no fault. The scalar `state` alone cannot say
// this much, which is why the axes are on the line at all.
static void test_node_status_line_carries_the_health_axes(void) {
  TracerRig rig;
  rig.verb("status 5");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"state\":\"UNINITIALIZED\""),
                           "status line lost the projected state");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"liveness\":\"RESPONSIVE\""),
                           "status line missing the liveness axis");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"imageState\":\"NONE\""),
                           "status line missing the image axis");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"conformance\":\"UNKNOWN\""),
                           "status line missing the conformance axis");
  // Nothing has been demonstrated yet. This must render as null: 0 is a
  // legal geometry, so any number here would be read as a byte count.
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"observedIn\":null"),
                           "unobserved geometry did not render as null");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"fault\":{\"name\":\"none\""),
                           "status line missing the last-fault block");
}

// The #80 shape as an operator would meet it: ask the tracer about a
// node whose declared geometry the hardware disagrees with, and get
// back the disagreement named, classified, and quantified. Every one of
// these fields was absent before #85 -- the line carried only the
// projected state, which said UNINITIALIZED.
static void test_node_status_line_reports_the_geometry_disagreement(void) {
  // A wide reply gate: this test is about what the status line says,
  // not about hitting a one-tick window.
  TracerRig rig(2, 3, /*replyTimeoutMs=*/50);  // declares 2 input bytes
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  const CMRIPacket reply = [&] {
    CMRIPacket p;
    p.wireUA = 5 + kWireUAOffset;
    p.mt = 'R';
    p.setBody(threeBytes, sizeof(threeBytes));
    return p;
  }();
  uint8_t wire[16];
  const size_t n = encodeFrame(reply, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN_size_t(0, n);

  // Wait for the poll to actually reach the wire, then answer it. Which
  // tick that happens on is the schedule's business, not this test's.
  uint32_t t = 0;
  bool polled = false;
  for (; t <= 50 && !polled; ++t) {
    rig.tick(t);
    polled = (findContaining(rig.lines, "\"mt\":\"P\"") != nullptr);
  }
  TEST_ASSERT_TRUE_MESSAGE(polled, "node 5 was never polled");
  rig.tick(t);  // the send completes and the reply gate opens
  rig.port.queueRx(wire, n);

  bool faulted = false;
  for (uint32_t u = t + 1; u <= t + 40 && !faulted; ++u) {
    rig.tick(u);
    faulted = rig.node()->statistics().errors >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(
      faulted, "the wrong-geometry reply was never matched to the poll");

  rig.verb("status 5");
  const std::string& line = rig.lines.back();
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"conformance\":\"NONCONFORMING\""),
                           "status line did not report the verdict");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"state\":\"MISCONFIGURED\""),
                           "status line did not project MISCONFIGURED");
  // Claim against evidence, side by side: `in` is what the Host
  // declared, `observedIn` is what the Node demonstrated (D14).
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"in\":2"),
                           "status line lost the declared geometry");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"observedIn\":3"),
                           "status line did not report the observed geometry");
  // The fault block carries the classification, so an analyzer can tell
  // "go fix the configuration" from "go fix the firmware" without
  // reimplementing the taxonomy.
  TEST_ASSERT_TRUE_MESSAGE(
      contains(line, "\"name\":\"image geometry mismatch\""),
      "fault block did not name the fault");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"layer\":\"image\""),
                           "fault block did not report the layer");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"attribution\":\"disagreement\""),
                           "fault block did not report the attribution");
  TEST_ASSERT_TRUE_MESSAGE(contains(line, "\"expected\":2,\"observed\":3"),
                           "fault block did not carry expected vs observed");
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
  RUN_TEST(test_node_status_line_carries_outputs_hex);
  RUN_TEST(test_host_status_reports_the_table_not_one_node);
  RUN_TEST(test_telemetry_speaks_the_ua_not_the_wire_byte);
  RUN_TEST(test_trace_line_for_illegal_wire_ua);
  RUN_TEST(test_verb_on_unknown_ua_reports_no_such_node);
  RUN_TEST(test_node_add_and_delete_verbs_move_the_roster);
  RUN_TEST(test_node_geometry_verb_reannounces_ni_no);
  RUN_TEST(test_geometry_event_line_carries_previous_geometry);
  RUN_TEST(test_direct_api_delete_renders_node_delete_line);
  RUN_TEST(test_host_status_surfaces_an_orphaned_exchange);
  RUN_TEST(test_status_line_includes_image_and_version);
  RUN_TEST(test_epoch_line_includes_image_and_version);
  RUN_TEST(test_node_status_line_carries_the_health_axes);
  RUN_TEST(test_node_status_line_reports_the_geometry_disagreement);
  RUN_TEST(test_unknown_verb_emits_error);
  return UNITY_END();
}
