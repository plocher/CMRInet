// test_node.cpp — desktop loopback integration test for CMRINode.
//
// Proves the Host ↔ Node round trip over paired mock
// transports. Every timing runs on the injected mock clock.

#include <string.h>

#include "CMRInet.h"
#include "unity.h"

using CMRInet::CMRIHost;
using CMRInet::CMRINode;
using CMRInet::CMRINodeConfig;
using CMRInet::CMRIHostConfig;
using CMRInet::CMRIPacket;
using CMRInet::CMRITransport;
using CMRInet::MockCMRITransport;
using CMRInet::RemoteNodeConfig;
using CMRInet::kWireUAOffset;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- helpers

static CMRIPacket makePacket(uint8_t addr, uint8_t mt,
                               const uint8_t* body = nullptr,
                               size_t len = 0) {
  CMRIPacket p;
  p.wireUA = static_cast<uint8_t>(addr + kWireUAOffset);
  p.mt = mt;
  TEST_ASSERT_TRUE_MESSAGE(p.setBody(body, len), "setBody rejected test body");
  return p;
}

// ---------------------------------------------------------------- handlers

struct UnpackLog {
  int calls = 0;
  uint8_t ob[16] = {0};
  size_t len = 0;
};

static void recordUnpack(void* ctx, const uint8_t* ob, size_t len) {
  UnpackLog& log = *static_cast<UnpackLog*>(ctx);
  log.calls++;
  log.len = len;
  if (len > sizeof(log.ob)) len = sizeof(log.ob);
  memcpy(log.ob, ob, len);
}

static void fillPack(void* ctx, uint8_t* ib, size_t len) {
  (void)ctx;
  if (len > 0) {
    memset(ib, 0xA5, len);
  }
}

// ---------------------------------------------------------------- rig

struct LoopbackRig {
  MockCMRITransport hostTransport;
  MockCMRITransport nodeTransport;
  CMRIHost host;
  CMRINode node;
  UnpackLog unpackLog;

  LoopbackRig()
      : host(hostTransport),
        node(nodeTransport, makeNodeConfig()) {
    RemoteNodeConfig nodeConfig;
    nodeConfig.inputBytes = 2;
    nodeConfig.outputBytes = 3;
    host.addRemoteNode(5, nodeConfig);
    node.unpack(recordUnpack, &unpackLog);
  }

  static CMRINodeConfig makeNodeConfig() {
    CMRINodeConfig cfg;
    cfg.ua = 5;
    cfg.inputBytes = 2;
    cfg.outputBytes = 3;
    return cfg;
  }
};

static void tickBoth(LoopbackRig& rig, uint32_t fromMs, uint32_t toMs) {
  for (uint32_t t = fromMs; t <= toMs; ++t) {
    rig.host.tick(t);
    rig.node.tick(t);
  }
}

static void bridgeAndRun(LoopbackRig& rig, uint32_t fromMs, uint32_t toMs) {
  for (uint32_t t = fromMs; t <= toMs; ++t) {
    rig.host.tick(t);
    rig.node.tick(t);
    CMRIPacket sent;
    while (rig.hostTransport.takeSent(sent)) {
      rig.nodeTransport.injectPacket(sent);
    }
    while (rig.nodeTransport.takeSent(sent)) {
      rig.hostTransport.injectPacket(sent);
    }
  }
}

static uint32_t primeToPoll(LoopbackRig& rig) {
  rig.host.begin();
  rig.node.begin();
  bridgeAndRun(rig, 0, 507);
  CMRIPacket scratch;
  while (rig.hostTransport.takeSent(scratch)) { }
  while (rig.nodeTransport.takeSent(scratch)) { }
  return 508;
}

// ---------------------------------------------------------------- tests

static void test_init_reaches_node(void) {
  LoopbackRig rig;
  rig.host.begin();
  rig.node.begin();
  tickBoth(rig, 0, 1);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.hostTransport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('I', sent.mt);
  TEST_ASSERT_EQUAL_HEX8('C', sent.body[0]);
  TEST_ASSERT_EQUAL_UINT32(0, rig.nodeTransport.sentCount());
}

static void test_transmit_reaches_node(void) {
  LoopbackRig rig;
  uint32_t base = primeToPoll(rig);
  rig.unpackLog = UnpackLog();
  const uint8_t out[] = {0xDE, 0xAD, 0xBE};
  rig.host.node(5)->setOutputs(out, 3);
  rig.host.node(5)->forceTransmit();
  tickBoth(rig, base, base + 5);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.hostTransport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('T', sent.mt);
  rig.nodeTransport.injectPacket(sent);
  rig.node.tick(base + 6);
  TEST_ASSERT_EQUAL_INT(1, rig.unpackLog.calls);
  TEST_ASSERT_EQUAL_size_t(3, rig.unpackLog.len);
  TEST_ASSERT_EQUAL_HEX8(0xDE, rig.unpackLog.ob[0]);
  TEST_ASSERT_EQUAL_HEX8(0xAD, rig.unpackLog.ob[1]);
  TEST_ASSERT_EQUAL_HEX8(0xBE, rig.unpackLog.ob[2]);
}

static void test_poll_reply_round_trip(void) {
  LoopbackRig rig;
  rig.node.pack(fillPack, nullptr);
  uint32_t base = primeToPoll(rig);
  tickBoth(rig, base, base + 1);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.hostTransport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('P', sent.mt);
  rig.nodeTransport.injectPacket(sent);
  tickBoth(rig, base + 2, base + 3);
  CMRIPacket r;
  while (rig.nodeTransport.takeSent(r)) {
    rig.hostTransport.injectPacket(r);
  }
  rig.host.tick(base + 4);
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().repliesAccepted);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.host.node(5)->inputByte(0));
}

static void test_full_loopback(void) {
  LoopbackRig rig;
  rig.node.pack(fillPack, nullptr);
  uint32_t base = primeToPoll(rig);
  const uint8_t out[] = {0x01, 0x02, 0x03};
  rig.host.node(5)->setOutputs(out, 3);
  rig.host.node(5)->forceTransmit();
  bridgeAndRun(rig, base, base + 10);
  CMRIPacket scratch;
  while (rig.hostTransport.takeSent(scratch)) { }
  while (rig.nodeTransport.takeSent(scratch)) { }
  TEST_ASSERT_TRUE(rig.host.statistics().repliesAccepted >= 1);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.host.node(5)->inputByte(0));
  TEST_ASSERT_TRUE(rig.unpackLog.calls >= 1);
}

static void test_ua_mismatch_discarded(void) {
  LoopbackRig rig;
  rig.node.begin();
  rig.host.begin();
  rig.nodeTransport.injectPacket(makePacket(6, 'P'));
  bridgeAndRun(rig, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(0, rig.nodeTransport.sentCount());
}

static void test_ndp_mismatch_discarded(void) {
  LoopbackRig rig;
  rig.node.begin();
  rig.host.begin();
  rig.nodeTransport.injectPacket(makePacket(5, 'I',
      reinterpret_cast<const uint8_t*>("X"), 1));
  bridgeAndRun(rig, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(0, rig.nodeTransport.sentCount());
}

static void test_set_input_bit(void) {
  LoopbackRig rig;
  rig.node.setInputBit(0, 0, true);   // byte 0, bit 0
  rig.node.setInputBit(1, 7, true);    // byte 1, bit 7
  TEST_ASSERT_TRUE(rig.node.inputBit(0, 0));
  TEST_ASSERT_TRUE(rig.node.inputBit(1, 7));
  TEST_ASSERT_FALSE(rig.node.inputBit(0, 1));   // byte 0, bit 1
  TEST_ASSERT_FALSE(rig.node.inputBit(1, 0));    // byte 1, bit 0
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_init_reaches_node);
  RUN_TEST(test_transmit_reaches_node);
  RUN_TEST(test_poll_reply_round_trip);
  RUN_TEST(test_full_loopback);
  RUN_TEST(test_ua_mismatch_discarded);
  RUN_TEST(test_ndp_mismatch_discarded);
  RUN_TEST(test_set_input_bit);
  return UNITY_END();
}
