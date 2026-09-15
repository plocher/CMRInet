// test_tcp_transport.cpp — tests for TcpCMRITransport: carrier liveness,
// packet send/receive, frame encoding/decoding, backpressure, sendRejects,
// receiveDrops, default disabled inter-byte timeout, override timeout,
// poll hook, and error accounting.
//
// Driven against FakeTcpPort, an in-memory scriptable TcpPort double.
// Every timing assertion runs on the injected clock; no test sleeps.

#include <string.h>

#include "CMRInet.h"
#include "transport/tcp.h"
#include "transport/tcpPort.h"
#include "unity.h"

using CMRInet::CMRIPacket;
using CMRInet::TcpPort;
using CMRInet::TcpCMRITransport;
using CMRInet::encodeFrame;
using CMRInet::kWireUAOffset;

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------- fake port

class FakeTcpPort : public TcpPort {
 public:
  static constexpr size_t kBufferCapacity = 2048;

  void begin() override { beganCount_++; }

  bool connected() const override { return connected_; }
  void setConnected(bool c) { connected_ = c; }

  int readByte() override {
    if (!connected_ || rxHead_ >= rxCount_) {
      return -1;
    }
    return rx_[rxHead_++];
  }

  size_t writeBytes(const uint8_t* bytes, size_t length) override {
    if (!connected_ || length == 0) {
      return 0;
    }
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
    }
    return accepted;
  }

  bool transmitDrained() const override { return drained_; }
  void setDrained(bool drained) { drained_ = drained; }

  void poll(uint32_t nowMs) override {
    pollCalls_++;
    lastPollMs_ = nowMs;
  }

  uint32_t socketErrorCount() const override { return socketErrors_; }
  void setSocketErrorCount(uint32_t count) { socketErrors_ = count; }

  // Test controls
  void queueRx(const uint8_t* bytes, size_t length) {
    TEST_ASSERT_TRUE_MESSAGE(rxCount_ + length <= kBufferCapacity,
                             "fake rx buffer overfilled by test");
    memcpy(rx_ + rxCount_, bytes, length);
    rxCount_ += length;
  }

  void setWriteLimit(size_t limit) { writeLimit_ = limit; }
  const uint8_t* txData() const { return tx_; }
  size_t txCount() const { return txCount_; }
  int beganCount() const { return beganCount_; }
  uint32_t pollCalls() const { return pollCalls_; }
  uint32_t lastPollMs() const { return lastPollMs_; }

 private:
  uint8_t rx_[kBufferCapacity] = {0};
  size_t rxHead_ = 0;
  size_t rxCount_ = 0;
  uint8_t tx_[kBufferCapacity] = {0};
  size_t txCount_ = 0;

  size_t writeLimit_ = 0;  // 0 = unlimited
  bool connected_ = true;
  bool drained_ = true;
  uint32_t socketErrors_ = 0;
  int beganCount_ = 0;
  uint32_t pollCalls_ = 0;
  uint32_t lastPollMs_ = 0;
};

// ---------------------------------------------------------------- helpers

static CMRIPacket makePacket(uint8_t addr, uint8_t mt,
                             const uint8_t* body = nullptr, size_t len = 0) {
  CMRIPacket p;
  p.wireUA = static_cast<uint8_t>(addr + kWireUAOffset);
  p.mt = mt;
  TEST_ASSERT_TRUE_MESSAGE(p.setBody(body, len), "setBody rejected test body");
  return p;
}

static size_t encodeInto(const CMRIPacket& p, uint8_t* out, size_t capacity) {
  const size_t n = encodeFrame(p, out, capacity);
  TEST_ASSERT_TRUE_MESSAGE(n > 0, "encodeFrame failed for test frame");
  return n;
}

// ------------------------------------------------------------- test cases

static void test_initial_state_connected(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  TEST_ASSERT_EQUAL(1, port.beganCount());
  TEST_ASSERT_TRUE(t.stats().linkUp);
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().packetsSent);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().packetsReceived);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().sendRejects);
}

static void test_initial_state_disconnected(void) {
  FakeTcpPort port;
  port.setConnected(false);
  TcpCMRITransport t(port);
  t.begin();

  TEST_ASSERT_FALSE(t.stats().linkUp);
  TEST_ASSERT_TRUE(t.sendComplete());
}

static void test_send_rejects_when_link_down(void) {
  FakeTcpPort port;
  port.setConnected(false);
  TcpCMRITransport t(port);
  t.begin();

  CMRIPacket p = makePacket(1, 'P');
  TEST_ASSERT_FALSE(t.sendPacket(p));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().packetsSent);
  TEST_ASSERT_EQUAL_UINT32(0, port.txCount());
}

static void test_send_writes_frame_directly(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  const uint8_t bodyBytes[] = {0x00, 0x10, 0x02};  // 0x10 and 0x02 will be escaped
  CMRIPacket p = makePacket(2, 'T', bodyBytes, sizeof(bodyBytes));

  uint8_t expected[32];
  const size_t expectedLen = encodeInto(p, expected, sizeof(expected));

  TEST_ASSERT_TRUE(t.sendPacket(p));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().packetsSent);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().sendRejects);
  TEST_ASSERT_TRUE(t.sendComplete());

  TEST_ASSERT_EQUAL_UINT32(expectedLen, port.txCount());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, port.txData(), expectedLen);
}

static void test_send_backpressure_and_trickle(void) {
  FakeTcpPort port;
  port.setConnected(true);
  port.setWriteLimit(4);  // only accept 4 bytes per write call
  TcpCMRITransport t(port);
  t.begin();

  CMRIPacket p = makePacket(5, 'P');  // 6 bytes wire frame
  uint8_t expected[16];
  const size_t expectedLen = encodeInto(p, expected, sizeof(expected));
  TEST_ASSERT_EQUAL(6, expectedLen);

  // First write accepts 4 bytes; 2 bytes remain in flight
  TEST_ASSERT_TRUE(t.sendPacket(p));
  TEST_ASSERT_FALSE(t.sendComplete());
  TEST_ASSERT_EQUAL_UINT32(4, port.txCount());

  // Second send while first is in flight is rejected (backpressure)
  CMRIPacket p2 = makePacket(6, 'P');
  TEST_ASSERT_FALSE(t.sendPacket(p2));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().sendRejects);

  // Tick pumps remaining 2 bytes
  t.tick(10);
  TEST_ASSERT_TRUE(t.sendComplete());
  TEST_ASSERT_EQUAL_UINT32(6, port.txCount());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, port.txData(), expectedLen);
}

static void test_carrier_drop_aborts_inflight_send(void) {
  FakeTcpPort port;
  port.setConnected(true);
  port.setWriteLimit(2);
  TcpCMRITransport t(port);
  t.begin();

  CMRIPacket p = makePacket(5, 'P');
  TEST_ASSERT_TRUE(t.sendPacket(p));
  TEST_ASSERT_FALSE(t.sendComplete());

  // Carrier drops
  port.setConnected(false);
  t.tick(10);
  TEST_ASSERT_FALSE(t.stats().linkUp);

  // Reconnect: in-flight state was cleared, not stuck
  port.setConnected(true);
  t.tick(20);
  TEST_ASSERT_TRUE(t.stats().linkUp);
  TEST_ASSERT_TRUE(t.sendComplete());

  // Can send a new packet immediately
  port.setWriteLimit(0);
  TEST_ASSERT_TRUE(t.sendPacket(p));
  TEST_ASSERT_TRUE(t.sendComplete());
}

static void test_receive_single_packet(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  const uint8_t inBody[] = {0x41, 0x42, 0x43};
  CMRIPacket sent = makePacket(3, 'R', inBody, sizeof(inBody));
  uint8_t wire[32];
  const size_t n = encodeInto(sent, wire, sizeof(wire));

  port.queueRx(wire, n);
  t.tick(5);

  CMRIPacket received;
  TEST_ASSERT_TRUE(t.receivePacket(received));
  TEST_ASSERT_EQUAL_UINT8(sent.wireUA, received.wireUA);
  TEST_ASSERT_EQUAL_UINT8('R', received.mt);
  TEST_ASSERT_EQUAL(sizeof(inBody), received.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(inBody, received.body, sizeof(inBody));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().packetsReceived);

  // Queue is now empty
  TEST_ASSERT_FALSE(t.receivePacket(received));
}

static void test_receive_multiple_packets_fifo_order(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  CMRIPacket p1 = makePacket(1, 'I');
  CMRIPacket p2 = makePacket(2, 'T');
  CMRIPacket p3 = makePacket(3, 'P');

  uint8_t wire[64];
  size_t total = 0;
  total += encodeInto(p1, wire + total, sizeof(wire) - total);
  total += encodeInto(p2, wire + total, sizeof(wire) - total);
  total += encodeInto(p3, wire + total, sizeof(wire) - total);

  port.queueRx(wire, total);
  t.tick(10);

  CMRIPacket out;
  TEST_ASSERT_TRUE(t.receivePacket(out));
  TEST_ASSERT_EQUAL_UINT8('I', out.mt);

  TEST_ASSERT_TRUE(t.receivePacket(out));
  TEST_ASSERT_EQUAL_UINT8('T', out.mt);

  TEST_ASSERT_TRUE(t.receivePacket(out));
  TEST_ASSERT_EQUAL_UINT8('P', out.mt);

  TEST_ASSERT_FALSE(t.receivePacket(out));
  TEST_ASSERT_EQUAL_UINT32(3, t.stats().packetsReceived);
}

static void test_receive_queue_drop_on_overflow(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  // kRxQueueCapacity is 4; queue 6 packets
  uint8_t wire[128];
  size_t total = 0;
  for (uint8_t i = 0; i < 6; ++i) {
    CMRIPacket p = makePacket(i + 1, 'P');
    total += encodeInto(p, wire + total, sizeof(wire) - total);
  }

  port.queueRx(wire, total);
  t.tick(10);

  TEST_ASSERT_EQUAL_UINT32(2, t.stats().receiveDrops);

  // Only 4 packets in queue
  for (int i = 0; i < 4; ++i) {
    CMRIPacket out;
    TEST_ASSERT_TRUE(t.receivePacket(out));
  }
  CMRIPacket empty;
  TEST_ASSERT_FALSE(t.receivePacket(empty));
}

static void test_inter_byte_timeout_default_disabled(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  CMRIPacket p = makePacket(4, 'P');
  uint8_t wire[16];
  const size_t n = encodeInto(p, wire, sizeof(wire));

  // Send first 3 bytes at t = 0
  port.queueRx(wire, 3);
  t.tick(0);

  // Huge time gap (10 seconds)
  t.tick(10000);

  // Send remaining 3 bytes at t = 10000
  port.queueRx(wire + 3, n - 3);
  t.tick(10000);

  CMRIPacket out;
  TEST_ASSERT_TRUE(t.receivePacket(out));
  TEST_ASSERT_EQUAL_UINT8('P', out.mt);
  TEST_ASSERT_EQUAL_UINT32(0, t.decoderStatistics().timeoutAborts);
}

static void test_inter_byte_timeout_override(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.setInterByteTimeoutMs(50);
  t.begin();

  TEST_ASSERT_EQUAL_UINT32(50, t.interByteTimeoutMs());

  CMRIPacket p = makePacket(4, 'P');
  uint8_t wire[16];
  const size_t n = encodeInto(p, wire, sizeof(wire));

  // Send first 3 bytes at t = 0
  port.queueRx(wire, 3);
  t.tick(0);

  // Advance time past 50 ms without completing the frame
  t.tick(60);
  TEST_ASSERT_EQUAL_UINT32(1, t.decoderStatistics().timeoutAborts);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().decodeErrors);

  // Trailing bytes cannot complete the expired frame
  port.queueRx(wire + 3, n - 3);
  t.tick(60);

  CMRIPacket out;
  TEST_ASSERT_FALSE(t.receivePacket(out));
}

static void test_poll_hook_and_socket_errors(void) {
  FakeTcpPort port;
  port.setConnected(true);
  TcpCMRITransport t(port);
  t.begin();

  t.tick(42);
  TEST_ASSERT_EQUAL_UINT32(1, port.pollCalls());
  TEST_ASSERT_EQUAL_UINT32(42, port.lastPollMs());

  // Socket error accounting
  port.setSocketErrorCount(3);
  t.tick(50);
  TEST_ASSERT_EQUAL_UINT32(3, t.stats().decodeErrors);
}

// ------------------------------------------------------------- main runner

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_initial_state_connected);
  RUN_TEST(test_initial_state_disconnected);
  RUN_TEST(test_send_rejects_when_link_down);
  RUN_TEST(test_send_writes_frame_directly);
  RUN_TEST(test_send_backpressure_and_trickle);
  RUN_TEST(test_carrier_drop_aborts_inflight_send);
  RUN_TEST(test_receive_single_packet);
  RUN_TEST(test_receive_multiple_packets_fifo_order);
  RUN_TEST(test_receive_queue_drop_on_overflow);
  RUN_TEST(test_inter_byte_timeout_default_disabled);
  RUN_TEST(test_inter_byte_timeout_override);
  RUN_TEST(test_poll_hook_and_socket_errors);
  return UNITY_END();
}
