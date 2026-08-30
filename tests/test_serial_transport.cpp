// test_serial_transport.cpp — tests for SerialCMRITransport: TXEN
// discipline (assert, write, flush to full drain, deassert),
// sendComplete semantics, codec integration on receive, inter-byte
// timeout, and error accounting through transport stats.
//
// The transport is driven against FakeSerialPort, a scriptable
// byte-port double that records the exact order of TXEN transitions
// and writes. Every timing assertion runs on the injected clock; no
// test sleeps or reads a wall clock.

#include <string.h>

#include "CMRInet.h"
#include "transport/serial.h"
#include "transport/serialPort.h"
#include "unity.h"

using CMRInet::CMRIPacket;
using CMRInet::SerialPort;
using CMRInet::encodeFrame;
using CMRInet::kMaxBody;
using CMRInet::kWireUAOffset;
using CMRInet::SerialCMRITransport;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------- fake port

/// Scriptable SerialPort double. Bytes written land in txData();
/// TXEN transitions and writes land in an ordered event log; received
/// bytes are queued by the test with queueRx(). The port models a UART
/// whose software buffer accepts writeLimit() bytes per call.
class FakeSerialPort : public SerialPort {
 public:
  enum class Op : uint8_t { kAssert, kDeassert, kWrite };
  struct Event {
    Op op;
    size_t length;  // bytes accepted (kWrite only)
  };

  static constexpr size_t kBufferCapacity = 2048;
  static constexpr size_t kEventCapacity = 64;

  void begin() override { beganCount_++; }

  int readByte() override {
    if (rxHead_ >= rxCount_) {
      return -1;
    }
    return rx_[rxHead_++];
  }

  size_t writeBytes(const uint8_t* bytes, size_t length) override {
    size_t accepted = length;
    if (writeLimit_ != 0 && accepted > writeLimit_) {
      accepted = writeLimit_;
    }
    if (txCount_ + accepted > kBufferCapacity) {
      accepted = kBufferCapacity - txCount_;
    }
    if (accepted != 0) {
      memcpy(tx_ + txCount_, bytes, accepted);
      txCount_ += accepted;
      pushEvent_(Op::kWrite, accepted);
    }
    return accepted;
  }

  bool transmitDrained() const override { return drained_; }

  void setTransmitEnable(bool enabled) override {
    txen_ = enabled;
    pushEvent_(enabled ? Op::kAssert : Op::kDeassert, 0);
  }

  uint32_t byteDurationMicros() const override { return byteMicros_; }

  uint32_t hardwareErrorCount() const override { return hardwareErrors_; }

  // ---- test controls ----

  void queueRx(const uint8_t* bytes, size_t length) {
    TEST_ASSERT_TRUE_MESSAGE(rxCount_ + length <= kBufferCapacity,
                             "fake rx buffer overfilled by test");
    memcpy(rx_ + rxCount_, bytes, length);
    rxCount_ += length;
  }

  void setWriteLimit(size_t limit) { writeLimit_ = limit; }  // 0 = unlimited
  void setDrained(bool drained) { drained_ = drained; }
  void setHardwareErrorCount(uint32_t count) { hardwareErrors_ = count; }

  bool txenAsserted() const { return txen_; }
  const uint8_t* txData() const { return tx_; }
  size_t txCount() const { return txCount_; }
  size_t eventCount() const { return eventCount_; }
  const Event& event(size_t i) const { return events_[i]; }
  int beganCount() const { return beganCount_; }

 private:
  void pushEvent_(Op op, size_t length) {
    TEST_ASSERT_TRUE_MESSAGE(eventCount_ < kEventCapacity,
                             "fake event log overfilled");
    events_[eventCount_].op = op;
    events_[eventCount_].length = length;
    eventCount_++;
  }

  uint8_t rx_[kBufferCapacity] = {0};
  size_t rxHead_ = 0;
  size_t rxCount_ = 0;
  uint8_t tx_[kBufferCapacity] = {0};
  size_t txCount_ = 0;
  Event events_[kEventCapacity] = {};
  size_t eventCount_ = 0;
  size_t writeLimit_ = 0;  // 0 = accept everything
  bool drained_ = true;
  bool txen_ = false;
  uint32_t byteMicros_ = 500;  // 2000 chars/s: easy math (6 bytes = 3 ms)
  uint32_t hardwareErrors_ = 0;
  int beganCount_ = 0;
};

// ---------------------------------------------------------------- helpers

/// Build a packet for node UA `addr` (UA = addr + 65).
static CMRIPacket makePacket(uint8_t addr, uint8_t mt,
                             const uint8_t* body = nullptr, size_t len = 0) {
  CMRIPacket p;
  p.wireUA = static_cast<uint8_t>(addr + kWireUAOffset);
  p.mt = mt;
  TEST_ASSERT_TRUE_MESSAGE(p.setBody(body, len), "setBody rejected test body");
  return p;
}

/// Encode `p` as a wire frame into `out`; assert success.
static size_t encodeInto(const CMRIPacket& p, uint8_t* out, size_t capacity) {
  const size_t n = encodeFrame(p, out, capacity);
  TEST_ASSERT_TRUE_MESSAGE(n > 0, "encodeFrame failed for test frame");
  return n;
}

// A bare P poll frame is 6 wire bytes: SYN SYN STX UA MT ETX. At the
// fake's 500 us character time that is 3 ms of wire time.
static constexpr uint32_t kPollFrameBytes = 6;
static constexpr uint32_t kPollWireMs = 3;

// ------------------------------------------------------------- send / TXEN

static void test_idle_reports_send_complete(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendComplete());  // idle transport reports true
  TEST_ASSERT_FALSE(port.txenAsserted());
  TEST_ASSERT_TRUE(t.stats().linkUp);
}

static void test_send_writes_frame_with_txen_discipline(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);

  const CMRIPacket poll = makePacket(5, 'P');
  uint8_t expected[16];
  const size_t n = encodeInto(poll, expected, sizeof(expected));
  TEST_ASSERT_EQUAL_size_t(kPollFrameBytes, n);

  TEST_ASSERT_TRUE(t.sendPacket(poll));
  // TXEN asserted, then the whole frame written as one gapless write.
  TEST_ASSERT_TRUE(port.txenAsserted());
  TEST_ASSERT_EQUAL_size_t(n, port.txCount());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, port.txData(), n);
  TEST_ASSERT_FALSE(t.sendComplete());  // accepted != on the wire

  t.tick(kPollWireMs - 1);
  TEST_ASSERT_FALSE(t.sendComplete());  // still draining
  TEST_ASSERT_TRUE(port.txenAsserted());

  t.tick(kPollWireMs);
  TEST_ASSERT_TRUE(t.sendComplete());  // drained: TXEN dropped at once
  TEST_ASSERT_FALSE(port.txenAsserted());

  // Event order: begin() releases the driver, then the send is
  // assert, one write, deassert. Nothing else.
  TEST_ASSERT_EQUAL_size_t(4, port.eventCount());
  TEST_ASSERT_TRUE(port.event(0).op == FakeSerialPort::Op::kDeassert);
  TEST_ASSERT_TRUE(port.event(1).op == FakeSerialPort::Op::kAssert);
  TEST_ASSERT_TRUE(port.event(2).op == FakeSerialPort::Op::kWrite);
  TEST_ASSERT_EQUAL_size_t(n, port.event(2).length);
  TEST_ASSERT_TRUE(port.event(3).op == FakeSerialPort::Op::kDeassert);

  TEST_ASSERT_EQUAL_UINT32(1, t.stats().packetsSent);
}

static void test_txen_holds_until_port_reports_drained(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));

  // The wire-time estimate has elapsed, but the port still holds bytes
  // (shift register, FIFO): TXEN must not drop early.
  port.setDrained(false);
  t.tick(kPollWireMs + 5);
  TEST_ASSERT_FALSE(t.sendComplete());
  TEST_ASSERT_TRUE(port.txenAsserted());

  port.setDrained(true);
  t.tick(kPollWireMs + 6);
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_FALSE(port.txenAsserted());
}

static void test_send_backpressure_while_draining(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_FALSE(t.sendPacket(makePacket(6, 'P')));  // still draining
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);
  t.tick(kPollWireMs);
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(6, 'P')));
}

static void test_send_rejects_oversized_body(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);
  CMRIPacket p = makePacket(5, 'T');
  p.length = static_cast<uint16_t>(kMaxBody + 1);  // forge a bad length
  TEST_ASSERT_FALSE(t.sendPacket(p));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);
  TEST_ASSERT_FALSE(port.txenAsserted());  // rejected before TXEN moved
  TEST_ASSERT_EQUAL_size_t(0, port.txCount());
}

static void test_chunked_write_completes_across_ticks(void) {
  FakeSerialPort port;
  port.setWriteLimit(4);  // UART buffer accepts 4 bytes per call
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);

  const CMRIPacket poll = makePacket(5, 'P');
  uint8_t expected[16];
  const size_t n = encodeInto(poll, expected, sizeof(expected));

  TEST_ASSERT_TRUE(t.sendPacket(poll));
  TEST_ASSERT_EQUAL_size_t(4, port.txCount());  // first chunk only
  TEST_ASSERT_FALSE(t.sendComplete());

  t.tick(1);  // pump the remainder
  TEST_ASSERT_EQUAL_size_t(n, port.txCount());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, port.txData(), n);
  TEST_ASSERT_FALSE(t.sendComplete());

  t.tick(kPollWireMs);  // full-frame wire time still governs
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_FALSE(port.txenAsserted());
}

// --------------------------------------------------------------- receive

static void test_rx_decodes_whole_validated_frame(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  const uint8_t body[] = {0x00, 0x02, 0xFF, 0x10, 0x41};  // needs escaping
  const CMRIPacket sent = makePacket(23, 'R', body, sizeof(body));
  uint8_t wire[64];
  const size_t n = encodeInto(sent, wire, sizeof(wire));
  port.queueRx(wire, n);

  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(sent.wireUA, got.wireUA);
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
  TEST_ASSERT_EQUAL_UINT16(sizeof(body), got.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got.body, sizeof(body));
  TEST_ASSERT_FALSE(t.receivePacket(got));  // at most one per call, then empty
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().packetsReceived);
}

static void test_rx_truncated_frame_never_delivers(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  const uint8_t truncated[] = {0xFF, 0xFF, 0x02, 0x46, 0x52};  // no ETX
  port.queueRx(truncated, sizeof(truncated));
  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));

  // Line silence past the inter-byte timeout abandons the partial
  // frame, and the abort surfaces through transport stats. The shipped
  // default abort is 100 ms (D13); the decoder abandons on a gap
  // strictly greater than the limit, so tick just past 100.
  t.tick(101);
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_UINT32(1, t.decoderStatistics().timeoutAborts);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().decodeErrors);

  // The link recovers: the next whole frame decodes.
  const CMRIPacket ok = makePacket(5, 'R');
  uint8_t wire[16];
  const size_t n = encodeInto(ok, wire, sizeof(wire));
  port.queueRx(wire, n);
  t.tick(102);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(ok.wireUA, got.wireUA);
}

static void test_rx_reply_arrives_after_drain(void) {
  // Physics-aware replacement for the v1.1 fiction test. A Node
  // cannot reply until it has received ETX (interop 2.3.15, E10), so
  // its reply arrives after the Host's TXEN deasserts — not while ETX
  // drains. The prior test queued a reply "while draining" with no
  // wire-propagation model, encoding the fiction that the bench
  // disproved. This test models the physics: the reply is queued
  // only after the drain completes and TXEN drops.
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));

  // Drain completes: TXEN drops, sendComplete() true.
  t.tick(kPollWireMs);
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_FALSE(port.txenAsserted());

  // Only now (after ETX has left the wire) can a Node reply.
  const CMRIPacket reply = makePacket(5, 'R');
  uint8_t wire[16];
  const size_t n = encodeInto(reply, wire, sizeof(wire));
  port.queueRx(wire, n);
  t.tick(kPollWireMs + 1);

  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(reply.wireUA, got.wireUA);
}

static void test_rx_queue_overflow_keeps_oldest_drops_newest(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  uint8_t wire[16];
  for (uint8_t i = 0; i < SerialCMRITransport::kRxQueueCapacity + 1; ++i) {
    const size_t n = encodeInto(makePacket(i, 'R'), wire, sizeof(wire));
    port.queueRx(wire, n);
  }
  t.tick(0);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().receiveDrops);
  CMRIPacket got;
  for (uint8_t i = 0; i < SerialCMRITransport::kRxQueueCapacity; ++i) {
    TEST_ASSERT_TRUE(t.receivePacket(got));
    TEST_ASSERT_EQUAL_HEX8(i + kWireUAOffset, got.wireUA);  // arrival order held
  }
  TEST_ASSERT_FALSE(t.receivePacket(got));
}

// ------------------------------------------------- timeouts and statistics

static void test_default_interbyte_timeout_is_tolerant_shipped_value(void) {
  FakeSerialPort port;  // 500 us per character
  SerialCMRITransport t(port);
  t.begin();
  // Design v1.1 D13: the shipped/deployment abort default is a tolerant
  // limit (order 100 ms), not the rate-derived conformance value. A
  // legally paced classic node (dH/dL per-character delay, erratum E4)
  // must never trip the Host's own abort; the 250 ms reply gate is the
  // truncation backstop.
  TEST_ASSERT_EQUAL_UINT32(SerialCMRITransport::kShippedInterByteTimeoutMs,
                           t.interByteTimeoutMs());
}

static void test_rate_derived_interbyte_timeout_derives_from_char_time(void) {
  FakeSerialPort port;  // 500 us per character
  SerialCMRITransport t(port);
  // The rate-derived value is the conformance-strict instrument (three
  // character times, interop 2.2.6). It is an explicit opt-in via
  // rateDerivedInterByteTimeoutMs(), never the shipped default (D13).
  // The accessor reads the port's character time, so it works before
  // begin() as well as after.
  TEST_ASSERT_EQUAL_UINT32(2, t.rateDerivedInterByteTimeoutMs());
  t.begin();
  TEST_ASSERT_EQUAL_UINT32(2, t.rateDerivedInterByteTimeoutMs());
}

static void test_shipped_abort_exceeds_rate_derived(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  // D13 invariant: the shipped deployment limit is strictly more
  // tolerant than the rate-derived conformance value, so a paced node
  // never trips the Host's own abort.
  TEST_ASSERT_GREATER_THAN_UINT32(t.rateDerivedInterByteTimeoutMs(),
                                  t.interByteTimeoutMs());
}

static void test_timeout_override_and_disable(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.setInterByteTimeoutMs(0);  // conformance-grade: tolerate any gap
  t.begin();                   // begin() must not clobber the override
  TEST_ASSERT_EQUAL_UINT32(0, t.interByteTimeoutMs());

  const uint8_t head[] = {0xFF, 0xFF, 0x02, 0x46, 0x52};  // frame, no ETX yet
  port.queueRx(head, sizeof(head));
  t.tick(0);
  t.tick(60000);  // an interpreter-scale gap (interop 2.2.6 exception)
  const uint8_t tail[] = {0x03};
  port.queueRx(tail, sizeof(tail));
  t.tick(60001);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(0x46, got.wireUA);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().decodeErrors);
}

static void test_hardware_errors_surface_through_stats(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  port.setHardwareErrorCount(3);  // UART framing/overrun errors
  t.tick(0);
  TEST_ASSERT_EQUAL_UINT32(3, t.stats().decodeErrors);
}

static void test_begin_resets_state(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  const uint8_t garbage[] = {0xFF, 0xFF, 0x02, 0x46};
  port.queueRx(garbage, sizeof(garbage));
  t.tick(1);

  t.begin();  // re-initialize mid-flight
  TEST_ASSERT_EQUAL(2, port.beganCount());
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_FALSE(port.txenAsserted());  // driver released
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().packetsSent);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().decodeErrors);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
}

// ------------------------------------------- gap observability (2.2.6)

// The transport derives the observation floor and suspicion floor from
// the port's character time, mirroring the inter-byte abort derivation.
static void test_default_slow_gap_thresholds_derive_from_char_time(void) {
  FakeSerialPort port;  // 500 us/char
  SerialCMRITransport t(port);
  t.begin();
  // lo = 1 char time (ceil 500 us) = 1 ms; hi = 3 char times = 2 ms.
  TEST_ASSERT_EQUAL_UINT32(1, t.slowGapLoMs());
  TEST_ASSERT_EQUAL_UINT32(2, t.slowGapHiMs());
}

// An explicit override survives begin() (same contract as the abort limit).
static void test_slow_gap_override_survives_begin(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.setSlowGapThresholdsMs(5, 15);
  t.begin();  // must not clobber the override
  TEST_ASSERT_EQUAL_UINT32(5, t.slowGapLoMs());
  TEST_ASSERT_EQUAL_UINT32(15, t.slowGapHiMs());
}

// A gapped frame observed end-to-end through the transport: the first
// intra-frame gap lands in the slow band and surfaces via
// decoderStatistics(), without aborting the exchange.
static void test_slow_gap_observed_end_to_end_through_transport(void) {
  FakeSerialPort port;  // 500 us/char -> lo=1, hi=2 derived
  SerialCMRITransport t(port);
  t.setInterByteTimeoutMs(100);  // raise abort to open the slow band [2,100)
  t.begin();

  const uint8_t body[] = {0x41};
  const CMRIPacket sent = makePacket(5, 'R', body, sizeof(body));
  uint8_t wire[16];
  const size_t n = encodeInto(sent, wire, sizeof(wire));
  // wire = SYN SYN STX UA MT body ETX. Feed STX at t=0, the rest at
  // t=50: the UA byte's gap is 50 ms (slow band), the rest are gapless.
  port.queueRx(wire + 2, 1);      // STX
  t.tick(0);
  port.queueRx(wire + 3, n - 3);  // UA, MT, body, ETX
  t.tick(50);

  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(sent.wireUA, got.wireUA);
  TEST_ASSERT_EQUAL_UINT32(1, t.decoderStatistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(50, t.decoderStatistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(0, t.decoderStatistics().timeoutAborts);
}

// ------------------------------------------------------------------- main

// Phase B/C (issue #104, ADR-0003): echo-cancel characterization
// through the transport against FakeSerialPort. The 2-wire bench
// measured the host's own echo arriving while TXEN is asserted. These
// tests pin the three-state mode (Auto | AlwaysOn | AlwaysOff), the
// discard scope (through kDraining to deassert), and the auto-detect
// mechanism (rxDuringTx signal).

// The shipped default is Auto (the library ships 2-wire-ready).
static void test_echo_cancel_default_auto(void) {
  FakeSerialPort port;
  SerialCMRITransport t(port);
  t.begin();
  TEST_ASSERT_TRUE(t.echoCancelMode() ==
                    SerialCMRITransport::EchoCancelMode::kAuto);
}

// AlwaysOn: RX bytes that arrive while TXEN is asserted (kWriting or
// kDraining) are discarded at the byte level — the decoder never sees
// them. Use a write limit to keep kWriting alive across ticks, then
// let it drain and confirm the discard still holds through deassert.
static void test_echo_cancel_on_discards_rx_through_drain(void) {
  FakeSerialPort port;
  port.setWriteLimit(2);  // accept 2 bytes/call -> kWriting persists
  SerialCMRITransport t(port);
  t.setEchoCancelMode(SerialCMRITransport::EchoCancelMode::kAlwaysOn);
  t.begin();
  t.tick(0);

  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  uint8_t echo[16];
  const size_t n = encodeInto(makePacket(5, 'P'), echo, sizeof(echo));

  // kWriting phase: discard. (tick 1: pumpTransmit_ writes 2,
  // txWritten_=4, still kWriting.)
  port.queueRx(echo, n);
  t.tick(1);

  // kDraining phase (all bytes accepted): discard still holds.
  // tick 2: pumpTransmit_ writes the last 2, txWritten_=6 ->
  // kDraining, then pumpReceive_ discards.
  port.queueRx(echo, n);
  t.tick(2);
  // Do NOT reach tick 3: drainDueMs_ = wireTime(3) + oneCharTime(0) = 3,
  // so the drain completes at tick 3 and pumpReceive_ would feed the
  // decoder in kIdle. Stopping at tick 2 keeps txState_ = kDraining
  // for the assertion.

  // The whole echo was discarded; nothing decoded.
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_UINT32(0, t.decoderStatistics().framesDecoded);
}

// AlwaysOff: the same echo bytes reach the decoder and assemble.
// The mode survives begin(), so the opt-out can be set before begin().
static void test_echo_cancel_off_feeds_rx_while_writing(void) {
  FakeSerialPort port;
  port.setWriteLimit(2);
  SerialCMRITransport t(port);
  t.setEchoCancelMode(SerialCMRITransport::EchoCancelMode::kAlwaysOff);
  t.begin();  // survives: mode stays AlwaysOff
  TEST_ASSERT_TRUE(t.echoCancelMode() ==
                    SerialCMRITransport::EchoCancelMode::kAlwaysOff);
  t.tick(0);

  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  uint8_t echo[16];
  const size_t n = encodeInto(makePacket(5, 'P'), echo, sizeof(echo));
  port.queueRx(echo, n);

  t.tick(1);  // kWriting; AlwaysOff -> decoder fed
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(5 + kWireUAOffset),
                          got.wireUA);
  TEST_ASSERT_EQUAL_HEX8('P', got.mt);
  // rxDuringTx counted the echo bytes (defect visible in AlwaysOff).
  TEST_ASSERT_TRUE(t.rxDuringTx() > 0);
}

// The mid-tick race: drain completes AND echo bytes are in the
// RX buffer in the SAME tick. Without the pendingIdle_ deferral,
// pumpTransmit_ flips txState_ to kIdle before pumpReceive_ runs,
// so pumpReceive_ sees kIdle and feeds the echo to the decoder.
// With the deferral, pumpReceive_ sees kDraining for the whole
// tick and discards the echo. This is the race the bench surfaced.
static void test_echo_cancel_discard_holds_across_drain_complete_tick(void) {
  FakeSerialPort port;  // unlimited write -> kWriting momentary
  SerialCMRITransport t(port);
  t.setEchoCancelMode(SerialCMRITransport::EchoCancelMode::kAlwaysOn);
  t.begin();
  t.tick(0);

  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  // All bytes accepted -> kDraining. Queue the echo and tick at
  // exactly kPollWireMs: drain completes THIS tick, and the echo
  // is in the RX buffer THIS tick. The discard must hold.
  uint8_t echo[16];
  const size_t n = encodeInto(makePacket(5, 'P'), echo, sizeof(echo));
  port.queueRx(echo, n);
  t.tick(kPollWireMs);  // drain-complete + echo in the same tick

  // The echo was discarded (pumpReceive_ saw kDraining, not kIdle).
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_UINT32(0, t.decoderStatistics().framesDecoded);
  // The flip commits within tick() (after pumpReceive_), so
  // sendComplete() is true when tick() returns. The deferral
  // protects pumpReceive_'s mid-tick view, not the caller's.
  TEST_ASSERT_TRUE(t.sendComplete());
}

// Auto: the first RX byte observed while TXEN is asserted arms the
// discard permanently and counts rxDuringTx. Before arming, echo
// bytes reach the decoder (defect visible); after arming, discarded.
static void test_echo_cancel_auto_arms_on_first_rx_during_tx(void) {
  FakeSerialPort port;
  port.setWriteLimit(2);
  SerialCMRITransport t(port);
  t.begin();  // default Auto
  TEST_ASSERT_FALSE(t.echoCancelMode() ==
                    SerialCMRITransport::EchoCancelMode::kAlwaysOn);
  t.tick(0);

  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  uint8_t echo[16];
  const size_t n = encodeInto(makePacket(5, 'P'), echo, sizeof(echo));
  // First echo: arms the discard, counts rxDuringTx, but this tick
  // still feeds the decoder (arm takes effect next tick).
  port.queueRx(echo, n);
  t.tick(1);
  TEST_ASSERT_EQUAL_UINT32(n, t.rxDuringTx());
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));  // decoded this tick
  // Next tick: discard active. Queue more echo; it's discarded.
  port.queueRx(echo, n);
  t.tick(2);
  TEST_ASSERT_FALSE(t.receivePacket(got));
  // rxDuringTx didn't climb (discard ate the second echo).
  TEST_ASSERT_EQUAL_UINT32(n, t.rxDuringTx());
}

// ------------------------------------------------------------------- main

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_reports_send_complete);
  RUN_TEST(test_send_writes_frame_with_txen_discipline);
  RUN_TEST(test_txen_holds_until_port_reports_drained);
  RUN_TEST(test_send_backpressure_while_draining);
  RUN_TEST(test_send_rejects_oversized_body);
  RUN_TEST(test_chunked_write_completes_across_ticks);
  RUN_TEST(test_rx_decodes_whole_validated_frame);
  RUN_TEST(test_rx_truncated_frame_never_delivers);
  RUN_TEST(test_rx_reply_arrives_after_drain);
  RUN_TEST(test_rx_queue_overflow_keeps_oldest_drops_newest);
  RUN_TEST(test_default_interbyte_timeout_is_tolerant_shipped_value);
  RUN_TEST(test_rate_derived_interbyte_timeout_derives_from_char_time);
  RUN_TEST(test_shipped_abort_exceeds_rate_derived);
  RUN_TEST(test_timeout_override_and_disable);
  RUN_TEST(test_hardware_errors_surface_through_stats);
  RUN_TEST(test_begin_resets_state);
  // gap observability (2.2.6 grace-band receive model)
  RUN_TEST(test_default_slow_gap_thresholds_derive_from_char_time);
  RUN_TEST(test_slow_gap_override_survives_begin);
  RUN_TEST(test_slow_gap_observed_end_to_end_through_transport);
  // Phase B/C (issue #104, ADR-0003): echo-cancel characterization
  RUN_TEST(test_echo_cancel_default_auto);
  RUN_TEST(test_echo_cancel_on_discards_rx_through_drain);
  RUN_TEST(test_echo_cancel_discard_holds_across_drain_complete_tick);
  RUN_TEST(test_echo_cancel_off_feeds_rx_while_writing);
  RUN_TEST(test_echo_cancel_auto_arms_on_first_rx_during_tx);
  return UNITY_END();
}
