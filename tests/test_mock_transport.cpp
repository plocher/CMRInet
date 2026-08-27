// test_mock_transport.cpp — tests for MockCMRITransport: the transport
// seam behavior, packet and byte injection, pathological byte replay,
// and the scripted replay rig.
//
// Every timing assertion runs on the injected mock clock. No test
// sleeps or reads a wall clock.

#include <string.h>

#include "CMRInet.h"
#include "unity.h"

using CMRInet::CMRIPacket;
using CMRInet::encodeFrame;
using CMRInet::kMaxBody;
using CMRInet::kMaxWireFrame;
using CMRInet::kWireUAOffset;
using CMRInet::MockCMRITransport;

void setUp(void) {}
void tearDown(void) {}

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

// ---------------------------------------------------------- transport seam

static void test_send_accepts_and_logs(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendComplete());  // idle reports complete
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_TRUE(t.sendComplete());  // zero latency: immediate
  TEST_ASSERT_EQUAL_size_t(1, t.sentCount());
  CMRIPacket sent;
  TEST_ASSERT_TRUE(t.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8(0x46, sent.wireUA);
  TEST_ASSERT_EQUAL_HEX8('P', sent.mt);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().packetsSent);
}

static void test_send_latency_gates_completion(void) {
  MockCMRITransport t;
  t.begin();
  t.setSendLatencyMs(5);
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_FALSE(t.sendComplete());
  t.tick(4);
  TEST_ASSERT_FALSE(t.sendComplete());
  t.tick(5);
  TEST_ASSERT_TRUE(t.sendComplete());
}

static void test_send_backpressure_while_in_flight(void) {
  MockCMRITransport t;
  t.begin();
  t.setSendLatencyMs(5);
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_FALSE(t.sendPacket(makePacket(6, 'P')));  // still draining
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);
  t.tick(5);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(6, 'P')));
}

static void test_send_refused_while_link_down(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  t.setLinkUp(false);
  TEST_ASSERT_FALSE(t.stats().linkUp);
  TEST_ASSERT_FALSE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);
  t.setLinkUp(true);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
}

static void test_send_rejects_oversized_body(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  CMRIPacket p = makePacket(5, 'T');
  p.length = static_cast<uint16_t>(kMaxBody + 1);  // forge a bad length
  TEST_ASSERT_FALSE(t.sendPacket(p));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);
}

// -------------------------------------------------------- packet injection

static void test_inject_packet_delivers_in_fifo_order(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  TEST_ASSERT_TRUE(t.injectPacket(makePacket(1, 'R')));
  TEST_ASSERT_TRUE(t.injectPacket(makePacket(2, 'R')));
  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(1 + kWireUAOffset, got.wireUA);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(2 + kWireUAOffset, got.wireUA);
  TEST_ASSERT_FALSE(t.receivePacket(got));  // at most one per call, then empty
  TEST_ASSERT_EQUAL_UINT32(2, t.stats().packetsReceived);
}

static void test_inject_packet_at_future_time(void) {
  MockCMRITransport t;
  t.begin();
  TEST_ASSERT_TRUE(t.injectPacketAt(makePacket(5, 'R'), 50));
  t.tick(49);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  t.tick(50);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
}

static void test_rx_overflow_keeps_oldest_drops_newest(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  for (uint8_t i = 0; i < MockCMRITransport::kRxQueueCapacity + 1; ++i) {
    TEST_ASSERT_TRUE(t.injectPacket(makePacket(i, 'R')));
  }
  t.tick(0);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().receiveDrops);
  CMRIPacket got;
  for (uint8_t i = 0; i < MockCMRITransport::kRxQueueCapacity; ++i) {
    TEST_ASSERT_TRUE(t.receivePacket(got));
    TEST_ASSERT_EQUAL_HEX8(i + kWireUAOffset, got.wireUA);  // arrival order held
  }
  TEST_ASSERT_FALSE(t.receivePacket(got));
}

// Delivery order equals injection order, regardless of due times: the
// head event gates everything behind it.
static void test_event_order_is_injection_order(void) {
  MockCMRITransport t;
  t.begin();
  TEST_ASSERT_TRUE(t.injectPacketAt(makePacket(1, 'R'), 100));
  TEST_ASSERT_TRUE(t.injectPacketAt(makePacket(2, 'R'), 0));
  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));  // head (due 100) gates both
  t.tick(100);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(1 + kWireUAOffset, got.wireUA);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(2 + kWireUAOffset, got.wireUA);
}

// ---------------------------------------------------------- byte injection

static void test_inject_bytes_decodes_whole_frame(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  const uint8_t body[] = {0x00, 0x02, 0xFF, 0x10, 0x41};
  const CMRIPacket sent = makePacket(23, 'R', body, sizeof(body));
  uint8_t wire[64];
  const size_t n = encodeInto(sent, wire, sizeof(wire));
  TEST_ASSERT_TRUE(t.injectBytes(wire, n));
  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8(sent.wireUA, got.wireUA);
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
  TEST_ASSERT_EQUAL_UINT16(sizeof(body), got.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got.body, sizeof(body));
}

static void test_truncated_frame_never_delivers(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  const uint8_t truncated[] = {0xFF, 0xFF, 0x02, 0x46, 0x52};  // no ETX
  TEST_ASSERT_TRUE(t.injectBytes(truncated, sizeof(truncated)));
  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  // Line silence past the inter-byte timeout abandons the partial frame.
  t.tick(100);
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_TRUE(t.stats().decodeErrors > 0);
}

static void test_dangling_dle_counts_and_never_delivers(void) {
  MockCMRITransport t;
  t.begin();
  t.tick(0);
  const uint8_t danglingDle[] = {0x02, 0x46, 0x52, 0x10};  // dies escaped
  TEST_ASSERT_TRUE(t.injectBytes(danglingDle, sizeof(danglingDle)));
  t.tick(0);
  t.tick(100);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_UINT32(1, t.decoderStatistics().danglingDle);
}

// A transmitter slower than the receiver's inter-byte timeout loses the
// frame. The per-byte timestamps replay the gaps exactly.
static void test_gapped_bytes_beyond_timeout_abandon_frame(void) {
  MockCMRITransport t;
  t.begin();
  t.setDecoderInterByteTimeoutMs(20);
  uint8_t wire[16];
  const size_t n = encodeInto(makePacket(5, 'P'), wire, sizeof(wire));
  TEST_ASSERT_TRUE(t.injectBytesAt(wire, n, 0, 30));  // 30 ms gaps
  t.tick(1000);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_TRUE(t.stats().decodeErrors > 0);
}

static void test_gapped_bytes_within_timeout_decode(void) {
  MockCMRITransport t;
  t.begin();
  t.setDecoderInterByteTimeoutMs(20);
  uint8_t wire[16];
  const size_t n = encodeInto(makePacket(5, 'P'), wire, sizeof(wire));
  TEST_ASSERT_TRUE(t.injectBytesAt(wire, n, 0, 10));  // 10 ms gaps
  t.tick(1000);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('P', got.mt);
}

// A timeout of 0 disables the gap check entirely: a conformance-grade
// receiver tolerates arbitrarily slow transmitters.
static void test_disabled_timeout_tolerates_any_gap(void) {
  MockCMRITransport t;
  t.begin();
  t.setDecoderInterByteTimeoutMs(0);
  uint8_t wire[16];
  const size_t n = encodeInto(makePacket(5, 'P'), wire, sizeof(wire));
  TEST_ASSERT_TRUE(t.injectBytesAt(wire, n, 0, 1000));  // 1 s per byte
  t.tick(10000);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('P', got.mt);
}

// ---------------------------------------------------------- replay script

static void test_script_replies_after_delay(void) {
  MockCMRITransport t;
  t.begin();
  const uint8_t inputs[] = {0xA5, 0x00};
  TEST_ASSERT_TRUE(t.onSendReplyPacket(
      0x46, 'P', makePacket(5, 'R', inputs, sizeof(inputs)), 10));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  t.tick(9);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));  // reply not due yet
  t.tick(10);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
  TEST_ASSERT_EQUAL_UINT16(sizeof(inputs), got.length);
  TEST_ASSERT_EQUAL_size_t(0, t.scriptRemaining());
}

static void test_script_mismatch_counts_and_keeps_step(void) {
  MockCMRITransport t;
  t.begin();
  TEST_ASSERT_TRUE(t.onSendReplyPacket(0x46, 'P', makePacket(5, 'R')));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'T')));  // not the expected P
  TEST_ASSERT_EQUAL_UINT32(1, t.scriptMismatches());
  TEST_ASSERT_EQUAL_size_t(1, t.scriptRemaining());
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));  // now it matches
  t.tick(0);
  CMRIPacket got;
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
}

static void test_script_silence_consumes_match(void) {
  MockCMRITransport t;
  t.begin();
  TEST_ASSERT_TRUE(t.onSendStaySilent(MockCMRITransport::kMatchAny, 'P'));
  TEST_ASSERT_TRUE(t.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                                       makePacket(5, 'R')));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));  // eaten by silence
  t.tick(1);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_size_t(1, t.scriptRemaining());
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));  // reply step fires
  t.tick(2);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
}

static void test_script_silence_repeat_models_dead_node(void) {
  MockCMRITransport t;
  t.begin();
  TEST_ASSERT_TRUE(t.onSendStaySilent(MockCMRITransport::kMatchAny, 'P', 2));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_EQUAL_size_t(1, t.scriptRemaining());
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_EQUAL_size_t(0, t.scriptRemaining());
}

static void test_script_repeat_forever_answers_every_poll(void) {
  MockCMRITransport t;
  t.begin();
  TEST_ASSERT_TRUE(t.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                                       makePacket(5, 'R'), 0,
                                       MockCMRITransport::kRepeatForever));
  for (uint32_t i = 0; i < 3; ++i) {
    t.tick(i * 10);
    TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
    t.tick(i * 10 + 1);
    CMRIPacket got;
    TEST_ASSERT_TRUE(t.receivePacket(got));
    TEST_ASSERT_EQUAL_HEX8('R', got.mt);
  }
  TEST_ASSERT_EQUAL_size_t(1, t.scriptRemaining());  // never retires
}

// A scripted byte reply runs through the real decoder: a pathological
// reply (no ETX) produces decode errors, never a packet.
static void test_script_bytes_reply_can_be_pathological(void) {
  MockCMRITransport t;
  t.begin();
  const uint8_t truncated[] = {0xFF, 0xFF, 0x02, 0x46, 0x52, 0xA5};  // no ETX
  TEST_ASSERT_TRUE(t.onSendReplyBytes(MockCMRITransport::kMatchAny, 'P',
                                      truncated, sizeof(truncated)));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  t.tick(1);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
  t.tick(100);  // silence expires the partial frame
  TEST_ASSERT_FALSE(t.receivePacket(got));
  TEST_ASSERT_TRUE(t.stats().decodeErrors > 0);
}

// The reply timer starts at send completion, not send acceptance:
// reply due = accept time + send latency + scripted delay.
static void test_script_fires_at_send_completion(void) {
  MockCMRITransport t;
  t.begin();
  t.setSendLatencyMs(5);
  TEST_ASSERT_TRUE(t.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                                       makePacket(5, 'R'), 3));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  t.tick(7);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));  // 5 + 3 = 8: not yet
  t.tick(8);
  TEST_ASSERT_TRUE(t.receivePacket(got));
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
}

// ------------------------------------------------------------------ begin

static void test_begin_resets_all_state(void) {
  MockCMRITransport t;
  t.begin();
  t.setSendLatencyMs(5);
  TEST_ASSERT_TRUE(t.onSendStaySilent(MockCMRITransport::kMatchAny, 'P'));
  t.tick(0);
  TEST_ASSERT_TRUE(t.sendPacket(makePacket(5, 'P')));
  TEST_ASSERT_TRUE(t.injectPacket(makePacket(6, 'R')));
  t.begin();  // full reset
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_EQUAL_size_t(0, t.sentCount());
  TEST_ASSERT_EQUAL_size_t(0, t.scriptRemaining());
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().packetsSent);
  t.tick(1000);
  CMRIPacket got;
  TEST_ASSERT_FALSE(t.receivePacket(got));
}

// ------------------------------------------------------------------- main

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_send_accepts_and_logs);
  RUN_TEST(test_send_latency_gates_completion);
  RUN_TEST(test_send_backpressure_while_in_flight);
  RUN_TEST(test_send_refused_while_link_down);
  RUN_TEST(test_send_rejects_oversized_body);
  RUN_TEST(test_inject_packet_delivers_in_fifo_order);
  RUN_TEST(test_inject_packet_at_future_time);
  RUN_TEST(test_rx_overflow_keeps_oldest_drops_newest);
  RUN_TEST(test_event_order_is_injection_order);
  RUN_TEST(test_inject_bytes_decodes_whole_frame);
  RUN_TEST(test_truncated_frame_never_delivers);
  RUN_TEST(test_dangling_dle_counts_and_never_delivers);
  RUN_TEST(test_gapped_bytes_beyond_timeout_abandon_frame);
  RUN_TEST(test_gapped_bytes_within_timeout_decode);
  RUN_TEST(test_disabled_timeout_tolerates_any_gap);
  RUN_TEST(test_script_replies_after_delay);
  RUN_TEST(test_script_mismatch_counts_and_keeps_step);
  RUN_TEST(test_script_silence_consumes_match);
  RUN_TEST(test_script_silence_repeat_models_dead_node);
  RUN_TEST(test_script_repeat_forever_answers_every_poll);
  RUN_TEST(test_script_bytes_reply_can_be_pathological);
  RUN_TEST(test_script_fires_at_send_completion);
  RUN_TEST(test_begin_resets_all_state);
  return UNITY_END();
}
