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
using CMRInet::CMRIHostStatistics;
using CMRInet::CMRIPacket;
using CMRInet::encodeFrame;
using CMRInet::kUaOffset;
using CMRInet::MockCMRITransport;
using CMRInet::RemoteNodeConfig;
using CMRInet::RemoteNodeConformance;
using CMRInet::RemoteNodeHandle;
using CMRInet::RemoteNodeImageState;
using CMRInet::RemoteNodeLiveness;
using CMRInet::RemoteNodeState;
using CMRInet::RemoteNodeStatistics;

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
    // Register the node, then look up its handle. addRemoteNode returns
    // this call's status, not the handle; node(address) does.
    RemoteNodeConfig nodeConfig;
    nodeConfig.inputBytes = inputBytes;
    nodeConfig.outputBytes = outputBytes;
    TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                      host.addRemoteNode(5, nodeConfig, policy));
    node = host.node(5);
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
    case CMRIHostEventType::kPollBackoffChanged: break;
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
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kFresh, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kUnknown, rig.node->conformance());
  TEST_ASSERT_TRUE(rig.node->inputsUsable());
  TEST_ASSERT_FALSE(rig.node->isHealthy());
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().repliesAccepted);
  TEST_ASSERT_TRUE(rig.node->inputAgeMs(base + 2) <= 2);
}

static void test_initial_axes_and_predicates_before_first_reply(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  runUntil(rig.host, base, base + 1);  // first poll sent; no reply yet
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kUnknown, rig.node->conformance());
  TEST_ASSERT_FALSE(rig.node->inputsUsable());
  TEST_ASSERT_FALSE(rig.node->isHealthy());
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
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->consecutiveMisses());
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kMissing, rig.node->liveness());
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

static void test_wrong_geometry_after_silent_run_reports_stale(void) {
  Rig rig;  // node expects 2 input bytes
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);

  // Establish a committed image first, so this node has image history.
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());

  // Then force the node through a silent miss-run and have it return with
  // wrong geometry. The mismatch reply must not commit data.
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 6);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)),
      0, 1);

  bool reachedGeometryReject = false;
  uint32_t rejectAt = base + 3;
  for (uint32_t t = base + 3; t <= base + 45000; ++t) {
    rig.host.tick(t);
    if (rig.node->statistics().errors >= 1) {
      reachedGeometryReject = true;
      rejectAt = t;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(
      reachedGeometryReject,
      "did not reach geometry mismatch after silent miss-run");

  // Pin the projection behavior: after image history exists, silent +
  // invalidation + wrong-geometry return keeps the image axis verdict and
  // projects STALE (not UNINITIALIZED).
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kStale, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kStale, rig.node->state());
  TEST_ASSERT_EQUAL(Age::kNeverMarked, rig.node->inputAgeMs(rejectAt));
}

// ------------------------------------------- miss, recovery, re-init, health

static void test_recovery_after_miss(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 1);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  bool reachedRecovery = false;
  for (uint32_t t = base; t <= base + 6000; ++t) {
    rig.host.tick(t);
    if (rig.node->statistics().recoveries >= 1) {
      reachedRecovery = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(reachedRecovery, "did not reach first recovery in expected window");
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().recoveries);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->consecutiveMisses());
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

static void test_offline_after_miss_threshold_then_recovers(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendStaySilent(5 + kUaOffset, 'P', 6);
  rig.transport.onSendReplyPacket(
      5 + kUaOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  // Wait until six misses are actually observed under the active backoff
  // schedule; the 6th miss arms the re-init ladder.
  bool reachedSixMisses = false;
  uint32_t sixMissesAt = base;
  for (uint32_t t = base; t <= base + 25000; ++t) {
    rig.host.tick(t);
    if (rig.node->statistics().noReplies >= 6) {
      reachedSixMisses = true;
      sixMissesAt = t;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(reachedSixMisses, "did not reach six misses in expected window");
  TEST_ASSERT_EQUAL_UINT32(6, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kSilent, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, rig.node->state());
  // VALIDATION: Interop v1.1 2.3.10: keep polling a silent Node forever.
  bool recovered = false;
  for (uint32_t t = sixMissesAt + 1; t <= sixMissesAt + 20000; ++t) {
    rig.host.tick(t);
    if (rig.node->statistics().recoveries >= 1) {
      recovered = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(recovered, "did not recover after miss-run");
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
  // Run until the ladder is observed under the active backoff schedule.
  bool ladderArmed = false;
  uint32_t ladderArmedAt = base;
  for (uint32_t t = base; t <= base + 25000; ++t) {
    rig.host.tick(t);
    if (log.reinitScheduled >= 1) {
      ladderArmed = true;
      ladderArmedAt = t;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(ladderArmed, "re-init ladder did not arm in expected window");
  TEST_ASSERT_TRUE(log.reinitScheduled >= 1);
  runUntil(rig.host, ladderArmedAt + 1, ladderArmedAt + 40000);
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
  bool reachedSixMisses = false;
  uint32_t sixMissesAt = base + 3;
  for (uint32_t t = base + 3; t <= base + 25000; ++t) {
    rig.host.tick(t);
    if (rig.node->statistics().noReplies >= 6) {
      reachedSixMisses = true;
      sixMissesAt = t;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(reachedSixMisses, "did not reach six misses for invalidation test");
  // Invalidation clears freshness but keeps the last-good bytes: 0 is a
  // valid consumer value, so zeroing would assert "all clear" (QBASIC F15).
  TEST_ASSERT_EQUAL(Age::kNeverMarked, rig.node->inputAgeMs(sixMissesAt));
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));  // NOT zeroed
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->inputByte(1));
  // Discriminating D16 assertion: liveness says SILENT, and image state
  // keeps its own validity verdict (STALE) instead of collapsing to NONE.
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kSilent, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kStale, rig.node->imageState());
  // Projection still maps this to OFFLINE because liveness dominates the
  // scalar state.
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, rig.node->state());
}

static void test_stale_when_inputs_outlive_threshold(void) {
  CMRIHostConfig config;
  config.missThreshold = 1000;  // keep OFFLINE out of this test
  Rig rig(config, CMRIHost::RemoteNodePolicy(), 2, 0);
  RemoteNodeConfig staleConfig;
  staleConfig.inputBytes = 2;
  staleConfig.stalenessMs = 100;
  rig.host.addRemoteNode(6, staleConfig);
  RemoteNodeHandle* node = rig.host.node(6);
  TEST_ASSERT_NOT_NULL(node);
  rig.node->setEnabled(false);  // only the staleness node is polled
  rig.host.begin();
  rig.transport.onSendReplyPacket(
      6 + kUaOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  // I (t=0) -> settle -> T -> P -> reply; node 6 ONLINE.
  runUntil(rig.host, 0, 510);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kFresh, node->imageState());
  runUntil(rig.host, 511, 700);  // silence: the image ages past 100 ms
  TEST_ASSERT_EQUAL(RemoteNodeState::kStale, node->state());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kStale, node->imageState());
  TEST_ASSERT_FALSE(node->inputsUsable());
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
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(1, config));
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(2, config));
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
  bool reachedSixTimeouts = false;
  for (uint32_t t = base; t <= base + 25000; ++t) {
    rig.host.tick(t);
    if (log.timeouts >= 6) {
      reachedSixTimeouts = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(reachedSixTimeouts, "did not reach six timeout events in expected window");
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
  // Each add reports its own outcome immediately (Design v1.2 D5).
  // One host serves every rejection case, because a rejection leaves no
  // residue for the next call to trip over.
  using CS = CMRIHost::ConfigStatus;

  MockCMRITransport t;
  CMRIHost h(t);

  RemoteNodeConfig ok;
  ok.inputBytes = 2;

  RemoteNodeConfig tooManyInputs;
  tooManyInputs.inputBytes = RemoteNodeHandle::kMaxInputBytes + 1;

  RemoteNodeConfig tooManyOutputs;
  tooManyOutputs.outputBytes = RemoteNodeHandle::kMaxOutputBytes + 1;

  TEST_ASSERT_EQUAL(CS::kAddressOutOfRange, h.addRemoteNode(128, ok));
  TEST_ASSERT_EQUAL(CS::kInputBytesTooLarge, h.addRemoteNode(5, tooManyInputs));
  TEST_ASSERT_EQUAL(CS::kOutputBytesTooLarge,
                    h.addRemoteNode(5, tooManyOutputs));
  TEST_ASSERT_EQUAL_size_t(0, h.nodeCount());

  // A good add still succeeds after all of those rejections.
  TEST_ASSERT_EQUAL(CS::kOk, h.addRemoteNode(5, ok));
  TEST_ASSERT_EQUAL_size_t(1, h.nodeCount());
  TEST_ASSERT_NOT_NULL(h.node(5));

  // A duplicate address is rejected without disturbing the original.
  TEST_ASSERT_EQUAL(CS::kAddressInUse, h.addRemoteNode(5, ok));
  TEST_ASSERT_EQUAL_size_t(1, h.nodeCount());

  // begin() is the config->running transition, not a table lock: the
  // same add is judged on its merits before and after (Design v1.2 D5).
  h.begin();
  TEST_ASSERT_EQUAL(CS::kOk, h.addRemoteNode(6, ok));
  TEST_ASSERT_EQUAL_size_t(2, h.nodeCount());
  TEST_ASSERT_NOT_NULL(h.node(6));

  // The same validations still apply at runtime.
  TEST_ASSERT_EQUAL(CS::kAddressInUse, h.addRemoteNode(6, ok));
  TEST_ASSERT_EQUAL(CS::kAddressOutOfRange, h.addRemoteNode(200, ok));

  // Mutators name a live node; a missing one is its own status, not a
  // silent no-op.
  TEST_ASSERT_EQUAL(CS::kNoSuchNode, h.deleteRemoteNode(7));
  TEST_ASSERT_EQUAL(CS::kNoSuchNode, h.setRemoteNodeGeometry(7, 1, 1));
  TEST_ASSERT_EQUAL(CS::kInputBytesTooLarge,
                    h.setRemoteNodeGeometry(
                        6, RemoteNodeHandle::kMaxInputBytes + 1, 0));
  TEST_ASSERT_EQUAL(CS::kOutputBytesTooLarge,
                    h.setRemoteNodeGeometry(
                        6, 0, RemoteNodeHandle::kMaxOutputBytes + 1));
  // A rejected geometry change leaves the node alone.
  TEST_ASSERT_EQUAL_size_t(2, h.node(6)->inputLength());
}

// ------------------------------------------- runtime mutation (D5, #86)

// A node added after begin() is a full participant: it gets the same
// I -> full-T bootstrap as one added during configuration (interop
// 2.3.1). Without this, runtime add would produce a node the engine
// polls but never initializes.
static void test_add_after_begin_bootstraps_the_new_node(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(5, 2, 0));
  host.begin();
  runUntil(host, 0, 600);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }  // drain node 5's preamble

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(6, 1, 2));
  TEST_ASSERT_EQUAL_size_t(2, host.nodeCount());
  TEST_ASSERT_NOT_NULL(host.node(6));

  bool sawInit = false;
  bool sawTransmitAfterInit = false;
  for (uint32_t t = 601; t <= 4000; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.ua != 6 + kUaOffset) {
        continue;
      }
      if (sent.mt == 'I') {
        sawInit = true;
        TEST_ASSERT_EQUAL_HEX8(1, sent.body[5]);  // NI
        TEST_ASSERT_EQUAL_HEX8(2, sent.body[6]);  // NO
      } else if (sent.mt == 'T' && sawInit) {
        sawTransmitAfterInit = true;
        TEST_ASSERT_EQUAL_UINT16(2, sent.length);  // full output image
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(sawInit, "runtime-added node never received an I");
  TEST_ASSERT_TRUE_MESSAGE(sawTransmitAfterInit,
                           "runtime-added node never received its full T");
}

// Delete takes the node out of the rotation for good. The round-robin
// cursor must step over the tombstone rather than treating the table as
// a dense prefix.
static void test_delete_removes_the_node_from_the_rotation(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(5, 0, 0);
  host.addRemoteNode(6, 0, 0);
  host.begin();
  runUntil(host, 0, 2000);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.deleteRemoteNode(5));
  TEST_ASSERT_EQUAL_size_t(1, host.nodeCount());
  TEST_ASSERT_NULL(host.node(5));
  TEST_ASSERT_NOT_NULL(host.node(6));
  // Deleting it twice is not a silent success.
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kNoSuchNode,
                    host.deleteRemoteNode(5));

  bool addressedTheDeletedNode = false;
  bool addressedTheSurvivor = false;
  for (uint32_t t = 2001; t <= 9000; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.ua == 5 + kUaOffset) {
        addressedTheDeletedNode = true;
      } else if (sent.ua == 6 + kUaOffset) {
        addressedTheSurvivor = true;
      }
    }
  }
  TEST_ASSERT_FALSE_MESSAGE(addressedTheDeletedNode,
                            "a deleted node was still addressed on the wire");
  TEST_ASSERT_TRUE_MESSAGE(addressedTheSurvivor,
                           "the surviving node fell out of the rotation");
}

// The subtle one. Deleting the node of the outstanding exchange is
// legal, and the send cannot be aborted -- so the exchange is orphaned:
// the frame completes, a late reply is discarded, and NOTHING is
// attributed anywhere. Not a node counter, not a host reply counter,
// and not an event either: emitEvent_ takes a handle, so an event fired
// here would name a tombstone or the slot's next occupant.
static void test_delete_while_in_flight_attributes_nothing(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(5, 2, 0);
  host.addRemoteNode(6, 2, 0);
  ListenerLog log;
  host.onEvent(recordEvent, &log);
  host.begin();
  // Node 6 sits out, so every event in the measured window can only
  // have come from the orphaned exchange.
  host.node(6)->setEnabled(false);

  CMRIPacket sent;
  uint32_t t = 0;
  bool polled = false;
  for (; t <= 5000 && !polled; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt == 'P' && sent.ua == 5 + kUaOffset) {
        polled = true;
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(polled, "node 5 was never polled");

  const CMRIHostStatistics before = host.statistics();
  const int timeoutsBefore = log.timeouts;
  const int acceptedBefore = log.accepted;
  const int rejectedBefore = log.rejected;

  // Delete with the poll outstanding.
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.deleteRemoteNode(5));
  TEST_ASSERT_EQUAL_UINT32(before.orphanedExchanges + 1,
                           host.statistics().orphanedExchanges);

  // Hand the freed slot straight to a different logical device. This is
  // the hazard in its sharpest form: without an explicit orphan mark the
  // late reply below is matched by slot index and pollutes THIS node's
  // counters -- a node that has never been on the wire.
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(7, 2, 0));
  RemoteNodeHandle* newcomer = host.node(7);
  TEST_ASSERT_NOT_NULL(newcomer);
  newcomer->setEnabled(false);  // keep it off the wire in this window

  // The deleted node answers after it is gone. Then let the gate expire.
  transport.injectPacketAt(makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                           t);
  runUntil(host, t, t + 600);

  // The slot's new occupant is untouched, and is certainly not ONLINE
  // off the back of somebody else's reply.
  TEST_ASSERT_EQUAL_UINT32(0, newcomer->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, newcomer->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(0, newcomer->statistics().errors);
  TEST_ASSERT_EQUAL_UINT32(0, newcomer->consecutiveMisses());
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, newcomer->state());
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, newcomer->inputAgeMs(t + 600));

  // The bystander is untouched too.
  const RemoteNodeStatistics& survivor = host.node(6)->statistics();
  TEST_ASSERT_EQUAL_UINT32(0, survivor.exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, survivor.noReplies);
  TEST_ASSERT_EQUAL_UINT32(0, survivor.errors);

  // No host reply counter moved either: the late reply was neither
  // accepted, nor rejected, nor miscounted as unsolicited.
  const CMRIHostStatistics& after = host.statistics();
  TEST_ASSERT_EQUAL_UINT32(before.repliesAccepted, after.repliesAccepted);
  TEST_ASSERT_EQUAL_UINT32(before.repliesRejected, after.repliesRejected);
  TEST_ASSERT_EQUAL_UINT32(before.unsolicitedPackets, after.unsolicitedPackets);

  // And no event named anybody.
  TEST_ASSERT_EQUAL_INT_MESSAGE(timeoutsBefore, log.timeouts,
                                "orphaned exchange charged a miss");
  TEST_ASSERT_EQUAL_INT_MESSAGE(acceptedBefore, log.accepted,
                                "orphaned exchange accepted a reply");
  TEST_ASSERT_EQUAL_INT_MESSAGE(rejectedBefore, log.rejected,
                                "orphaned exchange rejected a reply");
}

// Regression for the batch error model retired in Design v1.2 D5. The
// old chain-poisoning short-circuit made one rejected add silently
// suppress every later add for the life of the host, which broke
// runtime reconfiguration through the verb shell.
static void test_rejected_add_does_not_poison_later_adds(void) {
  using CS = CMRIHost::ConfigStatus;
  MockCMRITransport transport;
  CMRIHost host(transport);

  RemoteNodeConfig bad;
  bad.inputBytes = RemoteNodeHandle::kMaxInputBytes + 1;
  RemoteNodeConfig good;
  good.inputBytes = 2;

  TEST_ASSERT_EQUAL(CS::kInputBytesTooLarge, host.addRemoteNode(7, bad));
  TEST_ASSERT_EQUAL_size_t(0, host.nodeCount());

  // The next add must be judged on its own merits.
  TEST_ASSERT_EQUAL(CS::kOk, host.addRemoteNode(7, good));
  TEST_ASSERT_EQUAL_size_t(1, host.nodeCount());
  TEST_ASSERT_NOT_NULL(host.node(7));

  // And the host must still be usable: a poisoned host would never poll.
  TEST_ASSERT_EQUAL(CS::kOk, host.addRemoteNode(8, good));
  host.begin();
  runUntil(host, 0, 1020);
  TEST_ASSERT_TRUE(host.statistics().pollsSent >= 1);
}

static void test_node_table_capacity_is_enforced(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;
  config.inputBytes = 0;
  for (size_t i = 0; i < CMRIHost::kMaxNodes; ++i) {
    TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                      host.addRemoteNode(static_cast<uint8_t>(i), config));
  }
  // The next add overflows the table.
  TEST_ASSERT_EQUAL(
      CMRIHost::ConfigStatus::kTooManyNodes,
      host.addRemoteNode(static_cast<uint8_t>(CMRIHost::kMaxNodes), config));
  TEST_ASSERT_EQUAL_size_t(CMRIHost::kMaxNodes, host.nodeCount());
}

// Capacity and membership are independent (Design v1.2 D5). Capacity is
// slot availability, so deleting from a full table frees a slot and the
// very add that just overflowed must now succeed. A high-water count
// would refuse it forever.
static void test_delete_frees_a_slot_in_a_full_table(void) {
  using CS = CMRIHost::ConfigStatus;
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;
  for (size_t i = 0; i < CMRIHost::kMaxNodes; ++i) {
    TEST_ASSERT_EQUAL(CS::kOk,
                      host.addRemoteNode(static_cast<uint8_t>(i), config));
  }
  const uint8_t overflow = static_cast<uint8_t>(CMRIHost::kMaxNodes);
  TEST_ASSERT_EQUAL(CS::kTooManyNodes, host.addRemoteNode(overflow, config));

  TEST_ASSERT_EQUAL(CS::kOk, host.deleteRemoteNode(0));
  TEST_ASSERT_EQUAL_size_t(CMRIHost::kMaxNodes - 1, host.nodeCount());

  TEST_ASSERT_EQUAL(CS::kOk, host.addRemoteNode(overflow, config));
  TEST_ASSERT_EQUAL_size_t(CMRIHost::kMaxNodes, host.nodeCount());
  TEST_ASSERT_NOT_NULL(host.node(overflow));

  // Address 0 is the trap here: a cleaned tombstone holds address_ == 0,
  // and 0 is a perfectly legal node address. A lookup that tested the
  // address before occupancy would hand this tombstone back.
  TEST_ASSERT_NULL_MESSAGE(host.node(0),
                           "a cleaned tombstone answered to address 0");
}

// Slot reuse must produce a genuinely new subject. The hazard is quiet:
// inherited freshness makes a node that has never answered look ONLINE
// with data it never sent.
static void test_reused_slot_is_a_new_subject(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(5, 2, 0);
  host.begin();
  transport.onSendReplyPacket(5 + kUaOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);
  runUntil(host, 0, 1000);

  RemoteNodeHandle* before = host.node(5);
  TEST_ASSERT_NOT_NULL(before);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, before->state());
  TEST_ASSERT_TRUE(before->statistics().exchanges > 0);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.deleteRemoteNode(5));

  // A handle cached across the delete must be detectable as stale.
  // address() is that self-check, and it works only because the slot is
  // cleaned at delete rather than at reuse.
  TEST_ASSERT_NOT_EQUAL_MESSAGE(5, before->address(),
                                "a deleted node still answered to its address");

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(5, 2, 0));
  RemoteNodeHandle* fresh = host.node(5);
  TEST_ASSERT_NOT_NULL(fresh);
  TEST_ASSERT_EQUAL_PTR_MESSAGE(before, fresh,
                                "the cleaned tombstone was not reused");

  // Belief: no image at all, so not ONLINE before its first reply.
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, fresh->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, fresh->state());
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, fresh->inputAgeMs(1000));
  TEST_ASSERT_FALSE(fresh->inputsUsable());
  TEST_ASSERT_EQUAL_HEX8(0, fresh->inputByte(0));

  // Observation: a new subject counts from zero. Nothing was reset
  // mid-life -- delete ended the previous subject -- so monotonicity is
  // intact.
  TEST_ASSERT_EQUAL_UINT32(0, fresh->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, fresh->statistics().noReplies);
  TEST_ASSERT_EQUAL_UINT32(0, fresh->statistics().errors);
  TEST_ASSERT_EQUAL_UINT32(0, fresh->consecutiveMisses());

  // And it stays not-ONLINE until it actually answers.
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, host.node(5)->state());
}

// Geometry change is in place and identity-preserving: same address,
// same counters, same handle. What it must invalidate is the cached
// image, because the NI/NO announced in the I body just changed.
static void test_geometry_change_invalidates_image_and_forces_reinit(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(5, 2, 1);
  host.begin();
  transport.onSendReplyPacket(5 + kUaOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);
  runUntil(host, 0, 1000);

  RemoteNodeHandle* node = host.node(5);
  TEST_ASSERT_NOT_NULL(node);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  node->setOutputBit(0, true);
  TEST_ASSERT_TRUE(node->outputBit(0));
  const uint32_t exchangesBefore = node->statistics().exchanges;
  TEST_ASSERT_TRUE(exchangesBefore > 0);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.setRemoteNodeGeometry(5, 4, 3));

  // Identity preserved: same handle, same address, same running totals.
  // This is the same logical device with its IO cards rearranged, so its
  // observation substrate keeps counting.
  TEST_ASSERT_EQUAL_PTR(node, host.node(5));
  TEST_ASSERT_EQUAL_UINT8(5, node->address());
  TEST_ASSERT_EQUAL_UINT32(exchangesBefore, node->statistics().exchanges);

  // Belief cleared: the cached bytes described the old shape.
  TEST_ASSERT_EQUAL_size_t(4, node->inputLength());
  TEST_ASSERT_EQUAL_size_t(3, node->outputLength());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, node->state());
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, node->inputAgeMs(1000));
  TEST_ASSERT_FALSE(node->inputsUsable());
  TEST_ASSERT_FALSE_MESSAGE(node->outputBit(0),
                            "an output bit survived a geometry change");

  // The re-init ladder runs, and the new I announces the new NI/NO.
  bool sawInit = false;
  bool sawFullTransmit = false;
  for (uint32_t t = 1001; t <= 3000; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.ua != 5 + kUaOffset) {
        continue;
      }
      if (sent.mt == 'I') {
        sawInit = true;
        TEST_ASSERT_EQUAL_HEX8(4, sent.body[5]);  // NI
        TEST_ASSERT_EQUAL_HEX8(3, sent.body[6]);  // NO
      } else if (sent.mt == 'T' && sawInit) {
        sawFullTransmit = true;
        TEST_ASSERT_EQUAL_UINT16(3, sent.length);
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(sawInit, "geometry change did not force a re-init");
  TEST_ASSERT_TRUE_MESSAGE(sawFullTransmit,
                           "no full T followed the re-init");
}

// The whole lifecycle in one pass, in the order a layout actually
// evolves: a section is built, taken out of service, rewired,
// recommissioned, retired, and its address handed to a new device.
static void test_full_mutation_lifecycle(void) {
  using CS = CMRIHost::ConfigStatus;
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(9, 1, 1);  // a bystander that must survive it all
  host.begin();
  runUntil(host, 0, 700);

  // add
  TEST_ASSERT_EQUAL(CS::kOk, host.addRemoteNode(5, 2, 2));
  RemoteNodeHandle* node = host.node(5);
  TEST_ASSERT_NOT_NULL(node);
  TEST_ASSERT_TRUE(node->enabled());
  TEST_ASSERT_EQUAL_size_t(2, host.nodeCount());

  // disable
  node->setEnabled(false);
  TEST_ASSERT_FALSE(node->enabled());

  // geometry change while out of service
  TEST_ASSERT_EQUAL(CS::kOk, host.setRemoteNodeGeometry(5, 5, 4));
  TEST_ASSERT_EQUAL_size_t(5, node->inputLength());
  TEST_ASSERT_EQUAL_size_t(4, node->outputLength());
  // Disable is control state and geometry change does not touch it.
  TEST_ASSERT_FALSE_MESSAGE(node->enabled(),
                            "geometry change re-enabled a disabled node");

  // enable, and let it take its turn on the wire
  node->setEnabled(true);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }
  bool addressed = false;
  for (uint32_t t = 701; t <= 3000 && !addressed; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.ua == 5 + kUaOffset) {
        addressed = true;
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(addressed, "a re-enabled node was never addressed");

  // disable, then delete
  node->setEnabled(false);
  TEST_ASSERT_EQUAL(CS::kOk, host.deleteRemoteNode(5));
  TEST_ASSERT_NULL(host.node(5));
  TEST_ASSERT_EQUAL_size_t(1, host.nodeCount());

  // add again: the address is reusable, and the new device inherits
  // nothing -- not the geometry, not the disable, not the counters.
  TEST_ASSERT_EQUAL(CS::kOk, host.addRemoteNode(5, 1, 1));
  RemoteNodeHandle* reborn = host.node(5);
  TEST_ASSERT_NOT_NULL(reborn);
  TEST_ASSERT_EQUAL_size_t(1, reborn->inputLength());
  TEST_ASSERT_EQUAL_size_t(1, reborn->outputLength());
  TEST_ASSERT_TRUE_MESSAGE(reborn->enabled(),
                           "a reused slot inherited the previous disable");
  TEST_ASSERT_EQUAL_UINT32(0, reborn->statistics().exchanges);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, reborn->state());

  // The bystander was never disturbed by any of it.
  TEST_ASSERT_NOT_NULL(host.node(9));
  TEST_ASSERT_EQUAL_UINT8(9, host.node(9)->address());
  TEST_ASSERT_EQUAL_size_t(2, host.nodeCount());
}

// --------------------------------------------------- anti-starvation (#41)

// Reproduces the map issue #41 defect: a node whose outputs are marked
// dirty on essentially every scheduling turn must still get a real P
// within maxOutputPreemptMs, even sharing the rotation with a second,
// permanently silent node whose 250 ms reply-gate timeout would otherwise
// stretch the round-robin cycle into lockstep with the dirty cadence.
static void test_dirty_output_cannot_starve_poll_forever(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig liveConfig;
  liveConfig.inputBytes = 2;
  liveConfig.outputBytes = 1;
  RemoteNodeConfig deadConfig;
  deadConfig.inputBytes = 0;
  deadConfig.outputBytes = 0;
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.addRemoteNode(5, liveConfig));  // output-heavy
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.addRemoteNode(6, deadConfig));  // silent, no script
  RemoteNodeHandle* live = host.node(5);
  TEST_ASSERT_NOT_NULL(live);
  host.begin();

  // Prime both nodes past their I -> T preambles.
  runUntil(host, 0, 1020);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }  // drain the preamble

  // Node 5 always replies to a poll; node 6 never does (no script
  // entry -> silent, exactly like an unconfigured phantom node).
  transport.onSendReplyPacket(5 + kUaOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);

  // Dirty node 5's output on (almost) every millisecond -- far faster
  // than any real round-trip could keep up with, modeling the resonance
  // between an example sketch's output-update timer and a stretched
  // round-robin cycle.
  uint32_t lastExchanges = live->statistics().exchanges;
  uint32_t sinceLastExchangeMs = 0;
  bool everExchanged = false;
  for (uint32_t t = 1021; t <= 5000; ++t) {
    live->setOutputBit(0, (t % 2) == 0);  // keep outputsDirty_ true forever
    host.tick(t);
    if (live->statistics().exchanges != lastExchanges) {
      lastExchanges = live->statistics().exchanges;
      sinceLastExchangeMs = 0;
      everExchanged = true;
    } else {
      ++sinceLastExchangeMs;
    }
    // The anti-starvation bound guarantees a poll is forced within
    // maxOutputPreemptMs of the last one; allow generous slack for the
    // dead node's timeout/backoff and pacing overhead sharing the cycle.
    TEST_ASSERT_TRUE_MESSAGE(sinceLastExchangeMs <= 1000,
                             "node 5 went starved of real exchanges");
  }
  TEST_ASSERT_TRUE_MESSAGE(everExchanged,
                           "node 5 never completed a single P/R exchange");
  TEST_ASSERT_TRUE(live->statistics().exchanges > 1);
}

// The mirror-image failure: forcing a poll whenever it is "overdue" must
// not itself starve legitimate output transmits once the round-robin's
// baseline cycle time is kept short by backoff on the dead node.
static void test_anti_starvation_does_not_starve_transmit(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig liveConfig;
  liveConfig.inputBytes = 2;
  liveConfig.outputBytes = 1;
  RemoteNodeConfig deadConfig;
  deadConfig.outputBytes = 0;
  host.addRemoteNode(5, liveConfig);
  host.addRemoteNode(6, deadConfig);  // permanently silent
  RemoteNodeHandle* live = host.node(5);
  TEST_ASSERT_NOT_NULL(live);
  host.begin();
  runUntil(host, 0, 1020);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }

  transport.onSendReplyPacket(5 + kUaOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);

  // Set one real output change and confirm it is delivered promptly
  // (well inside the anti-starvation bound), even though node 6 keeps
  // timing out and backing off in the same rotation.
  live->setOutputBit(0, true);
  bool sawTransmit = false;
  for (uint32_t t = 1021; t <= 2000 && !sawTransmit; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt == 'T') {
        sawTransmit = true;
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(sawTransmit,
                           "a real output change never made it onto the wire");
}

// Poll backoff: a chronically missing node's poll attempts must thin
// out over time (doubling backoff), and recovery must be prompt once it
// starts answering. A lone-node rig can't observe this: with nothing
// else to poll, selectNextNode_'s never-stall fallback always bypasses
// backoff immediately (by design -- see its header comment), so a
// second, genuinely-live node has to be in the rotation to make the
// skip actually bite. That live node's replies are delivered by direct
// injection (bypassing MockCMRITransport's single-head script queue,
// see matchScript_), leaving the one script slot free for node 6's
// silence-then-recover sequence.
static void test_poll_backoff_doubles_and_clears_on_reply(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig liveConfig;   // node 5: always answered, below
  RemoteNodeConfig deadConfig;   // node 6: scripted silence then reply
  host.addRemoteNode(5, liveConfig);
  host.addRemoteNode(6, deadConfig);
  RemoteNodeHandle* dead = host.node(6);
  TEST_ASSERT_NOT_NULL(dead);
  host.begin();
  runUntil(host, 0, 1020);  // both nodes past their I -> T preamble
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }

  // Node 6 stays silent for a long run (self-answers never scripted
  // here); node 5 is kept alive by injecting its reply the instant it
  // is polled, every time, so it is always available as the
  // non-backed-off alternative selectNextNode_ needs to actually skip
  // node 6 rather than fall back to bypassing backoff.
  uint32_t node6PollsEarly = 0, node6PollsLate = 0;
  for (uint32_t t = 1021; t <= 4020; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt == 'P' && sent.ua == 5 + kUaOffset) {
        transport.injectPacketAt(makePacket(5, 'R'), t);
      } else if (sent.mt == 'P' && sent.ua == 6 + kUaOffset) {
        // Equal-length windows (1000 ms each) with a gap between them,
        // so a genuinely thinning rate shows up even though backoff
        // needs time to ramp up -- comparing raw counts over unequal
        // windows would not isolate the rate change.
        if (t >= 1021 && t < 2021) {
          ++node6PollsEarly;
        } else if (t >= 3020 && t < 4020) {
          ++node6PollsLate;
        }
      }
    }
  }
  TEST_ASSERT_TRUE(dead->statistics().noReplies >= 3);
  // The doubling backoff must make later polls to node 6 markedly less
  // frequent than early ones, while node 5's own cadence stays fast
  // throughout (proving the rotation isn't just slow overall).
  TEST_ASSERT_TRUE_MESSAGE(node6PollsLate < node6PollsEarly,
                           "node 6 poll attempts did not thin out over time");

  // Now let node 6 answer: backoff must clear immediately rather than
  // waiting out whatever multi-second window it had reached.
  transport.onSendReplyPacket(6 + kUaOffset, 'P', makePacket(6, 'R'), 0,
                              MockCMRITransport::kRepeatForever);
  const uint32_t exchangesBefore = dead->statistics().exchanges;
  bool recovered = false;
  for (uint32_t t = 4001; t <= 12000 && !recovered; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt == 'P' && sent.ua == 5 + kUaOffset) {
        transport.injectPacketAt(makePacket(5, 'R'), t);
      }
    }
    recovered = dead->statistics().exchanges > exchangesBefore;
  }
  TEST_ASSERT_TRUE_MESSAGE(recovered, "node 6 did not recover after its backoff expired");

  // Recovery clears the backoff outright rather than ramping it down:
  // once online, node 6 gets polled again at ordinary (fast) cadence,
  // not still throttled by its prior backoff.
  const uint32_t exchangesAfterFirst = dead->statistics().exchanges;
  for (uint32_t t = 12001; t <= 12300; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt == 'P' && sent.ua == 5 + kUaOffset) {
        transport.injectPacketAt(makePacket(5, 'R'), t);
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(dead->statistics().exchanges > exchangesAfterFirst,
                           "node 6 stayed throttled after recovery");
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
  RUN_TEST(test_initial_axes_and_predicates_before_first_reply);
  RUN_TEST(test_turnaround_is_measured_from_send_complete);
  RUN_TEST(test_default_reply_gate_is_250ms);
  RUN_TEST(test_policy_overrides_reply_gate);
  RUN_TEST(test_reply_gate_opens_at_send_complete_not_accept);
  RUN_TEST(test_wrong_ua_reply_is_rejected);
  RUN_TEST(test_wrong_mt_reply_is_rejected);
  RUN_TEST(test_wrong_length_reply_counts_error_without_commit);
  RUN_TEST(test_wrong_geometry_after_silent_run_reports_stale);
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
  RUN_TEST(test_rejected_add_does_not_poison_later_adds);
  RUN_TEST(test_node_table_capacity_is_enforced);
  RUN_TEST(test_add_after_begin_bootstraps_the_new_node);
  RUN_TEST(test_delete_removes_the_node_from_the_rotation);
  RUN_TEST(test_delete_while_in_flight_attributes_nothing);
  RUN_TEST(test_delete_frees_a_slot_in_a_full_table);
  RUN_TEST(test_reused_slot_is_a_new_subject);
  RUN_TEST(test_geometry_change_invalidates_image_and_forces_reinit);
  RUN_TEST(test_full_mutation_lifecycle);
  RUN_TEST(test_dirty_output_cannot_starve_poll_forever);
  RUN_TEST(test_anti_starvation_does_not_starve_transmit);
  RUN_TEST(test_poll_backoff_doubles_and_clears_on_reply);
  return UNITY_END();
}
