// test_host.cpp — tests for the CMRIHost engine: I/T/P scheduling, reply
// verification, freshness, health states, statistics, the reply-gate
// timeout, the re-init ladder, input invalidation, E8 unsolicited handling,
// optional T refresh, and the D7 listener seam — all against MockCMRITransport
// and the scripted replay rig.
//
// Every timing assertion runs on the injected mock clock. No test sleeps or
// reads a wall clock.

#include <string.h>

#include "CMRInet.h"
#include "unity.h"

using CMRInet::Age;
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

/// One host + mock rig with a single node at address 5.
struct Rig {
  MockCMRITransport transport;
  CMRIHost host;
  RemoteNodeHandle* node = nullptr;

  explicit Rig(const CMRIHostConfig& config = CMRIHostConfig(),
               const CMRIHost::RemoteNodePolicy& policy =
                   CMRIHost::RemoteNodePolicy(),
               uint16_t inputBytes = 2,
               uint16_t outputBytes = 0)
      : host(transport, config) {
    RemoteNodeConfig nodeConfig;
    nodeConfig.inputBytes = inputBytes;
    nodeConfig.outputBytes = outputBytes;
    node = host.addRemoteNode(5, nodeConfig, policy);
    TEST_ASSERT_NOT_NULL_MESSAGE(node, "addRemoteNode failed in rig");
  }
};

static const uint8_t kInputsA5[] = {0xA5, 0x01};

/// Run the initial I -> full-T preamble to completion (I and T expect no
/// reply, interop E8), drain them from the sent log, and return the mock
/// tick at which the first P is sent. Assumes the default 500 ms settle,
/// 2 ms post-T gap, and 5 ms pacing with zero send latency.
static uint32_t primeToPoll(Rig& rig) {
  rig.host.begin();
  runUntil(rig.host, 0, 507);
  CMRIPacket scratch;
  (void)rig.transport.takeSent(scratch);  // I
  (void)rig.transport.takeSent(scratch);  // T
  return 508;
}

// ----------------------------------------------------- listener seam (D7)

/// Context cookie for the recording listeners below.
struct ListenerLog {
  int accepted = 0;
  int rejected = 0;
  int timeouts = 0;
  int stateChanges = 0;
  int reinitScheduled = 0;
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
    case CMRIHostEventType::kReinitScheduled: ++log.reinitScheduled; break;
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

// ----------------------------------------------------- init / poll emission

static void test_first_exchange_is_init(void) {
  Rig rig;
  rig.host.begin();
  rig.host.tick(0);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8(5 + kUaOffset, sent.ua);
  TEST_ASSERT_EQUAL_HEX8('I', sent.mt);
  TEST_ASSERT_EQUAL_UINT16(13, sent.length);
  // CPNODE 'C' dialect (interop E3):
  // 'C' dH dL opts1 opts2 NI NO 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
  TEST_ASSERT_EQUAL_HEX8('C', sent.body[0]);
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[1]);   // dH
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[2]);   // dL
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[3]);   // opts1
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[4]);   // opts2
  TEST_ASSERT_EQUAL_HEX8(2, sent.body[5]);   // NI = inputBytes
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[6]);   // NO = outputBytes
  TEST_ASSERT_EQUAL_HEX8(0xFF, sent.body[7]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, sent.body[12]);
  TEST_ASSERT_EQUAL_UINT32(0, rig.host.statistics().pollsSent);
}

static void test_init_carries_per_node_dh_dl(void) {
  CMRIHost::RemoteNodePolicy policy;
  policy.transmissionDelayDh = 0x07;
  policy.transmissionDelayDl = 0xD0;
  Rig rig(CMRIHostConfig(), policy);
  rig.host.begin();
  rig.host.tick(0);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('I', sent.mt);
  TEST_ASSERT_EQUAL_HEX8(0x07, sent.body[1]);  // dH
  TEST_ASSERT_EQUAL_HEX8(0xD0, sent.body[2]);  // dL
}

static void test_init_followed_by_full_transmit_after_settle(void) {
  Rig rig;
  rig.host.begin();
  runUntil(rig.host, 0, 499);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));   // I
  TEST_ASSERT_EQUAL_HEX8('I', sent.mt);
  TEST_ASSERT_FALSE(rig.transport.takeSent(sent));  // no T before settle
  runUntil(rig.host, 500, 501);                    // settle -> T sends
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));   // T
  TEST_ASSERT_EQUAL_HEX8('T', sent.mt);
  TEST_ASSERT_EQUAL_UINT16(0, sent.length);  // outputBytes=0 -> empty image
}

static void test_no_poll_before_begin(void) {
  Rig rig;
  rig.host.tick(0);
  TEST_ASSERT_EQUAL_size_t(0, rig.transport.sentCount());
}

static void test_poll_carries_wire_ua_and_empty_body(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  runUntil(rig.host, base, base + 1);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));  // P (I and T drained)
  TEST_ASSERT_EQUAL_HEX8(5 + kUaOffset, sent.ua);
  TEST_ASSERT_EQUAL_HEX8('P', sent.mt);
  TEST_ASSERT_EQUAL_UINT16(0, sent.length);
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
}

// ------------------------------------------------------- transmit (T) image

static void test_transmit_carries_full_output_image(void) {
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), 2, 3);
  const uint8_t out[] = {0xDE, 0xAD, 0xBE};
  TEST_ASSERT_TRUE(rig.node->setOutputs(out, 3));
  rig.host.begin();
  rig.host.tick(0);  // I first
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));  // I
  TEST_ASSERT_EQUAL_HEX8(3, sent.body[6]);  // NO = outputBytes
  runUntil(rig.host, 500, 501);             // settle -> T
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));  // T
  TEST_ASSERT_EQUAL_HEX8('T', sent.mt);
  TEST_ASSERT_EQUAL_UINT16(3, sent.length);
  TEST_ASSERT_EQUAL_HEX8(0xDE, sent.body[0]);
  TEST_ASSERT_EQUAL_HEX8(0xAD, sent.body[1]);
  TEST_ASSERT_EQUAL_HEX8(0xBE, sent.body[2]);
}

static void test_setOutputBit_marks_dirty_then_transmit(void) {
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), 0, 1);
  rig.node->setOutputBit(3, true);
  rig.host.begin();
  rig.host.tick(0);  // I
  CMRIPacket sent;
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));  // I
  runUntil(rig.host, 500, 501);                    // settle -> T
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));   // T
  TEST_ASSERT_EQUAL_HEX8('T', sent.mt);
  TEST_ASSERT_EQUAL_UINT16(1, sent.length);
  TEST_ASSERT_EQUAL_HEX8(0x08, sent.body[0]);  // bit 3 set
}

static void test_forceTransmit_marks_dirty_then_transmit(void) {
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), 0, 1);
  rig.host.begin();
  // Run past the initial I + T and its post-T gap clear (t=503) before
  // forcing, so the gap clear does not wipe the forced dirty flag.
  runUntil(rig.host, 0, 504);
  CMRIPacket sent;
  while (rig.transport.takeSent(sent)) { }  // drain I, T
  rig.node->forceTransmit();
  runUntil(rig.host, 505, 520);  // next slot sends T (dirty)
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('T', sent.mt);
}

static void test_refresh_resends_transmit_on_interval(void) {
  CMRIHostConfig config;
  config.transmitRefreshMs = 100;
  Rig rig(config, CMRIHost::RemoteNodePolicy(), 0, 1);
  rig.host.begin();
  rig.transport.onSendReplyPacket(5 + kUaOffset, 'P', makePacket(5, 'R'), 0,
                                  MockCMRITransport::kRepeatForever);
  runUntil(rig.host, 0, 501);  // I + T (lastTxMs_ = 501)
  CMRIPacket sent;
  while (rig.transport.takeSent(sent)) { }  // drain I, T
  // No output change; the refresh timer re-sends a full T once
  // now - lastTxMs_ >= transmitRefreshMs (100 ms). Polls run fast (scripted
  // reply) until the refresh window opens.
  runUntil(rig.host, 502, 700);
  int transmits = 0;
  while (rig.transport.takeSent(sent)) {
    if (sent.mt == 'T') ++transmits;
  }
  TEST_ASSERT_TRUE(transmits >= 1);
}

// --------------------------------------------------------- reply acceptance

static void test_reply_commits_inputs_freshness_state_statistics(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->inputByte(1));
  TEST_ASSERT_TRUE(rig.node->inputBit(0));
  TEST_ASSERT_FALSE(rig.node->inputBit(1));
  TEST_ASSERT_TRUE(rig.node->inputBit(2));
  TEST_ASSERT_TRUE(rig.node->inputBit(8));
  TEST_ASSERT_FALSE(rig.node->inputBit(9));
  TEST_ASSERT_FALSE(rig.node->inputBit(999));
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().repliesAccepted);
  TEST_ASSERT_TRUE(rig.node->inputAgeMs(base + 2) <= 2);
}

static void test_turnaround_is_measured_from_send_complete(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
      /*delayMs=*/20);
  runUntil(rig.host, base, base + 20);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(20, rig.node->statistics().lastTurnaroundMs);
}

// -------------------------------------------------------- reply-gate timeout

static void test_default_reply_gate_is_250ms(void) {
  Rig rig;  // no script: silent
  uint32_t base = primeToPoll(rig);
  runUntil(rig.host, base, base + 249);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  rig.host.tick(base + 250);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().consecutiveMisses);
}

static void test_policy_overrides_reply_gate(void) {
  CMRIHost::RemoteNodePolicy policy;
  policy.replyTimeoutMs = 50;
  Rig rig(CMRIHostConfig(), policy);
  uint32_t base = primeToPoll(rig);
  runUntil(rig.host, base, base + 49);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  rig.host.tick(base + 50);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
}

static void test_reply_gate_opens_at_send_complete_not_accept(void) {
  CMRIHost::RemoteNodePolicy policy;
  policy.replyTimeoutMs = 50;
  Rig rig(CMRIHostConfig(), policy);
  uint32_t base = primeToPoll(rig);  // preamble at zero send latency
  rig.transport.setSendLatencyMs(5);  // latency applies to the P
  runUntil(rig.host, base, base + 54);  // P accept base, complete +5, gate +55
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  rig.host.tick(base + 55);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
}

// ------------------------------------------------------- reply verification

static void test_wrong_ua_reply_is_rejected(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 250);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_TRUE(rig.host.statistics().repliesRejected >= 1);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
}

static void test_wrong_mt_reply_is_rejected(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(5 + kUaOffset, 'P', makePacket(5, 'E'));
  runUntil(rig.host, base, base + 250);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_TRUE(rig.host.statistics().repliesRejected >= 1);
}

static void test_wrong_length_reply_counts_error_without_commit(void) {
  Rig rig;  // node expects 2 input bytes
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)));
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().errors);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_HEX8(0x00, rig.node->inputByte(0));  // never committed
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
}

// ------------------------------------------- miss, recovery, re-init, health

static void test_recovery_after_miss(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 1);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 300);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().recoveries);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().consecutiveMisses);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

static void test_offline_after_miss_threshold_then_recovers(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 6);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  // Six P misses (each 250 ms gate + 5 ms pacing); the 6th arms the
  // re-init ladder, which re-sends I + full T before the recovery P.
  runUntil(rig.host, base, base + 1530);
  TEST_ASSERT_EQUAL_UINT32(6, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, rig.node->state());
  // VALIDATION: Interop v1.1 2.3.10: keep polling a silent Node forever.
  runUntil(rig.host, base + 1531, base + 2600);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().recoveries);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

static void test_reinit_ladder_fires_after_miss_threshold(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P',
                                 MockCMRITransport::kRepeatForever);
  // Run past six P misses so the ladder arms and re-sends I + full T.
  runUntil(rig.host, base, base + 2100);
  TEST_ASSERT_TRUE(log.reinitScheduled >= 1);
  int inits = 0, transmits = 0;
  CMRIPacket sent;
  while (rig.transport.takeSent(sent)) {
    if (sent.mt == 'I') ++inits;
    else if (sent.mt == 'T') ++transmits;
  }
  TEST_ASSERT_TRUE(inits >= 1);
  TEST_ASSERT_TRUE(transmits >= 1);
}

static void test_invalidation_keeps_last_good_bytes(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  // Establish a good input image.
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0, 1);
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  // Force the re-init ladder: silence forever, run past six misses.
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P',
                                 MockCMRITransport::kRepeatForever);
  runUntil(rig.host, base + 3, base + 1600);
  // Invalidation clears freshness but keeps the last-good bytes: 0 is a
  // valid consumer value, so zeroing would assert "all clear" (QBASIC F15).
  TEST_ASSERT_EQUAL(Age::kNeverMarked, rig.node->inputAgeMs(base + 1600));
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));  // NOT zeroed
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->inputByte(1));
  // Misses exceed the threshold, so health is OFFLINE (misses outrank the
  // cleared freshness in the state computation).
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, rig.node->state());
}

static void test_stale_when_inputs_outlive_threshold(void) {
  CMRIHostConfig config;
  config.missThreshold = 1000;  // keep OFFLINE out of this test
  Rig rig(config, CMRIHost::RemoteNodePolicy(), 2, 0);
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
  // I (t=0) -> settle -> T -> P -> reply; node 6 ONLINE.
  runUntil(rig.host, 0, 510);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  runUntil(rig.host, 511, 700);  // silence: the image ages past 100 ms
  TEST_ASSERT_EQUAL(RemoteNodeState::kStale, node->state());
}

static void test_output_only_node_gets_keepalive_poll(void) {
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), 0, 1);
  rig.host.begin();
  runUntil(rig.host, 0, 501);  // I + T
  CMRIPacket sent;
  (void)rig.transport.takeSent(sent);  // I
  (void)rig.transport.takeSent(sent);  // T
  // An output-only node still receives a keepalive P (interop 2.3.13).
  rig.transport.onSendReplyPacket(5 + kUaOffset, 'P', makePacket(5, 'R'), 0, 1);
  runUntil(rig.host, 502, 510);  // first P at 508, reply accepted at 509
  TEST_ASSERT_TRUE(rig.transport.takeSent(sent));  // P (I and T drained)
  TEST_ASSERT_EQUAL_HEX8('P', sent.mt);
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
}

// ------------------------------------------------------- I/T reply (E8)

static void test_eot_during_init_settle_is_unsolicited(void) {
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), 2, 1);
  rig.host.begin();
  rig.host.tick(0);  // I sent
  // A node MAY emit an end-of-transmission marker ('E') or ack an I; it
  // arrives while no P is outstanding and is unsolicited: counted and
  // discarded, not rejected or an error (interop E8).
  rig.transport.injectPacketAt(makePacket(5, 'E'), 100);  // during I settle
  runUntil(rig.host, 1, 520);
  TEST_ASSERT_TRUE(rig.host.statistics().unsolicitedPackets >= 1);
  TEST_ASSERT_EQUAL_UINT32(0, rig.host.statistics().repliesRejected);
  // The exchange completed on its own timer: the P phase is reached.
  TEST_ASSERT_TRUE(rig.host.statistics().pollsSent >= 1);
}

// ------------------------------------------------------- schedule discipline

static void test_pacing_gap_between_exchanges(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      MockCMRITransport::kRepeatForever);
  runUntil(rig.host, base, base + 1);  // P at base, reply accepted
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
  runUntil(rig.host, base + 2, base + 5);  // pacing gate holds
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
  rig.host.tick(base + 6);
  TEST_ASSERT_EQUAL_UINT32(2, rig.host.statistics().pollsSent);
}

static void test_round_robin_over_enabled_nodes(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;  // inputBytes=0, outputBytes=0
  TEST_ASSERT_NOT_NULL(host.addRemoteNode(1, config));
  TEST_ASSERT_NOT_NULL(host.addRemoteNode(2, config));
  host.begin();
  // Prime both nodes through I -> T so the next outbounds are P.
  runUntil(host, 0, 1015);
  transport.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                              makePacket(1, 'R'), 0, 1);
  transport.onSendReplyPacket(MockCMRITransport::kMatchAny, 'P',
                              makePacket(2, 'R'), 0, 1);
  runUntil(host, 1016, 1030);
  CMRIPacket sent;
  uint8_t firstPollUa = 0, secondPollUa = 0;
  int pollSeen = 0;
  while (transport.takeSent(sent)) {
    if (sent.mt == 'P' && pollSeen < 2) {
      if (pollSeen == 0) firstPollUa = sent.ua;
      else secondPollUa = sent.ua;
      ++pollSeen;
    }
  }
  TEST_ASSERT_EQUAL_INT(2, pollSeen);
  TEST_ASSERT_EQUAL_HEX8(1 + kUaOffset, firstPollUa);
  TEST_ASSERT_EQUAL_HEX8(2 + kUaOffset, secondPollUa);
}

static void test_send_refusal_retries_without_blocking(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.setLinkUp(false);
  runUntil(rig.host, base, base + 3);
  TEST_ASSERT_EQUAL_UINT32(0, rig.host.statistics().pollsSent);
  TEST_ASSERT_TRUE(rig.host.statistics().pollSendRetries >= 1);
  rig.transport.setLinkUp(true);
  rig.host.tick(base + 4);
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);
}

static void test_unsolicited_packets_are_counted(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);  // no nodes: schedule stays idle
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
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyBytes(5 + kUaOffset, 'P', wire, n, /*delayMs=*/1,
                                 /*repeat=*/1, /*interByteGapMs=*/2);
  runUntil(rig.host, base, base + 40);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

static void test_event_listener_sees_accept_and_state_change(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_INT(1, log.accepted);
  TEST_ASSERT_EQUAL_PTR(rig.node, log.lastNode);
  TEST_ASSERT_EQUAL_INT(1, log.stateChanges);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, log.lastPreviousState);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, log.lastNewState);
}

static void test_event_listener_sees_timeout_and_offline_transition(void) {
  Rig rig;  // no script: silent
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);
  runUntil(rig.host, base, base + 1530);
  TEST_ASSERT_EQUAL_INT(6, log.timeouts);
  TEST_ASSERT_EQUAL_INT(1, log.stateChanges);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, log.lastPreviousState);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, log.lastNewState);
}

static void test_event_listener_sees_rejected_reply(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_INT(1, log.rejected);
  TEST_ASSERT_EQUAL_INT(0, log.accepted);
}

static void test_trace_listener_sees_both_directions(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onTrace(recordTrace, &log);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 2);
  // Preamble traced I and T on TX; the P and R happen here.
  TEST_ASSERT_TRUE(log.txTraces >= 1);
  TEST_ASSERT_EQUAL_HEX8('P', log.lastTxMt);
  TEST_ASSERT_TRUE(log.rxTraces >= 1);
  TEST_ASSERT_EQUAL_HEX8('R', log.lastRxMt);
}

static void test_trace_listener_sees_init_and_transmit(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onTrace(recordTrace, &log);
  rig.host.begin();
  runUntil(rig.host, 0, 501);  // I + T
  TEST_ASSERT_TRUE(log.txTraces >= 2);
  TEST_ASSERT_EQUAL_HEX8('T', log.lastTxMt);  // last TX before P is T
}

static void test_listener_registration_locked_after_begin(void) {
  Rig rig;
  ListenerLog log;
  uint32_t base = primeToPoll(rig);  // begin() called inside primeToPoll
  rig.host.onEvent(recordEvent, &log);  // ignored: configuration locked
  rig.host.onTrace(recordTrace, &log);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_INT(0, log.accepted);
  TEST_ASSERT_EQUAL_INT(0, log.txTraces);
  TEST_ASSERT_EQUAL_INT(0, log.rxTraces);
}

static void test_null_listeners_are_harmless(void) {
  Rig rig;
  rig.host.onEvent(nullptr);
  rig.host.onTrace(nullptr);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 2);
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

  RemoteNodeConfig overOut;
  overOut.outputBytes = RemoteNodeHandle::kMaxOutputBytes + 1;
  TEST_ASSERT_NULL(host.addRemoteNode(5, overOut));

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
  RUN_TEST(test_first_exchange_is_init);
  RUN_TEST(test_init_carries_per_node_dh_dl);
  RUN_TEST(test_init_followed_by_full_transmit_after_settle);
  RUN_TEST(test_no_poll_before_begin);
  RUN_TEST(test_poll_carries_wire_ua_and_empty_body);
  RUN_TEST(test_transmit_carries_full_output_image);
  RUN_TEST(test_setOutputBit_marks_dirty_then_transmit);
  RUN_TEST(test_forceTransmit_marks_dirty_then_transmit);
  RUN_TEST(test_refresh_resends_transmit_on_interval);
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
  RUN_TEST(test_reinit_ladder_fires_after_miss_threshold);
  RUN_TEST(test_invalidation_keeps_last_good_bytes);
  RUN_TEST(test_stale_when_inputs_outlive_threshold);
  RUN_TEST(test_output_only_node_gets_keepalive_poll);
  RUN_TEST(test_eot_during_init_settle_is_unsolicited);
  RUN_TEST(test_pacing_gap_between_exchanges);
  RUN_TEST(test_round_robin_over_enabled_nodes);
  RUN_TEST(test_send_refusal_retries_without_blocking);
  RUN_TEST(test_unsolicited_packets_are_counted);
  RUN_TEST(test_exchange_completes_over_gapped_wire_bytes);
  RUN_TEST(test_event_listener_sees_accept_and_state_change);
  RUN_TEST(test_event_listener_sees_timeout_and_offline_transition);
  RUN_TEST(test_event_listener_sees_rejected_reply);
  RUN_TEST(test_trace_listener_sees_both_directions);
  RUN_TEST(test_trace_listener_sees_init_and_transmit);
  RUN_TEST(test_listener_registration_locked_after_begin);
  RUN_TEST(test_null_listeners_are_harmless);
  RUN_TEST(test_add_remote_node_validation);
  RUN_TEST(test_node_table_capacity_is_enforced);
  return UNITY_END();
}
