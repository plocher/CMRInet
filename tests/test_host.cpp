// test_host.cpp — tests for the CMRIHost P/R engine: poll emission,
// reply verification, freshness, health states, statistics, and the
// reply-gate timeout, all against MockCMRITransport and the scripted
// replay rig.
//
// Every timing assertion runs on the injected mock clock. No test
// sleeps or reads a wall clock.

#include <string.h>

#include "CMRInet.h"
#include "unity.h"

using CMRInet::CMRIHost;
using CMRInet::CMRIHostConfig;
using CMRInet::CMRIHostEvent;
using CMRInet::CMRIHostEventType;
using CMRInet::CMRIPacket;
using CMRInet::encodeFrame;
using CMRInet::kUaOffset;
using CMRInet::MockCMRITransport;
using CMRInet::RemoteNodeConfig;
using CMRInet::RemoteNodeHandle;
using CMRInet::RemoteNodeState;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- helpers

/// Build a packet for node address `addr` (UA = addr + 65).
static CMRIPacket makePacket(uint8_t addr, uint8_t mt,
                             const uint8_t* body = nullptr, size_t len = 0) {
  CMRIPacket p;
  p.ua = static_cast<uint8_t>(addr + kUaOffset);
  p.mt = mt;
  TEST_ASSERT_TRUE_MESSAGE(p.setBody(body, len), "setBody rejected test body");
  return p;
}

/// Tick the host once per millisecond over [fromMs, toMs].
static void runUntil(CMRIHost& host, uint32_t fromMs, uint32_t toMs) {
  for (uint32_t t = fromMs; t <= toMs; ++t) {
    host.tick(t);
  }
}

/// One host + mock rig with a single node at address 5, two input bytes.
struct Rig {
  MockCMRITransport transport;
  CMRIHost host;
  RemoteNodeHandle* node = nullptr;

  explicit Rig(const CMRIHostConfig& config = CMRIHostConfig(),
               const CMRIHost::RemoteNodePolicy& policy =
                   CMRIHost::RemoteNodePolicy())
      : host(transport, config) {
    RemoteNodeConfig nodeConfig;
    nodeConfig.inputBytes = 2;
    node = host.addRemoteNode(5, nodeConfig, policy);
    TEST_ASSERT_NOT_NULL_MESSAGE(node, "addRemoteNode failed in rig");
  }
};

static const uint8_t kInputsA5[] = {0xA5, 0x01};

// ------------------------------------------------------------ poll emission

static void test_poll_carries_wire_ua_and_empty_body(void) {
  Rig rig;
  rig.host.begin();
  rig.host.tick(0);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8(5 + kUaOffset, sent.ua);
  TEST_ASSERT_EQUAL_HEX8('P', sent.mt);
  TEST_ASSERT_EQUAL_UINT16(0, sent.length);
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
}

static void test_no_poll_before_begin(void) {
  Rig rig;
  rig.host.tick(0);
  TEST_ASSERT_EQUAL_size_t(0, rig.transport.sentCount());
}

// --------------------------------------------------------- reply acceptance

static void test_reply_commits_inputs_freshness_state_statistics(void) {
  Rig rig;
  rig.host.begin();  // begin() resets the transport: script after it
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->inputByte(1));
  TEST_ASSERT_TRUE(rig.node->inputBit(0));
  TEST_ASSERT_FALSE(rig.node->inputBit(1));
  TEST_ASSERT_TRUE(rig.node->inputBit(2));
  TEST_ASSERT_TRUE(rig.node->inputBit(8));
  TEST_ASSERT_FALSE(rig.node->inputBit(9));
  TEST_ASSERT_FALSE(rig.node->inputBit(999));  // out of range reads false
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().repliesAccepted);
  TEST_ASSERT_TRUE(rig.node->inputAgeMs(2) <= 2);
}

static void test_turnaround_is_measured_from_send_complete(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
      /*delayMs=*/20);
  runUntil(rig.host, 0, 20);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(20, rig.node->statistics().lastTurnaroundMs);
}

// -------------------------------------------------------- reply-gate timeout

static void test_default_reply_gate_is_250ms(void) {
  Rig rig;  // no script: the node stays silent
  rig.host.begin();
  runUntil(rig.host, 0, 249);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  rig.host.tick(250);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().consecutiveMisses);
}

static void test_policy_overrides_reply_gate(void) {
  CMRIHost::RemoteNodePolicy policy;
  policy.replyTimeoutMs = 50;
  Rig rig(CMRIHostConfig(), policy);
  rig.host.begin();
  runUntil(rig.host, 0, 49);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  rig.host.tick(50);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
}

static void test_reply_gate_opens_at_send_complete_not_accept(void) {
  CMRIHost::RemoteNodePolicy policy;
  policy.replyTimeoutMs = 50;
  Rig rig(CMRIHostConfig(), policy);
  rig.transport.setSendLatencyMs(5);
  rig.host.begin();
  runUntil(rig.host, 0, 54);  // accept at 0, complete at 5, gate due 55
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  rig.host.tick(55);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
}

// ------------------------------------------------------- reply verification

static void test_wrong_ua_reply_is_rejected(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 250);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_TRUE(rig.host.statistics().repliesRejected >= 1);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
}

static void test_wrong_mt_reply_is_rejected(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.onSendReplyPacket(5 + kUaOffset, 'P', makePacket(5, 'E'));
  runUntil(rig.host, 0, 250);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_TRUE(rig.host.statistics().repliesRejected >= 1);
}

static void test_wrong_length_reply_counts_error_without_commit(void) {
  Rig rig;  // node expects 2 input bytes
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)));
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().errors);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_HEX8(0x00, rig.node->inputByte(0));  // never committed
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
}

// --------------------------------------------------- miss, recovery, health

static void test_recovery_after_miss(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 1);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 300);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().recoveries);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().consecutiveMisses);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

static void test_offline_after_miss_threshold_then_recovers(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 6);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  // Six miss cycles of (250 ms gate + 5 ms pacing).
  runUntil(rig.host, 0, 1530);
  TEST_ASSERT_EQUAL_UINT32(6, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, rig.node->state());
  // The silent node is still polled, and the next reply recovers it.
  // VALIDATION: Interop v1.1 2.3.10: keep polling a silent Node
  // forever.
  runUntil(rig.host, 1531, 1600);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().recoveries);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

static void test_stale_when_inputs_outlive_threshold(void) {
  CMRIHostConfig config;
  config.missThreshold = 1000;  // keep OFFLINE out of this test
  Rig rig(config);
  RemoteNodeConfig staleConfig;
  staleConfig.inputBytes = 2;
  staleConfig.stalenessMs = 100;
  RemoteNodeHandle* node = rig.host.addRemoteNode(6, staleConfig);
  TEST_ASSERT_NOT_NULL(node);
  rig.node->setEnabled(false);  // only the staleness node is polled
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      6 + kUaOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  runUntil(rig.host, 3, 200);  // silence: the image ages past 100 ms
  TEST_ASSERT_EQUAL(RemoteNodeState::kStale, node->state());
}

// ------------------------------------------------------- schedule discipline

static void test_pacing_gap_between_exchanges(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      MockCMRITransport::kRepeatForever);
  runUntil(rig.host, 0, 1);  // poll at 0, reply accepted at 1
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
  runUntil(rig.host, 2, 5);  // pacing gate holds until 1 + 5 ms
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
  rig.host.tick(6);
  TEST_ASSERT_EQUAL_UINT32(2, rig.host.statistics().pollsSent);
}

static void test_round_robin_over_enabled_nodes(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;
  config.inputBytes = 0;
  TEST_ASSERT_NOT_NULL(host.addRemoteNode(1, config));
  TEST_ASSERT_NOT_NULL(host.addRemoteNode(2, config));
  host.begin();
  transport.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                              makePacket(1, 'R'), 0, 1);
  transport.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                              makePacket(2, 'R'), 0, 1);
  runUntil(host, 0, 20);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8(1 + kUaOffset, sent.ua);
  TEST_ASSERT_TRUE(transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8(2 + kUaOffset, sent.ua);
}

static void test_send_refusal_retries_without_blocking(void) {
  Rig rig;
  rig.host.begin();
  rig.transport.setLinkUp(false);
  runUntil(rig.host, 0, 3);
  TEST_ASSERT_EQUAL_UINT32(0, rig.host.statistics().pollsSent);
  TEST_ASSERT_TRUE(rig.host.statistics().pollSendRetries >= 1);
  rig.transport.setLinkUp(true);
  rig.host.tick(4);
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
}

static void test_unsolicited_packets_are_counted(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);  // no nodes: the schedule stays idle
  host.begin();
  host.tick(0);
  TEST_ASSERT_TRUE(transport.injectPacket(makePacket(9, 'R')));
  host.tick(1);
  TEST_ASSERT_EQUAL_UINT32(1, host.statistics().unsolicitedPackets);
}

// ------------------------------------------------------- byte-level replay

static void test_exchange_completes_over_gapped_wire_bytes(void) {
  Rig rig;
  const CMRIPacket reply = makePacket(5, 'R', kInputsA5, sizeof(kInputsA5));
  uint8_t wire[64];
  const size_t n = encodeFrame(reply, wire, sizeof(wire));
  TEST_ASSERT_TRUE(n > 0);
  rig.host.begin();
  rig.transport.onSendReplyBytes(5 + kUaOffset, 'P', wire, n, /*delayMs=*/1,
                                 /*repeat=*/1, /*interByteGapMs=*/2);
  runUntil(rig.host, 0, 40);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

// ----------------------------------------------------- listener seam (D7)

/// Context cookie for the recording listeners below.
struct ListenerLog {
  int accepted = 0;
  int rejected = 0;
  int timeouts = 0;
  int stateChanges = 0;
  RemoteNodeState lastPreviousState = RemoteNodeState::kUninitialized;
  RemoteNodeState lastNewState = RemoteNodeState::kUninitialized;
  const RemoteNodeHandle* lastNode = nullptr;
  uint32_t lastEventMs = 0;
  int txTraces = 0;
  int rxTraces = 0;
  uint8_t lastTxMt = 0;
  uint8_t lastRxMt = 0;
};

static void recordEvent(void* context, const CMRIHostEvent& event) {
  ListenerLog& log = *static_cast<ListenerLog*>(context);
  switch (event.type) {
    case CMRIHostEventType::kReplyAccepted: ++log.accepted; break;
    case CMRIHostEventType::kReplyRejected: ++log.rejected; break;
    case CMRIHostEventType::kReplyTimeout: ++log.timeouts; break;
    case CMRIHostEventType::kNodeStateChanged:
      ++log.stateChanges;
      log.lastPreviousState = event.previousState;
      log.lastNewState = event.newState;
      break;
  }
  log.lastNode = event.node;
  log.lastEventMs = event.nowMs;
}

static void recordTrace(void* context, bool transmit,
                        const CMRIPacket& packet) {
  ListenerLog& log = *static_cast<ListenerLog*>(context);
  if (transmit) {
    ++log.txTraces;
    log.lastTxMt = packet.mt;
  } else {
    ++log.rxTraces;
    log.lastRxMt = packet.mt;
  }
}

static void test_event_listener_sees_accept_and_state_change(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_INT(1, log.accepted);
  TEST_ASSERT_EQUAL_PTR(rig.node, log.lastNode);
  // kUninitialized -> kOnline fired exactly once.
  TEST_ASSERT_EQUAL_INT(1, log.stateChanges);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, log.lastPreviousState);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, log.lastNewState);
}

static void test_event_listener_sees_timeout_and_offline_transition(void) {
  Rig rig;  // no script: the node stays silent
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  rig.host.begin();
  // Six miss cycles of (250 ms gate + 5 ms pacing) cross the threshold.
  runUntil(rig.host, 0, 1530);
  TEST_ASSERT_EQUAL_INT(6, log.timeouts);
  TEST_ASSERT_EQUAL_INT(1, log.stateChanges);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, log.lastPreviousState);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, log.lastNewState);
}

static void test_event_listener_sees_rejected_reply(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_INT(1, log.rejected);
  TEST_ASSERT_EQUAL_INT(0, log.accepted);
}

static void test_trace_listener_sees_both_directions(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onTrace(recordTrace, &log);
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_INT(1, log.txTraces);
  TEST_ASSERT_EQUAL_HEX8('P', log.lastTxMt);
  TEST_ASSERT_EQUAL_INT(1, log.rxTraces);
  TEST_ASSERT_EQUAL_HEX8('R', log.lastRxMt);
}

static void test_listener_registration_locked_after_begin(void) {
  Rig rig;
  ListenerLog log;
  rig.host.begin();
  rig.host.onEvent(recordEvent, &log);  // ignored: configuration is locked
  rig.host.onTrace(recordTrace, &log);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_INT(0, log.accepted);
  TEST_ASSERT_EQUAL_INT(0, log.txTraces);
  TEST_ASSERT_EQUAL_INT(0, log.rxTraces);
}

static void test_null_listeners_are_harmless(void) {
  Rig rig;  // default: no listeners registered
  rig.host.onEvent(nullptr);
  rig.host.onTrace(nullptr);
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, 0, 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
}

// ------------------------------------------------------ configuration phase

static void test_add_remote_node_validation(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;
  config.inputBytes = 2;

  TEST_ASSERT_NULL(host.addRemoteNode(128, config));  // address out of range

  RemoteNodeConfig oversized;
  oversized.inputBytes = RemoteNodeHandle::kMaxInputBytes + 1;
  TEST_ASSERT_NULL(host.addRemoteNode(5, oversized));

  TEST_ASSERT_NOT_NULL(host.addRemoteNode(5, config));
  TEST_ASSERT_NULL(host.addRemoteNode(5, config));  // duplicate address

  host.begin();
  TEST_ASSERT_NULL(host.addRemoteNode(6, config));  // locked after begin()
  TEST_ASSERT_EQUAL_size_t(1, host.nodeCount());
}

static void test_node_table_capacity_is_enforced(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;
  config.inputBytes = 0;
  for (size_t i = 0; i < CMRIHost::kMaxNodes; ++i) {
    TEST_ASSERT_NOT_NULL(host.addRemoteNode(static_cast<uint8_t>(i), config));
  }
  TEST_ASSERT_NULL(host.addRemoteNode(
      static_cast<uint8_t>(CMRIHost::kMaxNodes), config));
}

// ----------------------------------------------------------------- runner

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_poll_carries_wire_ua_and_empty_body);
  RUN_TEST(test_no_poll_before_begin);
  RUN_TEST(test_reply_commits_inputs_freshness_state_statistics);
  RUN_TEST(test_turnaround_is_measured_from_send_complete);
  RUN_TEST(test_default_reply_gate_is_250ms);
  RUN_TEST(test_policy_overrides_reply_gate);
  RUN_TEST(test_reply_gate_opens_at_send_complete_not_accept);
  RUN_TEST(test_wrong_ua_reply_is_rejected);
  RUN_TEST(test_wrong_mt_reply_is_rejected);
  RUN_TEST(test_wrong_length_reply_counts_error_without_commit);
  RUN_TEST(test_recovery_after_miss);
  RUN_TEST(test_offline_after_miss_threshold_then_recovers);
  RUN_TEST(test_stale_when_inputs_outlive_threshold);
  RUN_TEST(test_pacing_gap_between_exchanges);
  RUN_TEST(test_round_robin_over_enabled_nodes);
  RUN_TEST(test_send_refusal_retries_without_blocking);
  RUN_TEST(test_unsolicited_packets_are_counted);
  RUN_TEST(test_exchange_completes_over_gapped_wire_bytes);
  RUN_TEST(test_event_listener_sees_accept_and_state_change);
  RUN_TEST(test_event_listener_sees_timeout_and_offline_transition);
  RUN_TEST(test_event_listener_sees_rejected_reply);
  RUN_TEST(test_trace_listener_sees_both_directions);
  RUN_TEST(test_listener_registration_locked_after_begin);
  RUN_TEST(test_null_listeners_are_harmless);
  RUN_TEST(test_add_remote_node_validation);
  RUN_TEST(test_node_table_capacity_is_enforced);
  return UNITY_END();
}
