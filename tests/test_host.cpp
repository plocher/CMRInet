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
#include "transport/mock.h"
#include "unity.h"

using CMRInet::Age;
using CMRInet::attributionOf;
using CMRInet::CMRIHost;
using CMRInet::CMRIHostConfig;
using CMRInet::CMRIHostEvent;
using CMRInet::CMRIHostEventType;
using CMRInet::CMRIHostStatistics;
using CMRInet::CMRIPacket;
using CMRInet::ConformanceFault;
using CMRInet::ConformanceFaultRecord;
using CMRInet::ConformanceLayer;
using CMRInet::encodeFrame;
using CMRInet::FaultAttribution;
using CMRInet::HostExchangeKind;
using CMRInet::HostExchangeOutcome;
using CMRInet::kWireUAOffset;
using CMRInet::layerOf;
using CMRInet::MockCMRITransport;
using CMRInet::RemoteNodeConfig;
using CMRInet::RemoteNodeConformance;
using CMRInet::RemoteNodeHandle;
using CMRInet::RemoteNodeImageState;
using CMRInet::RemoteNodeLiveness;
using CMRInet::RemoteNodeState;
using CMRInet::RemoteNodeStatistics;
using CMRInet::SminiInit;
using CMRInet::CpnodeInit;
using CMRInet::NodeType;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- helpers

/// Build a packet for node UA `addr` (wire UA = addr + 65).
static CMRIPacket makePacket(uint8_t addr, uint8_t mt,
                             const uint8_t* body = nullptr, size_t len = 0) {
  CMRIPacket p;
  p.wireUA = static_cast<uint8_t>(addr + kWireUAOffset);
  p.mt = mt;
  TEST_ASSERT_TRUE_MESSAGE(p.setBody(body, len), "setBody rejected test body");
  return p;
}

/// Build a packet with an explicit raw wire-UA byte (for illegal-UA
/// tests where the byte is outside [65, 192]).
static CMRIPacket makePacketWithWireUA(uint8_t wireUA, uint8_t mt,
                                        const uint8_t* body = nullptr,
                                        size_t len = 0) {
  CMRIPacket p;
  p.wireUA = wireUA;
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

/// One host + mock rig with a single node at UA 5.
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
    // this call's status, not the handle; node(UA) does.
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
  /// Classification carried by the last kReplyRejected event. Recorded
  /// separately from the node's stored axis so a test can show a fault
  /// being *reported* without being *attributed*.
  ConformanceFault lastFault = ConformanceFault::kNone;
  uint16_t lastExpectedLength = 0;
  uint16_t lastReplyLength = 0;
  /// Conformance breaker transitions (D17).
  int breakerTripped = 0;
  int breakerClosed = 0;
  /// Runtime node-table mutation events (issue #91).
  int nodeAdded = 0;
  int nodeDeleted = 0;
  int geometryChanged = 0;
  uint8_t lastDepartedAddress = 0;
  uint16_t lastPreviousInputBytes = 0;
  uint16_t lastPreviousOutputBytes = 0;
  /// Illegal wire-UA events (issue #96).
  int illegalWireUA = 0;
  uint8_t lastIllegalWireUA = 0;
  /// Exchange completion / unsolicited (issue #112).
  int exchangeComplete = 0;
  int unsolicited = 0;
  HostExchangeKind lastKind = HostExchangeKind::kNone;
  HostExchangeKind lastPrevKind = HostExchangeKind::kNone;
  HostExchangeOutcome lastOutcome = HostExchangeOutcome::kNone;
  uint32_t lastGateMs = 0;
};

static void recordEvent(void* context, const CMRIHostEvent& event) {
  ListenerLog& log = *static_cast<ListenerLog*>(context);
  switch (event.type) {
    case CMRIHostEventType::kReplyAccepted: ++log.accepted; break;
    case CMRIHostEventType::kReplyRejected:
      ++log.rejected;
      log.lastFault = event.fault;
      log.lastExpectedLength = event.expectedLength;
      log.lastReplyLength = event.replyLength;
      break;
    case CMRIHostEventType::kReplyTimeout: ++log.timeouts; break;
    case CMRIHostEventType::kReinitScheduled: ++log.reinitScheduled; break;
    case CMRIHostEventType::kPollBackoffChanged: break;
    case CMRIHostEventType::kNodeStateChanged:
      ++log.stateChanges;
      log.lastPreviousState = event.previousState;
      log.lastNewState = event.newState;
      break;
    case CMRIHostEventType::kBreakerTripped: ++log.breakerTripped; break;
    case CMRIHostEventType::kBreakerClosed: ++log.breakerClosed; break;
    case CMRIHostEventType::kNodeAdded: ++log.nodeAdded; break;
    case CMRIHostEventType::kNodeDeleted:
      ++log.nodeDeleted;
      log.lastDepartedAddress = event.departedUA;
      break;
    case CMRIHostEventType::kGeometryChanged:
      ++log.geometryChanged;
      log.lastPreviousInputBytes = event.previousInputBytes;
      log.lastPreviousOutputBytes = event.previousOutputBytes;
      break;
    case CMRIHostEventType::kIllegalWireUA:
      ++log.illegalWireUA;
      log.lastIllegalWireUA = event.replyWireUA;
      log.lastFault = event.fault;
      break;
    case CMRIHostEventType::kExchangeComplete:
      ++log.exchangeComplete;
      log.lastKind = event.kind;
      log.lastPrevKind = event.prevKind;
      log.lastOutcome = event.outcome;
      log.lastGateMs = event.gateMs;
      break;
    case CMRIHostEventType::kUnsolicitedPacket:
      ++log.unsolicited;
      log.lastKind = event.kind;
      log.lastPrevKind = event.prevKind;
      log.lastGateMs = event.gateMs;
      log.lastReplyLength = event.replyLength;
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
  TEST_ASSERT_EQUAL_HEX8(5 + kWireUAOffset, sent.wireUA);
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
  TEST_ASSERT_EQUAL_HEX8(5 + kWireUAOffset, sent.wireUA);
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
  rig.node->setOutputBit(0, 3, true);
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
  rig.transport.onSendReplyPacket(5 + kWireUAOffset, 'P', makePacket(5, 'R'), 0,
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
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->inputByte(1));
  TEST_ASSERT_TRUE(rig.node->inputBit(0, 0));
  TEST_ASSERT_FALSE(rig.node->inputBit(0, 1));
  TEST_ASSERT_TRUE(rig.node->inputBit(0, 2));
  TEST_ASSERT_TRUE(rig.node->inputBit(1, 0));
  TEST_ASSERT_FALSE(rig.node->inputBit(1, 1));
  TEST_ASSERT_FALSE(rig.node->inputBit(124, 7));  // byte 124 bit 7 = bit 999, out of range
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kFresh, rig.node->imageState());
  // A reply of the declared length is current positive evidence, so the
  // conformance axis leaves kUnknown for the first time (#85).
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming,
                    rig.node->conformance());
  TEST_ASSERT_TRUE(rig.node->inputsUsable());
  // And that is what lets isHealthy() become true at all. While
  // conformance was inert the predicate could never fire, which is why
  // #84 staged this proof forward to #85.
  TEST_ASSERT_TRUE(rig.node->isHealthy());
  // L1: a geometry was demonstrated, and it agrees with the claim.
  TEST_ASSERT_EQUAL_UINT16(2, rig.node->observedInputBytes());
  TEST_ASSERT_EQUAL(ConformanceFault::kNone,
                    rig.node->lastConformanceFault().fault);
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
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
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

// issue #112: a fast Node's R can land in the RX queue while the Host is
// still in kAwaitSendComplete (transport.tick completed TX this same
// tick; drainReceive_ runs before runSchedule_ arms the reply gate).
// That R is the solicited poll reply — not unsolicited — and must close
// the exchange without a 250 ms false miss.
static void test_poll_r_accepted_during_await_send_complete(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);

  // Hold sendComplete false for a few ms so drainReceive_ sees the R
  // while phase is still kAwaitSendComplete.
  rig.transport.setSendLatencyMs(5);
  runUntil(rig.host, base, base);  // P accepted; not yet complete
  TEST_ASSERT_EQUAL_UINT32(1, rig.host.statistics().pollsSent);

  const CMRIHostStatistics before = rig.host.statistics();
  TEST_ASSERT_TRUE(rig.transport.injectPacketAt(
      makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), base + 2));
  runUntil(rig.host, base + 1, base + 6);

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      before.unsolicitedPackets, rig.host.statistics().unsolicitedPackets,
      "fast R during kAwaitSendComplete must not count as unsolicited");
  TEST_ASSERT_EQUAL_INT(0, log.unsolicited);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_INT(1, log.accepted);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
}

// ------------------------------------------------------- reply verification

static void test_wrong_ua_reply_is_rejected(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 250);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_TRUE(rig.host.statistics().repliesRejected >= 1);
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, rig.node->state());
  // The conformance axis does not move. A reply carrying somebody
  // else's UA is, definitionally, somebody else's -- charging it to the
  // node we happened to be polling would be an attribution error, and
  // nothing about *this* node's geometry was demonstrated either.
  // VALIDATION: Design v1.4 D14: packet-rung observations are reported
  // without moving the Node's stored conformance verdict.
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kUnknown, rig.node->conformance());
  TEST_ASSERT_EQUAL_UINT16(RemoteNodeHandle::kGeometryNeverObserved,
                           rig.node->observedInputBytes());
  TEST_ASSERT_EQUAL(ConformanceFault::kNone,
                    rig.node->lastConformanceFault().fault);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().errors);
}

static void test_wrong_mt_reply_is_rejected(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(5 + kWireUAOffset, 'P', makePacket(5, 'E'));
  runUntil(rig.host, base, base + 250);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().noReplies);
  TEST_ASSERT_TRUE(rig.host.statistics().repliesRejected >= 1);
  // Reported but not attributed: the event names the fault and its
  // rung, while the node's stored axis is untouched. See
  // test_self_echoed_poll_does_not_make_the_node_nonconforming for why
  // this branch in particular must not move the axis.
  TEST_ASSERT_EQUAL(ConformanceFault::kPacketUnexpectedType, log.lastFault);
  TEST_ASSERT_EQUAL(ConformanceLayer::kPacket, layerOf(log.lastFault));
  // No length comparison was made, so there is no expected length.
  TEST_ASSERT_EQUAL_UINT16(0, log.lastExpectedLength);
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kUnknown, rig.node->conformance());
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().errors);
}

static void test_wrong_length_reply_counts_error_without_commit(void) {
  Rig rig;  // node expects 2 input bytes
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)));
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().errors);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().noReplies);
  TEST_ASSERT_EQUAL_HEX8(0x00, rig.node->inputByte(0));  // never committed
  // Was UNINITIALIZED until #85, which reads as "hasn't started yet" for
  // a node that is in fact answering every poll with the wrong shape.
  // The disagreement now has a name.
  TEST_ASSERT_EQUAL(RemoteNodeState::kMisconfigured, rig.node->state());
}

// ------------------------------------------- conformance (D14 L1, D16)

static void test_declared_geometry_disagreement_is_observed_and_reported(void) {
  // The #80 bench failure reproduced end to end: a node declared NI=4
  // against physically 3-byte hardware. Every reply is rejected and no
  // data is ever committed. The Host used to report UNINITIALIZED,
  // which names the wrong problem and is a large part of why this hid.
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), /*inputBytes=*/4);
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)),
      0, 1);
  runUntil(rig.host, base, base + 2);

  // Detected and stored, not merely counted.
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    rig.node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeState::kMisconfigured, rig.node->state());

  // No data committed (interop 2.2.8), and the node is answering, so
  // this is emphatically not a liveness problem -- the distinction the
  // old UNINITIALIZED reading destroyed.
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().exchanges);
  TEST_ASSERT_EQUAL_HEX8(0x00, rig.node->inputByte(0));
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, rig.node->imageState());

  // Expected against actual, both durable on the handle: the detail an
  // operator needs to fix the declaration without reflashing the node.
  TEST_ASSERT_EQUAL_UINT16(3, rig.node->observedInputBytes());
  const ConformanceFaultRecord& fault = rig.node->lastConformanceFault();
  TEST_ASSERT_EQUAL(ConformanceFault::kImageGeometryMismatch, fault.fault);
  TEST_ASSERT_EQUAL_UINT16(4, fault.expected);
  TEST_ASSERT_EQUAL_UINT16(3, fault.observed);

  // Classified, not just named. Image rung, and a disagreement rather
  // than a defect -- so the remedy is a configuration change, and an
  // operator sent to reflash firmware would be sent to the wrong place.
  // VALIDATION: Design v1.3 D14: geometry mismatch is a disagreement;
  // layer and attribution are derived from the fault, not stored.
  TEST_ASSERT_EQUAL(ConformanceLayer::kImage, layerOf(fault.fault));
  TEST_ASSERT_EQUAL(FaultAttribution::kDisagreement,
                    attributionOf(fault.fault));

  // The same detail reaches a listener live.
  TEST_ASSERT_EQUAL(ConformanceFault::kImageGeometryMismatch, log.lastFault);
  TEST_ASSERT_EQUAL_UINT16(4, log.lastExpectedLength);
  TEST_ASSERT_EQUAL_UINT16(3, log.lastReplyLength);

  TEST_ASSERT_FALSE(rig.node->isHealthy());
  TEST_ASSERT_FALSE(rig.node->inputsUsable());
}

static void test_conformance_decays_to_unknown_when_contact_is_lost(void) {
  // Conformance is current evidence, never latched. A node that has
  // gone silent cannot be asserted nonconforming, because the Host
  // cannot experience a misconfiguration it cannot reach -- which is
  // also what makes OFFLINE and MISCONFIGURED mutually exclusive and
  // removes any ordering question between them.
  // VALIDATION: Design v1.3 D16: loss of contact degrades conformance
  // to unknown; history survives in the observation substrate.
  Rig rig(CMRIHostConfig(), CMRIHost::RemoteNodePolicy(), /*inputBytes=*/4);
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)),
      0, 1);
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    rig.node->conformance());

  // Now it stops answering entirely.
  rig.transport.onSendStaySilent(5 + kWireUAOffset, 'P',
                                 MockCMRITransport::kRepeatForever);
  bool silent = false;
  for (uint32_t t = base + 3; t <= base + 25000 && !silent; ++t) {
    rig.host.tick(t);
    silent = rig.node->liveness() == RemoteNodeLiveness::kSilent;
  }
  TEST_ASSERT_TRUE_MESSAGE(silent, "node did not reach silent liveness");
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kUnknown, rig.node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeState::kOffline, rig.node->state());

  // History is not lost, it moved to where D15 keeps history.
  TEST_ASSERT_EQUAL(ConformanceFault::kImageGeometryMismatch,
                    rig.node->lastConformanceFault().fault);
  TEST_ASSERT_EQUAL_UINT16(3, rig.node->observedInputBytes());
  TEST_ASSERT_TRUE(rig.node->statistics().errors >= 1);
}

static void test_degraded_when_a_conforming_node_starts_faulting(void) {
  // DEGRADED is the node that commits data while faulting. Reached by
  // interleaving: a good reply, then a wrong-shaped one while the
  // committed image is still fresh, then a good one again -- which also
  // shows the axis is not latched in either direction.
  Rig rig;  // 2 input bytes, stalenessMs 0, so a marked image stays fresh
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)),
      0, 1);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);

  uint32_t t = base;
  bool committed = false;
  for (; t <= base + 5000 && !committed; ++t) {
    rig.host.tick(t);
    committed = rig.node->statistics().exchanges >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(committed, "first good reply never committed");
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
  TEST_ASSERT_TRUE(rig.node->isHealthy());

  bool faulted = false;
  for (; t <= base + 5000 && !faulted; ++t) {
    rig.host.tick(t);
    faulted = rig.node->statistics().errors >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(faulted, "wrong-geometry reply never landed");
  // Faulting, but the image it already committed is still fresh and the
  // node is still answering: that is DEGRADED rather than MISCONFIGURED.
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    rig.node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kFresh, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeState::kDegraded, rig.node->state());
  // A nonconforming node's image must not be acted on even while fresh.
  TEST_ASSERT_FALSE(rig.node->inputsUsable());
  TEST_ASSERT_FALSE(rig.node->isHealthy());
  // The last good bytes are still readable; the fault did not overwrite
  // them with the wrong-shaped body.
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));

  bool recovered = false;
  for (; t <= base + 5000 && !recovered; ++t) {
    rig.host.tick(t);
    recovered = rig.node->statistics().exchanges >= 2;
  }
  TEST_ASSERT_TRUE_MESSAGE(recovered, "second good reply never committed");
  // Current evidence, so a conforming reply takes the verdict back.
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming,
                    rig.node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
  TEST_ASSERT_TRUE(rig.node->isHealthy());
  // The fault record is observation, so it survives the recovery.
  TEST_ASSERT_EQUAL(ConformanceFault::kImageGeometryMismatch,
                    rig.node->lastConformanceFault().fault);
}

static void test_nonconforming_node_reaches_stale_before_misconfigured(void) {
  // The middle rung of D16's chronology, which the v1.3 projection made
  // unreachable by folding every non-fresh image into MISCONFIGURED. A
  // node that goes nonconforming while holding a valid image reaches
  // STALE when that image ages out -- rejected replies stop refreshing
  // freshness, and "your data is old" is the honest report for as long
  // as the last good image is still valid.
  // VALIDATION: Design v1.5 D16: a nonconforming node reaches STALE
  // first; MISCONFIGURED needs invalidation to clear the image, and on
  // this path only D17's corrective re-init can supply one.
  //
  // Suppressing the conformance ladder is what holds the node at the
  // middle rung. This used to raise missThreshold instead, which read as
  // "stop the re-init ladder from invalidating" but suppressed nothing:
  // interop 2.3.10's ladder is armed by silence, and a node answering
  // every poll never accumulates a miss, so that ladder was unreachable
  // at the default of 5 too. The setup implied a hazard that did not
  // exist while the real one went unnamed (#87).
  CMRIHostConfig config;
  config.conformanceReinitThreshold = 0xFFFFFFFFu;
  Rig rig(config, CMRIHost::RemoteNodePolicy(), 2, 0);
  RemoteNodeConfig staleConfig;
  staleConfig.inputBytes = 2;
  staleConfig.stalenessMs = 100;
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    rig.host.addRemoteNode(6, staleConfig));
  RemoteNodeHandle* node = rig.host.node(6);
  TEST_ASSERT_NOT_NULL(node);
  rig.node->setEnabled(false);  // only node 6 is polled
  rig.host.begin();

  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  rig.transport.onSendReplyPacket(
      6 + kWireUAOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  runUntil(rig.host, 0, 510);  // I -> settle -> T -> P -> reply
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming, node->conformance());

  // Wrong geometry arrives while the image is still inside its 100 ms
  // staleness window, and keeps arriving. Answering forever is the
  // honest shape of this path -- the node is alive with rearranged IO,
  // not dying -- and it keeps liveness out of the result below.
  rig.transport.onSendReplyPacket(
      6 + kWireUAOffset, 'P', makePacket(6, 'R', threeBytes, sizeof(threeBytes)),
      0, MockCMRITransport::kRepeatForever);
  uint32_t t = 511;
  bool faulted = false;
  for (; t <= 590 && !faulted; ++t) {
    rig.host.tick(t);
    faulted = node->statistics().errors >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(
      faulted, "geometry mismatch did not land inside the staleness window");
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kFresh, node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kDegraded, node->state());

  // Rejected replies never refresh freshness, so the image ages out.
  // Still nonconforming, still answering -- and now STALE, not
  // MISCONFIGURED.
  runUntil(rig.host, t, 900);
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kStale, node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kStale, node->state());

  // Never silent, and never a single miss: the proof that 2.3.10's
  // ladder was not merely suppressed here but structurally unreachable.
  // That is why D17's breaker is the only exit from this rung, and why
  // test_answering_nonconforming_node_reaches_misconfigured is the
  // continuation of this one rather than a separate concern.
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, node->liveness());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, node->statistics().noReplies,
      "the node fell silent, so the silence ladder was reachable after all");
}

static void test_wrong_geometry_after_invalidation_reports_misconfigured(void) {
  // The last rung: re-init invalidation clears the image, so a
  // nonconforming node with nothing valid left reports MISCONFIGURED.
  //
  // This assertion was pinned as STALE on #89, under inert conformance,
  // and is deliberately reversed here. A test written before the
  // deciding input existed cannot testify about what happens once it
  // does -- and STALE would conceal a geometry disagreement behind
  // "your data is old", the same concealment as UNINITIALIZED hiding
  // the #80 node.
  Rig rig;  // node expects 2 input bytes
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);

  // Establish a committed image first, so invalidation has something to
  // invalidate.
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());

  // Then a silent miss-run past the threshold (which fires the re-init
  // ladder and invalidates the image), and a return with wrong
  // geometry. The mismatch reply must not commit data.
  rig.transport.onSendStaySilent(5 + kWireUAOffset, 'P', 6);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)),
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

  // The reply ended the miss run, so liveness recovered -- this is not
  // the OFFLINE branch. The image axis reads kNone because invalidation
  // cleared it, which is what makes MISCONFIGURED reachable at all.
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kResponsive, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    rig.node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeState::kMisconfigured, rig.node->state());
  TEST_ASSERT_EQUAL(Age::kNeverMarked, rig.node->inputAgeMs(rejectAt));
  // The bytes themselves are still there (F15): invalidation is about
  // validity, not about erasing the buffer.
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
}

static void test_self_echoed_poll_does_not_make_the_node_nonconforming(void) {
  // On 2-wire media the Host sees its own frames. Its own P comes back
  // carrying the polled node's UA with mt 'P', landing in the
  // MT-mismatch branch. If that branch moved the conformance axis,
  // every node on every 2-wire Host would sit in DEGRADED permanently.
  // VALIDATION: Interop v1.1 2.3.5: verify UA and MT and discard
  // everything else -- including the Host's own echoed frames.
  // VALIDATION: Design v1.4 D14: packet-rung observations are reported
  // without being attributed to the polled Node.
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);

  // Establish a healthy, conforming node first, so the echo has a real
  // verdict to damage rather than an unset one.
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  uint32_t t = base;
  bool committed = false;
  for (; t <= base + 5000 && !committed; ++t) {
    rig.host.tick(t);
    committed = rig.node->statistics().exchanges >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(committed, "setup reply never committed");
  TEST_ASSERT_TRUE(rig.node->isHealthy());

  // Now the Host's own poll echoes back at it: same UA, mt 'P'.
  rig.transport.onSendReplyPacket(5 + kWireUAOffset, 'P', makePacket(5, 'P'), 0, 1);
  const uint32_t rejectedBefore = rig.host.statistics().repliesRejected;
  bool echoed = false;
  for (; t <= base + 5000 && !echoed; ++t) {
    rig.host.tick(t);
    echoed = rig.host.statistics().repliesRejected > rejectedBefore;
  }
  TEST_ASSERT_TRUE_MESSAGE(echoed, "self-echo was never rejected");

  // Reported: the listener sees the fault, correctly classified.
  TEST_ASSERT_EQUAL(ConformanceFault::kPacketUnexpectedType, log.lastFault);
  TEST_ASSERT_EQUAL(ConformanceLayer::kPacket, layerOf(log.lastFault));
  // Not attributed: the node keeps its verdict, its counters, and its
  // health. This is the assertion that would fail if the MT branch were
  // wired to the axis.
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming,
                    rig.node->conformance());
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().errors);
  TEST_ASSERT_EQUAL(ConformanceFault::kNone,
                    rig.node->lastConformanceFault().fault);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());
  TEST_ASSERT_TRUE(rig.node->isHealthy());
  // And the echo taught us nothing about geometry, so the L1 observation
  // still reflects the last real reply.
  TEST_ASSERT_EQUAL_UINT16(2, rig.node->observedInputBytes());
}

static void test_axes_and_predicates_diverge_at_missing_with_fresh_image(void) {
  // Axis independence, and the non-degenerate divergence of the two
  // predicates, in one scenario.
  //
  // This replaces #84's recorded criterion (kSilent with a surviving
  // image verdict), which #85 makes unsatisfiable: silent past
  // threshold means the ladder fired, which means invalidated, which
  // means kNone. This pairing is better evidence anyway -- it needs no
  // re-init ladder, and no liveness-derived implementation can produce
  // it. See the amendment recorded on #84.
  // VALIDATION: Design v1.4 D16: silent liveness now implies an
  // invalidated image, so axis independence is demonstrated at missing
  // liveness with a fresh image instead.
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  uint32_t t = base;
  bool committed = false;
  for (; t <= base + 5000 && !committed; ++t) {
    rig.host.tick(t);
    committed = rig.node->statistics().exchanges >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(committed, "setup reply never committed");

  // Exactly one miss: enough for kMissing, well short of the threshold
  // that would invalidate the image. Assert the instant it lands, before
  // a second miss accumulates.
  bool missed = false;
  for (; t <= base + 5000 && !missed; ++t) {
    rig.host.tick(t);
    missed = rig.node->statistics().noReplies >= 1;
  }
  TEST_ASSERT_TRUE_MESSAGE(missed, "no miss inside the expected window");

  // Independence: liveness has moved off kResponsive while the image
  // axis holds its own verdict, and the projection still reads ONLINE.
  // A state derived from liveness could not produce this pairing.
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kMissing, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kFresh, rig.node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming,
                    rig.node->conformance());
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, rig.node->state());

  // Divergence, and non-degenerate: conformance is genuinely set to
  // kConforming, so the predicates disagree because they answer
  // different questions, not because an input is missing. The
  // application may act on this image; the operator should still be
  // told the node is missing polls.
  //
  // The divergence comes from *liveness*, and it has to. #85 originally
  // asked for it to arise from a conformance fault, which cannot
  // happen: a real fault sets kNonconforming, which fails both
  // predicates together, and the only conformance value that separates
  // them is kUnknown -- the unset case that criterion excluded. That is
  // correct behaviour rather than a gap: once geometry disagrees, the
  // committed bytes are of doubtful meaning and the application should
  // not act on them either. The conformance *domain* will separate the
  // predicates once D17's breaker has a writer, since isHealthy() reads
  // the breaker and inputsUsable() does not.
  TEST_ASSERT_TRUE(rig.node->inputsUsable());
  TEST_ASSERT_FALSE(rig.node->isHealthy());
}

static void test_health_implies_usability_but_not_the_reverse(void) {
  // Divergence is one-directional. isHealthy() strictly implies
  // inputsUsable(): healthy requires fresh, responsive implies not
  // silent, and conforming implies not nonconforming. The operator
  // predicate is meant to be strictly stricter than the application
  // one -- but D16 claimed they diverge in *both* directions, which the
  // predicates cannot do. Corrected in v1.4 and pinned here.
  //
  // Checked at every state the schedule passes through rather than at a
  // few hand-picked points, then shown non-vacuous by requiring that
  // all three reachable combinations were actually visited.
  // VALIDATION: Design v1.4 D16: isHealthy() implies inputsUsable(), so
  // only usable-and-unhealthy occurs.
  Rig rig;
  const uint8_t threeBytes[] = {0x11, 0x22, 0x33};
  uint32_t base = primeToPoll(rig);
  // good -> wrong geometry -> good, then silence. That walks
  // uninitialized, healthy, degraded, healthy, and missing-with-a-fresh
  // image without needing to know which tick each lands on.
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', threeBytes, sizeof(threeBytes)),
      0, 1);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
      1);

  bool sawHealthy = false;
  bool sawUsableNotHealthy = false;
  bool sawNeither = false;
  for (uint32_t t = base; t <= base + 3000; ++t) {
    rig.host.tick(t);
    const bool healthy = rig.node->isHealthy();
    const bool usable = rig.node->inputsUsable();
    TEST_ASSERT_TRUE_MESSAGE(
        !healthy || usable,
        "isHealthy() was true while inputsUsable() was false");
    if (healthy) sawHealthy = true;
    if (usable && !healthy) sawUsableNotHealthy = true;
    if (!usable && !healthy) sawNeither = true;
  }
  // Without these the implication above could pass vacuously.
  TEST_ASSERT_TRUE_MESSAGE(sawHealthy, "never reached a healthy state");
  TEST_ASSERT_TRUE_MESSAGE(sawUsableNotHealthy,
                           "never reached usable-but-not-healthy");
  TEST_ASSERT_TRUE_MESSAGE(sawNeither, "never reached neither");
}

// ------------------------------------------- miss, recovery, re-init, health

static void test_recovery_after_miss(void) {
  Rig rig;
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendStaySilent(5 + kWireUAOffset, 'P', 1);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
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
  rig.transport.onSendStaySilent(5 + kWireUAOffset, 'P', 6);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
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
  rig.transport.onSendStaySilent(5 + kWireUAOffset, 'P',
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
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0, 1);
  runUntil(rig.host, base, base + 2);
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));
  // Force the re-init ladder: silence forever, run past six misses.
  rig.transport.onSendStaySilent(5 + kWireUAOffset, 'P',
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
  // This is the whole point of the test and is unchanged -- the hazard is
  // about the buffer, never about the validity verdict.
  TEST_ASSERT_EQUAL(Age::kNeverMarked, rig.node->inputAgeMs(sixMissesAt));
  TEST_ASSERT_EQUAL_HEX8(0xA5, rig.node->inputByte(0));  // NOT zeroed
  TEST_ASSERT_EQUAL_HEX8(0x01, rig.node->inputByte(1));
  // The image axis reads kNone, not kStale. Invalidation means "this
  // image is no longer valid" (interop 2.3.10), and the axis is a
  // validity claim rather than a history one -- "did this node ever
  // work" is statistics().exchanges, in the observation substrate where
  // D15 keeps history.
  //
  // This pairing used to carry the axis-independence proof, back when a
  // latching hasInputImage_ kept the verdict at kStale. It cannot any
  // more: silent past threshold implies the ladder fired implies
  // invalidated. Independence moved to
  // test_axes_and_predicates_diverge_at_missing_with_fresh_image, which
  // proves it without depending on invalidation at all. Amendment
  // recorded on #84.
  TEST_ASSERT_EQUAL(RemoteNodeLiveness::kSilent, rig.node->liveness());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, rig.node->imageState());
  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  // Projection maps this to OFFLINE because liveness dominates the
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
      6 + kWireUAOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)), 0,
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
  rig.transport.onSendReplyPacket(5 + kWireUAOffset, 'P', makePacket(5, 'R'), 0, 1);
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
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)), 0,
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
      if (pollSeen == 0) firstPollUa = sent.wireUA;
      else secondPollUa = sent.wireUA;
      ++pollSeen;
    }
  }
  TEST_ASSERT_EQUAL_INT(2, pollSeen);
  TEST_ASSERT_EQUAL_HEX8(1 + kWireUAOffset, firstPollUa);
  TEST_ASSERT_EQUAL_HEX8(2 + kWireUAOffset, secondPollUa);
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
  rig.transport.onSendReplyBytes(5 + kWireUAOffset, 'P', wire, n, /*delayMs=*/1,
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
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
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
      5 + kWireUAOffset, 'P', makePacket(6, 'R', kInputsA5, sizeof(kInputsA5)));
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
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
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

// Registration is legal at any time. It used to be refused after
// begin(), silently, which was coherent only while begin() locked the
// whole configuration -- and once D5 unlocked the node table it left
// registration as the sole mutator that failed without saying so.
// Unlocking it dissolves the problem rather than inventing a status for
// it, and is strictly widening: registering before begin() still works.
static void test_listener_registration_is_legal_at_runtime(void) {
  Rig rig;
  rig.host.begin();
  uint32_t base = primeToPoll(rig);

  // Attach only after the engine is already running.
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  rig.host.onTrace(recordTrace, &log);

  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 20);

  TEST_ASSERT_EQUAL_UINT32(1, rig.node->statistics().exchanges);
  TEST_ASSERT_TRUE_MESSAGE(log.accepted >= 1,
                           "a listener registered at runtime saw no events");
  TEST_ASSERT_TRUE_MESSAGE(log.txTraces >= 1,
                           "a trace listener registered at runtime saw no TX");
  TEST_ASSERT_TRUE_MESSAGE(log.rxTraces >= 1,
                           "a trace listener registered at runtime saw no RX");

  // And clearing at runtime works the same way.
  rig.host.onEvent(nullptr);
  rig.host.onTrace(nullptr);
  const int acceptedAfterClear = log.accepted;
  const int tracesAfterClear = log.txTraces;
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base + 21, base + 60);
  TEST_ASSERT_EQUAL_INT(acceptedAfterClear, log.accepted);
  TEST_ASSERT_EQUAL_INT(tracesAfterClear, log.txTraces);
}

static void test_null_listeners_are_harmless(void) {
  Rig rig;
  rig.host.onEvent(nullptr);
  rig.host.onTrace(nullptr);
  uint32_t base = primeToPoll(rig);
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
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

  TEST_ASSERT_EQUAL(CS::kUAOutOfRange, h.addRemoteNode(128, ok));
  TEST_ASSERT_EQUAL(CS::kInputBytesTooLarge, h.addRemoteNode(5, tooManyInputs));
  TEST_ASSERT_EQUAL(CS::kOutputBytesTooLarge,
                    h.addRemoteNode(5, tooManyOutputs));
  TEST_ASSERT_EQUAL_size_t(0, h.nodeCount());

  // A good add still succeeds after all of those rejections.
  TEST_ASSERT_EQUAL(CS::kOk, h.addRemoteNode(5, ok));
  TEST_ASSERT_EQUAL_size_t(1, h.nodeCount());
  TEST_ASSERT_NOT_NULL(h.node(5));

  // A duplicate UA is rejected without disturbing the original.
  TEST_ASSERT_EQUAL(CS::kUAInUse, h.addRemoteNode(5, ok));
  TEST_ASSERT_EQUAL_size_t(1, h.nodeCount());

  // begin() is the config->running transition, not a table lock: the
  // same add is judged on its merits before and after (Design v1.2 D5).
  h.begin();
  TEST_ASSERT_EQUAL(CS::kOk, h.addRemoteNode(6, ok));
  TEST_ASSERT_EQUAL_size_t(2, h.nodeCount());
  TEST_ASSERT_NOT_NULL(h.node(6));

  // The same validations still apply at runtime.
  TEST_ASSERT_EQUAL(CS::kUAInUse, h.addRemoteNode(6, ok));
  TEST_ASSERT_EQUAL(CS::kUAOutOfRange, h.addRemoteNode(200, ok));

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
      if (sent.wireUA != 6 + kWireUAOffset) {
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
      if (sent.wireUA == 5 + kWireUAOffset) {
        addressedTheDeletedNode = true;
      } else if (sent.wireUA == 6 + kWireUAOffset) {
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
      if (sent.mt == 'P' && sent.wireUA == 5 + kWireUAOffset) {
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

// Round-trip tripwire: addRemoteNode(a) must produce a handle
// whose UA() returns a and whose wireUA() returns a + kWireUAOffset.
// This is the defense against the encode line (node.wireUA_ = UA +
// kWireUAOffset) or the offset constant drifting, since #96 adds a
// second consumer of kWireUAOffset (isLegalWireUA). The test is
// pure storage assertion — no bus, no transport, no clock.
static void test_add_remote_node_ua_round_trip(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  RemoteNodeConfig config;
  config.inputBytes = 0;

  // Test across the UA range: 0, 1, 5, 63, 64, 127.
  const uint8_t kTestUAs[] = {0, 1, 5, 63, 64, 127};
  for (size_t i = 0; i < sizeof(kTestUAs) / sizeof(kTestUAs[0]); ++i) {
    const uint8_t ua = kTestUAs[i];
    TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                      host.addRemoteNode(ua, config));
    RemoteNodeHandle* handle = host.node(ua);
    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "addRemoteNode failed");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ua, handle->UA(),
                                  "UA() does not return the configured UA");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(
        static_cast<uint8_t>(ua + kWireUAOffset), handle->wireUA(),
        "wireUA() does not return UA + kWireUAOffset");
  }
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

  // Address 0 is the trap here: a cleaned tombstone holds UA_ == 0,
  // and 0 is a perfectly legal node UA. A lookup that tested the
  // UA before occupancy would hand this tombstone back.
  TEST_ASSERT_NULL_MESSAGE(host.node(0),
                           "a cleaned tombstone answered to UA 0");
}

// Slot reuse must produce a genuinely new subject. The hazard is quiet:
// inherited freshness makes a node that has never answered look ONLINE
// with data it never sent.
static void test_reused_slot_is_a_new_subject(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(5, 2, 0);
  host.begin();
  transport.onSendReplyPacket(5 + kWireUAOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);
  runUntil(host, 0, 1000);

  RemoteNodeHandle* before = host.node(5);
  TEST_ASSERT_NOT_NULL(before);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, before->state());
  TEST_ASSERT_TRUE(before->statistics().exchanges > 0);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.deleteRemoteNode(5));

  // A handle cached across the delete must be detectable as stale.
  // UA() is that self-check, and it works only because the slot is
  // cleaned at delete rather than at reuse.
  TEST_ASSERT_NOT_EQUAL_MESSAGE(5, before->UA(),
                                "a deleted node still answered to its UA");

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

// Geometry change is in place and identity-preserving: same UA,
// same counters, same handle. What it must invalidate is the cached
// image, because the NI/NO announced in the I body just changed.
static void test_geometry_change_invalidates_image_and_forces_reinit(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.addRemoteNode(5, 2, 1);
  host.begin();
  transport.onSendReplyPacket(5 + kWireUAOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);
  runUntil(host, 0, 1000);

  RemoteNodeHandle* node = host.node(5);
  TEST_ASSERT_NOT_NULL(node);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  node->setOutputBit(0, 0, true);
  TEST_ASSERT_TRUE(node->outputBit(0, 0));
  const uint32_t exchangesBefore = node->statistics().exchanges;
  TEST_ASSERT_TRUE(exchangesBefore > 0);
  CMRIPacket sent;
  while (transport.takeSent(sent)) { }

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.setRemoteNodeGeometry(5, 4, 3));

  // Identity preserved: same handle, same UA, same running totals.
  // This is the same logical device with its IO cards rearranged, so its
  // observation substrate keeps counting.
  TEST_ASSERT_EQUAL_PTR(node, host.node(5));
  TEST_ASSERT_EQUAL_UINT8(5, node->UA());
  TEST_ASSERT_EQUAL_UINT32(exchangesBefore, node->statistics().exchanges);

  // Belief cleared: the cached bytes described the old shape.
  TEST_ASSERT_EQUAL_size_t(4, node->inputLength());
  TEST_ASSERT_EQUAL_size_t(3, node->outputLength());
  TEST_ASSERT_EQUAL(RemoteNodeImageState::kNone, node->imageState());
  TEST_ASSERT_EQUAL(RemoteNodeState::kUninitialized, node->state());
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, node->inputAgeMs(1000));
  TEST_ASSERT_FALSE(node->inputsUsable());
  TEST_ASSERT_FALSE_MESSAGE(node->outputBit(0, 0),
                            "an output bit survived a geometry change");

  // The re-init ladder runs, and the new I announces the new NI/NO.
  bool sawInit = false;
  bool sawFullTransmit = false;
  for (uint32_t t = 1001; t <= 3000; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.wireUA != 5 + kWireUAOffset) {
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
// recommissioned, retired, and its UA handed to a new device.
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
      if (sent.wireUA == 5 + kWireUAOffset) {
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

  // add again: the UA is reusable, and the new device inherits
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
  TEST_ASSERT_EQUAL_UINT8(9, host.node(9)->UA());
  TEST_ASSERT_EQUAL_size_t(2, host.nodeCount());
}

// --------------------------------------- mutation events (#91)
//
// Runtime add, delete, and geometry change now fire CMRIHostEvents, so
// a listener sees the node table change whether the mutation came from a
// C&C verb or a direct API call. These tests drive the mutators directly
// (no shell) to prove the listener seam is the single source.

// A delete fires kNodeDeleted. The slot is cleaned before the event
// fires (D5), so the event carries the departing identity by value and
// its node pointer is null -- the listener can still name the node that
// left (issue #91).
static void test_delete_event_names_the_departing_node(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  ListenerLog log;
  host.onEvent(recordEvent, &log);
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(5, 2, 0));
  host.begin();
  host.tick(1000);  // establish a clock so the delete stamp is non-zero

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.deleteRemoteNode(5));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, log.nodeDeleted,
                                "delete did not fire kNodeDeleted");
  TEST_ASSERT_NULL_MESSAGE(log.lastNode,
                           "kNodeDeleted carried a non-null node pointer");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(5, log.lastDepartedAddress,
                                  "kNodeDeleted named the wrong UA");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      1000, log.lastEventMs,
      "delete event did not stamp the last-tick clock");
  TEST_ASSERT_EQUAL_size_t(0, host.nodeCount());
}

// A runtime add fires kNodeAdded with the new handle, so a listener sees
// the table grow even when the add is a direct API call, not a C&C verb
// (issue #91).
static void test_add_event_fires_for_runtime_add(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  host.begin();
  host.tick(500);  // running, with a clock
  ListenerLog log;
  host.onEvent(recordEvent, &log);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(6, 1, 2));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, log.nodeAdded,
                                "add did not fire kNodeAdded");
  TEST_ASSERT_NOT_NULL_MESSAGE(log.lastNode,
                                "kNodeAdded carried a null node pointer");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(6, log.lastNode->UA(),
                                  "kNodeAdded named the wrong node");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      500, log.lastEventMs,
      "add event did not stamp the last-tick clock");
}

// A geometry change fires kGeometryChanged carrying both the old and the
// new NI/NO, so a reader can tell what changed without dereferencing the
// handle (issue #91). The new geometry is on the handle; the old rides by
// value in the event.
static void test_geometry_event_carries_old_and_new_geometry(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  ListenerLog log;
  host.onEvent(recordEvent, &log);
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(5, 2, 1));
  host.begin();
  host.tick(100);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.setRemoteNodeGeometry(5, 4, 3));
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      1, log.geometryChanged,
      "geometry change did not fire kGeometryChanged");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      2, log.lastPreviousInputBytes,
      "kGeometryChanged lost the old input bytes");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      1, log.lastPreviousOutputBytes,
      "kGeometryChanged lost the old output bytes");
  // The handle already holds the new geometry.
  TEST_ASSERT_EQUAL_size_t(4, host.node(5)->inputLength());
  TEST_ASSERT_EQUAL_size_t(3, host.node(5)->outputLength());
}

// A rejected mutation fires no event: each mutator returns its
// ConfigStatus before reaching the event dispatch (issue #91). This is
// the invariant that lets the shell's verb handlers become thin wrappers
// -- validation lives in the mutator, and a failed call produces only the
// error line, not a spurious event.
static void test_rejected_mutation_fires_no_event(void) {
  using CS = CMRIHost::ConfigStatus;
  MockCMRITransport transport;
  CMRIHost host(transport);
  ListenerLog log;
  host.onEvent(recordEvent, &log);
  TEST_ASSERT_EQUAL(CS::kOk, host.addRemoteNode(5, 2, 0));
  host.begin();
  host.tick(100);
  // Reset so the successful add above is not read as a mutation event
  // from the rejected calls below.
  log.nodeAdded = 0;
  log.nodeDeleted = 0;
  log.geometryChanged = 0;

  // Duplicate UA.
  TEST_ASSERT_EQUAL(CS::kUAInUse, host.addRemoteNode(5, 1, 1));
  // No such node.
  TEST_ASSERT_EQUAL(CS::kNoSuchNode, host.deleteRemoteNode(99));
  TEST_ASSERT_EQUAL(CS::kNoSuchNode, host.setRemoteNodeGeometry(99, 1, 1));
  // Byte ceilings.
  TEST_ASSERT_EQUAL(CS::kInputBytesTooLarge,
                    host.setRemoteNodeGeometry(
                        5, RemoteNodeHandle::kMaxInputBytes + 1, 0));
  TEST_ASSERT_EQUAL(CS::kOutputBytesTooLarge,
                    host.setRemoteNodeGeometry(
                        5, 0, RemoteNodeHandle::kMaxOutputBytes + 1));

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, log.nodeAdded,
                                "a rejected add fired kNodeAdded");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, log.nodeDeleted,
                                "a rejected delete fired kNodeDeleted");
  TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, log.geometryChanged,
      "a rejected geometry change fired kGeometryChanged");
}

// A mutation before the first tick stamps nowMs=0 -- "no clock yet" --
// rather than a fabricated timestamp. The guard is what keeps the event
// stream honest when a sketch adds its compiled-in nodes before the host
// begins ticking (issue #91). Pre-begin adds are a legitimate boot-time
// path and are not refused.
static void test_mutation_before_first_tick_stamps_zero_clock(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  ListenerLog log;
  host.onEvent(recordEvent, &log);
  // No begin(), no tick(): lastTickMs_ is still its default 0.
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(5, 1, 1));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, log.nodeAdded,
                                "pre-tick add did not fire kNodeAdded");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, log.lastEventMs,
      "pre-tick mutation stamped a non-zero clock");
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
  transport.onSendReplyPacket(5 + kWireUAOffset, 'P',
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
    live->setOutputBit(0, 0, (t % 2) == 0);  // keep outputsDirty_ true forever
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

  transport.onSendReplyPacket(5 + kWireUAOffset, 'P',
                              makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)),
                              0, MockCMRITransport::kRepeatForever);

  // Set one real output change and confirm it is delivered promptly
  // (well inside the anti-starvation bound), even though node 6 keeps
  // timing out and backing off in the same rotation.
  live->setOutputBit(0, 0, true);
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
      if (sent.mt == 'P' && sent.wireUA == 5 + kWireUAOffset) {
        transport.injectPacketAt(makePacket(5, 'R'), t);
      } else if (sent.mt == 'P' && sent.wireUA == 6 + kWireUAOffset) {
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
  transport.onSendReplyPacket(6 + kWireUAOffset, 'P', makePacket(6, 'R'), 0,
                              MockCMRITransport::kRepeatForever);
  const uint32_t exchangesBefore = dead->statistics().exchanges;
  bool recovered = false;
  for (uint32_t t = 4001; t <= 12000 && !recovered; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt == 'P' && sent.wireUA == 5 + kWireUAOffset) {
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
      if (sent.mt == 'P' && sent.wireUA == 5 + kWireUAOffset) {
        transport.injectPacketAt(makePacket(5, 'R'), t);
      }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(dead->statistics().exchanges > exchangesAfterFirst,
                           "node 6 stayed throttled after recovery");
}

// ------------------------------------- D17: degraded service classes (#87)
//
// The mock's replay script is head-of-step matched, so a kRepeatForever
// step for one UA would block every other node's step forever. These
// multi-node scenarios therefore drive the mock directly: observe each
// completed send with takeSent() and answer it per the node's own
// policy. That also gives an exact per-node poll count, which is the
// same quantity the bench captures as `tx_polls` -- so the assertions
// below compare against the #80 baseline in its own units.

struct FaultyNode {
  enum Kind : uint8_t {
    kConforming,   ///< answers with the declared geometry
    kWrongLength,  ///< answers, but with the wrong body length
    kSilent,       ///< never answers
  };

  FaultyNode() = default;
  FaultyNode(uint8_t addr, Kind k, uint16_t actual)
      : UA(addr), kind(k), actualIn(actual) {}

  uint8_t UA = 0;
  Kind kind = kConforming;
  /// What the Node really sends. The Host's *declared* geometry is not
  /// mirrored here on purpose: the host table already holds it, and a
  /// second copy in the rig could disagree with it -- which is the
  /// declared-versus-observed confusion the whole D14 axis exists to
  /// resolve. The disagreement under test must come from the real
  /// configuration, not from the fixture.
  uint16_t actualIn = 0;
  uint32_t polls = 0;  ///< P frames addressed to this node
};

/// Tick the host over [fromMs, toMs], answering every poll according to
/// the matching node's policy. Returns with all counters updated.
static void pumpFaultyBus(CMRIHost& host, MockCMRITransport& transport,
                          FaultyNode* nodes, size_t count, uint32_t fromMs,
                          uint32_t toMs) {
  static const uint8_t kFiller[RemoteNodeHandle::kMaxInputBytes] = {0};
  CMRIPacket sent;
  for (uint32_t t = fromMs; t <= toMs; ++t) {
    host.tick(t);
    while (transport.takeSent(sent)) {
      if (sent.mt != 'P') {
        continue;  // I and T expect no reply (interop E8)
      }
      if (sent.wireUA < kWireUAOffset) {
        continue;
      }
      const uint8_t UA = static_cast<uint8_t>(sent.wireUA - kWireUAOffset);
      for (size_t i = 0; i < count; ++i) {
        if (nodes[i].UA != UA) {
          continue;
        }
        ++nodes[i].polls;
        if (nodes[i].kind != FaultyNode::kSilent) {
          transport.injectPacketAt(
              makePacket(UA, 'R', kFiller, nodes[i].actualIn), t);
        }
        break;
      }
    }
  }
}

// Done-when 1: degraded participation stays bounded with a mixed
// healthy/silent/nonconforming population.
//
// The #80 baseline is the thing being refuted. There a node declared
// NI=4 answering with 3 bytes took 1316 polls to a healthy node's 765 --
// it was serviced 1.7x MORE than the node doing real work, while
// committing nothing. Geometry mismatch cleared its poll backoff, so it
// was immediately eligible on every rotation.
// VALIDATION: Design v1.5 D17: degraded nodes have bounded, shared,
// predictable impact on healthy ones.
static void test_degraded_participation_is_bounded(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.degradedSlotSharePercent = 20;
  config.degradedBandwidthPercent = 10;
  CMRIHost host(transport, config);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(30, 7, 0));
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(31, 4, 0));
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(32, 4, 0));
  host.begin();

  FaultyNode nodes[3];
  nodes[0] = FaultyNode(30, FaultyNode::kConforming, 7);
  nodes[1] = FaultyNode(31, FaultyNode::kWrongLength, 3);  // the #80 node
  nodes[2] = FaultyNode(32, FaultyNode::kSilent, 4);

  pumpFaultyBus(host, transport, nodes, 3, 0, 60000);  // 60 s, as on the bench

  const RemoteNodeHandle* healthy = host.node(30);
  const RemoteNodeHandle* broken = host.node(31);
  const RemoteNodeHandle* silent = host.node(32);
  TEST_ASSERT_NOT_NULL(healthy);
  TEST_ASSERT_NOT_NULL(broken);
  TEST_ASSERT_NOT_NULL(silent);

  // The healthy node is doing real work.
  TEST_ASSERT_TRUE_MESSAGE(healthy->statistics().exchanges > 100,
                           "healthy node never got going");
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, healthy->state());

  // The inversion is gone. On the #80 baseline the ratio was 1.72 the
  // other way: the broken node was serviced MORE than the working one.
  TEST_ASSERT_TRUE_MESSAGE(
      nodes[1].polls < nodes[0].polls,
      "the nonconforming node is still outpolling the healthy one");

  // And bounded, not merely smaller.
  //
  // Under defaults this bound is held mostly by the *breaker*, not by
  // the gates: the nonconforming node exhausts its corrective re-inits
  // within a few seconds and then drops to one bare-P probe per probe
  // interval, which throttles far harder than a 20% slot share. The
  // gates carry the pre-trip window and the silent node. Both mechanisms
  // are in play here by design -- this is the end-to-end acceptance
  // check, and test_gates_alone_bound_degraded_share below isolates the
  // allocator's own contribution with the breaker suppressed.
  const uint32_t degradedPolls = nodes[1].polls + nodes[2].polls;
  const uint32_t totalPolls = nodes[0].polls + degradedPolls;
  TEST_ASSERT_TRUE_MESSAGE(totalPolls > 0, "nothing was polled at all");
  TEST_ASSERT_TRUE_MESSAGE(
      degradedPolls * 100u < totalPolls * 35u,
      "degraded nodes took more than a bounded share of the rotation");

  // The ledger agrees with the wire, and names which gate bound.
  const CMRIHostStatistics& stats = host.statistics();
  TEST_ASSERT_TRUE_MESSAGE(stats.degradedGrants > 0,
                           "degraded class was never served");
  TEST_ASSERT_TRUE_MESSAGE(
      stats.degradedSlotDenials + stats.degradedBandwidthDenials > 0,
      "no gate ever bound, so nothing was being bounded");

  // Neither degraded node committed anything, which is what makes their
  // former share pure waste.
  TEST_ASSERT_EQUAL_UINT32(0, broken->statistics().exchanges);
  TEST_ASSERT_EQUAL_UINT32(0, silent->statistics().exchanges);
}

// The allocator on its own, with the breaker held off so it cannot do
// the work and mask a broken gate.
//
// This is the test that actually measures Gate A. The end-to-end check
// above cannot: under defaults the breaker trips within seconds and
// throttles the nonconforming node an order of magnitude harder than a
// slot share would, so that test still passes with the gates removed
// entirely. Suppressing the conformance ladder here leaves the two gates
// as the only thing bounding a node that answers every poll.
// VALIDATION: Design v1.5 D17: Gate A is a signed slot credit whose
// equilibrium is the configured share.
static void test_gates_alone_bound_degraded_share(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.degradedSlotSharePercent = 20;
  config.degradedBandwidthPercent = 10;
  // The breaker must not participate: this test is about the allocator.
  config.conformanceReinitThreshold = 0xFFFFFFFFu;
  CMRIHost host(transport, config);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(30, 7, 0));
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(31, 4, 0));
  host.begin();

  FaultyNode nodes[2];
  nodes[0] = FaultyNode(30, FaultyNode::kConforming, 7);
  nodes[1] = FaultyNode(31, FaultyNode::kWrongLength, 3);
  pumpFaultyBus(host, transport, nodes, 2, 0, 60000);

  // The breaker really did stay out of it.
  TEST_ASSERT_FALSE_MESSAGE(host.node(31)->conformanceBreakerOpen(),
                            "the breaker engaged and masked the gates");

  const uint32_t totalPolls = nodes[0].polls + nodes[1].polls;
  TEST_ASSERT_TRUE_MESSAGE(totalPolls > 100, "the bus barely ran");

  // Equilibrium is the configured share. Ungated this node would take
  // roughly half the rotation, since it answers as promptly as the
  // healthy one and its geometry mismatch clears the liveness backoff.
  TEST_ASSERT_TRUE_MESSAGE(
      nodes[1].polls * 100u < totalPolls * 30u,
      "Gate A did not hold the degraded node near its configured share");
  TEST_ASSERT_TRUE_MESSAGE(
      nodes[1].polls * 100u > totalPolls * 8u,
      "the degraded node got far less than its share; it is over-throttled");

  // Gate A is the one that bound, which is the asymmetry D17 predicts:
  // an answering node is cheap in milliseconds and expensive in turns,
  // so a wall-clock budget alone would barely notice it.
  const CMRIHostStatistics& stats = host.statistics();
  TEST_ASSERT_TRUE_MESSAGE(stats.degradedSlotDenials > 0,
                           "Gate A never refused an answering node");
  TEST_ASSERT_TRUE_MESSAGE(
      stats.degradedSlotDenials > stats.degradedBandwidthDenials,
      "the wall-clock gate bound an answering node before the slot gate");
}

// Done-when 3: the degraded class is never starved to zero.
//
// The sharp case: Gate A is shut outright (share 0), so nothing but the
// ceiling clamp can admit a degraded node. Recovery is unobservable if
// this fails, because a node polled at zero rate can never demonstrate
// that it is fixed.
// VALIDATION: Design v1.5 D17: the ceiling clamp guarantees the degraded
// class is never starved to zero, and may deliberately exceed budget.
static void test_degraded_class_is_never_starved_to_zero(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.degradedSlotSharePercent = 0;   // Gate A always refuses
  config.degradedBandwidthPercent = 0;   // Gate B always refuses
  config.maxPollBackoffMs = 2000;        // the clamp under test
  CMRIHost host(transport, config);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(30, 7, 0));
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(31, 4, 0));
  host.begin();

  FaultyNode nodes[2];
  nodes[0] = FaultyNode(30, FaultyNode::kConforming, 7);
  nodes[1] = FaultyNode(31, FaultyNode::kWrongLength, 3);

  pumpFaultyBus(host, transport, nodes, 2, 0, 20000);

  // Served, despite both gates being shut for the whole run.
  TEST_ASSERT_TRUE_MESSAGE(nodes[1].polls > 0,
                           "degraded node was starved to zero");
  TEST_ASSERT_TRUE_MESSAGE(
      host.statistics().degradedClampBypasses > 0,
      "the clamp never fired, so something else admitted the node");

  // Bounded from the other side too: the clamp is a floor, not a licence.
  // Roughly one grant per clamp interval over the window.
  TEST_ASSERT_TRUE_MESSAGE(nodes[1].polls < 40u,
                           "the clamp admitted far more than a floor");

  // The healthy node kept working throughout.
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, host.node(30)->state());
}

// The gates protect healthy nodes, so with no healthy node contending
// they must not engage at all. Otherwise a lone silent node jumps from
// the 250 ms backoff ladder straight to the ceiling clamp on its first
// miss, and interop 2.3.10's re-init ladder slips from ~16 s to minutes.
// VALIDATION: Design v1.5 D17: the gates bound degraded impact on
// healthy nodes.
static void test_gates_do_not_engage_without_healthy_contention(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(31, 4, 0));
  host.begin();

  FaultyNode nodes[1];
  nodes[0] = FaultyNode(31, FaultyNode::kWrongLength, 3);
  pumpFaultyBus(host, transport, nodes, 1, 0, 5000);

  // Polled freely, and the allocator was never consulted.
  TEST_ASSERT_TRUE_MESSAGE(nodes[0].polls > 10u,
                           "a lone degraded node was throttled by the gates");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, host.statistics().degradedGrants,
                                   "the gates engaged with nothing to protect");
  TEST_ASSERT_EQUAL_UINT32(0, host.statistics().degradedSlotDenials);
}

// Done-when 4, and the D16 cross-dependency this whole ticket turns on.
//
// A node that conformed and then went nonconforming while STILL
// ANSWERING is the organic-rot case: cards rearranged, sketch
// recompiled, node alive on the bus. Its miss run is zeroed by every
// mismatch (the reply does prove presence), so interop 2.3.10's
// silence-armed ladder can never fire and nothing else clears freshness.
// Without D17's bounded corrective re-init it parks at STALE forever
// with a growing age, reporting "your data is old" for what is really
// "your geometry is wrong".
// VALIDATION: Design v1.5 D16: the previously-conformed, still-answering
// path reaches MISCONFIGURED only through D17's corrective re-init.
static void test_answering_nonconforming_node_reaches_misconfigured(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.conformanceReinitThreshold = 2;  // keep the window short
  CMRIHost host(transport, config);
  ListenerLog log;
  host.onEvent(recordEvent, &log);

  RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = 4;
  nodeConfig.stalenessMs = 100;
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.addRemoteNode(31, nodeConfig));
  host.begin();

  // First it works.
  FaultyNode good[1];
  good[0] = FaultyNode(31, FaultyNode::kConforming, 4);
  pumpFaultyBus(host, transport, good, 1, 0, 700);
  RemoteNodeHandle* node = host.node(31);
  TEST_ASSERT_NOT_NULL(node);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming, node->conformance());

  // Then an IO card is pulled. It keeps answering, with the wrong shape.
  FaultyNode rotted[1];
  rotted[0] = FaultyNode(31, FaultyNode::kWrongLength, 3);
  pumpFaultyBus(host, transport, rotted, 1, 701, 720);
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kNonconforming,
                    node->conformance());
  // Still answering: this is emphatically not a liveness fault.
  TEST_ASSERT_TRUE(node->liveness() != RemoteNodeLiveness::kSilent);
  TEST_ASSERT_EQUAL_UINT32(0, node->consecutiveMisses());

  // The corrective re-init fires and invalidates, which is the only
  // thing on this path that can clear freshness.
  pumpFaultyBus(host, transport, rotted, 1, 721, 4000);
  TEST_ASSERT_TRUE_MESSAGE(log.reinitScheduled > 0,
                           "the corrective re-init never ran");
  TEST_ASSERT_EQUAL_MESSAGE(RemoteNodeImageState::kNone, node->imageState(),
                            "freshness was never cleared");
  TEST_ASSERT_EQUAL_MESSAGE(
      RemoteNodeState::kMisconfigured, node->state(),
      "the node stranded instead of naming the real problem");
  TEST_ASSERT_EQUAL_UINT32(Age::kNeverMarked, node->inputAgeMs(4000));

  // The miss run stayed at zero throughout, which is the proof that the
  // 2.3.10 ladder could not have been what invalidated the image.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, node->statistics().noReplies,
      "the node fell silent, so this exercised the wrong path");
}

// The invariant, enforced rather than documented: a configured zero
// attempts must still run one. Trimming the re-init as an optimisation
// would otherwise strand every previously-healthy misconfigured node at
// STALE, silently.
// VALIDATION: Design v1.5 D17: the bounded corrective re-init must run
// before the breaker trips.
static void test_zero_configured_reinit_attempts_still_invalidates(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.conformanceReinitThreshold = 2;
  config.conformanceReinitAttempts = 0;  // the trimmed configuration
  CMRIHost host(transport, config);

  RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = 4;
  nodeConfig.stalenessMs = 100;
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.addRemoteNode(31, nodeConfig));
  host.begin();

  FaultyNode good[1];
  good[0] = FaultyNode(31, FaultyNode::kConforming, 4);
  pumpFaultyBus(host, transport, good, 1, 0, 700);
  RemoteNodeHandle* node = host.node(31);
  TEST_ASSERT_NOT_NULL(node);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());

  FaultyNode rotted[1];
  rotted[0] = FaultyNode(31, FaultyNode::kWrongLength, 3);
  pumpFaultyBus(host, transport, rotted, 1, 701, 4000);

  TEST_ASSERT_EQUAL_MESSAGE(
      RemoteNodeState::kMisconfigured, node->state(),
      "zero attempts stranded the node at STALE");
}

// Done-when 2: recovery after a simulated reflash, with no Host restart.
//
// The breaker must never be a hard stop. A node reflashed with correct
// firmware has to come back on its own, because zero traffic means no
// evidence of recovery can ever arrive.
// VALIDATION: Design v1.5 D17: the breaker trips after bounded
// corrective re-inits, probes with a bare P, and re-closes on a
// conforming reply.
static void test_breaker_trips_then_recovers_after_reflash(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.conformanceReinitThreshold = 2;
  config.conformanceReinitAttempts = 2;
  config.breakerProbeIntervalMs = 1000;  // keep the test window sane
  CMRIHost host(transport, config);
  ListenerLog log;
  host.onEvent(recordEvent, &log);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(31, 4, 0));
  host.begin();
  RemoteNodeHandle* node = host.node(31);
  TEST_ASSERT_NOT_NULL(node);

  // A miscompiled sketch: answers every poll with the wrong shape.
  FaultyNode rotted[1];
  rotted[0] = FaultyNode(31, FaultyNode::kWrongLength, 3);
  pumpFaultyBus(host, transport, rotted, 1, 0, 8000);

  TEST_ASSERT_TRUE_MESSAGE(node->conformanceBreakerOpen(),
                           "the breaker never tripped");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, log.breakerTripped,
                                "expected exactly one trip");
  TEST_ASSERT_EQUAL(RemoteNodeState::kMisconfigured, node->state());
  TEST_ASSERT_FALSE(node->isHealthy());
  // The breaker is the axis doing the work here: every other axis on a
  // tripped-but-answering node still reads clean of silence.
  TEST_ASSERT_TRUE(node->liveness() != RemoteNodeLiveness::kSilent);

  // Tripped means throttled, not stopped. Count the probes over a window
  // and check they are paced, not free-running.
  const uint32_t polledAtTrip = rotted[0].polls;
  pumpFaultyBus(host, transport, rotted, 1, 8001, 12000);
  const uint32_t probes = rotted[0].polls - polledAtTrip;
  TEST_ASSERT_TRUE_MESSAGE(probes > 0, "a tripped breaker went silent");
  TEST_ASSERT_TRUE_MESSAGE(probes < 10u, "probes were not rate limited");

  // A tripped breaker probes with a bare P, never a re-init: the post-I
  // settle would stall the round-robin.
  CMRIPacket sent;
  while (transport.takeSent(sent)) {
    TEST_ASSERT_NOT_EQUAL_MESSAGE('I', sent.mt,
                                  "a tripped breaker sent a re-init");
  }

  // Now the node is reflashed with matching firmware. No Host restart:
  // begin() is never called again.
  FaultyNode reflashed[1];
  reflashed[0] = FaultyNode(31, FaultyNode::kConforming, 4);
  pumpFaultyBus(host, transport, reflashed, 1, 12001, 20000);

  TEST_ASSERT_FALSE_MESSAGE(node->conformanceBreakerOpen(),
                            "the breaker never re-closed");
  TEST_ASSERT_TRUE_MESSAGE(log.breakerClosed > 0, "no close was reported");
  TEST_ASSERT_EQUAL(RemoteNodeConformance::kConforming, node->conformance());
  TEST_ASSERT_EQUAL_MESSAGE(RemoteNodeState::kOnline, node->state(),
                            "the node did not return to service");
  TEST_ASSERT_TRUE(node->isHealthy());
  TEST_ASSERT_TRUE_MESSAGE(node->statistics().exchanges > 0,
                           "the recovered node never committed an image");
}

// A runtime geometry correction is the other way back: the operator
// fixes the Host's declaration rather than the Node's firmware.
// VALIDATION: Design v1.5 D17: the breaker re-closes on a runtime change
// to the declared geometry.
static void test_geometry_correction_closes_the_breaker(void) {
  MockCMRITransport transport;
  CMRIHostConfig config;
  config.conformanceReinitThreshold = 2;
  config.conformanceReinitAttempts = 2;
  CMRIHost host(transport, config);

  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(31, 4, 0));
  host.begin();
  RemoteNodeHandle* node = host.node(31);
  TEST_ASSERT_NOT_NULL(node);

  FaultyNode rotted[1];
  rotted[0] = FaultyNode(31, FaultyNode::kWrongLength, 3);
  pumpFaultyBus(host, transport, rotted, 1, 0, 8000);
  TEST_ASSERT_TRUE_MESSAGE(node->conformanceBreakerOpen(),
                           "the breaker never tripped");

  // The operator reads observedIn=3 off the handle and corrects the
  // declaration to match. That is a Host-side fix, so the node's own
  // behaviour never changes.
  TEST_ASSERT_EQUAL_UINT16(3, node->observedInputBytes());
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk,
                    host.setRemoteNodeGeometry(31, 3, 0));
  TEST_ASSERT_FALSE_MESSAGE(node->conformanceBreakerOpen(),
                            "the geometry correction left the breaker open");

  FaultyNode corrected[1];
  corrected[0] = FaultyNode(31, FaultyNode::kConforming, 3);
  pumpFaultyBus(host, transport, corrected, 1, 8001, 10000);
  TEST_ASSERT_EQUAL(RemoteNodeState::kOnline, node->state());
  TEST_ASSERT_TRUE(node->isHealthy());
}

// ------------------------------------- illegal wire-UA detection (#96)
//
// An illegal wire-UA byte (outside [65, 192]) is not carrying a UA at
// all. The gate in drainReceive_ catches it before the
// solicited/unsolicited split, bumps the host-scope counter, and fires
// kIllegalWireUA with node = null. No node is charged.

// An illegal UA arriving during an outstanding poll is detected,
// counted at host scope, and attributed to no node. The polled node
// takes no error, no reject event fires, and the illegal-UA event
// carries the raw byte.
static void test_illegal_wire_ua_during_poll_is_counted_not_attributed(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);

  const CMRIHostStatistics before = rig.host.statistics();
  // Inject a reply with an illegal wire-UA byte (10 < 65).
  rig.transport.injectPacketAt(
      makePacketWithWireUA(10, 'R', kInputsA5, sizeof(kInputsA5)), base);
  runUntil(rig.host, base, base + 2);

  const CMRIHostStatistics after = rig.host.statistics();
  TEST_ASSERT_EQUAL_UINT32(before.illegalWireUAFaults + 1,
                           after.illegalWireUAFaults);
  // No node counter moved.
  TEST_ASSERT_EQUAL_UINT32(before.repliesRejected, after.repliesRejected);
  TEST_ASSERT_EQUAL_UINT32(before.unsolicitedPackets, after.unsolicitedPackets);
  TEST_ASSERT_EQUAL_UINT32(0, rig.node->statistics().errors);
  // The illegal-UA event fired, not a reject.
  TEST_ASSERT_EQUAL_INT(1, log.illegalWireUA);
  TEST_ASSERT_EQUAL_INT(0, log.rejected);
  TEST_ASSERT_EQUAL_HEX8(10, log.lastIllegalWireUA);
  // No node was named.
  TEST_ASSERT_NULL(log.lastNode);
  // The event carries the classified fault.
  TEST_ASSERT_EQUAL(ConformanceFault::kPacketIllegalWireUA, log.lastFault);
}

// An illegal UA arriving while no poll is outstanding (unsolicited) is
// still counted at host scope, not lumped into unsolicitedPackets.
static void test_illegal_wire_ua_while_unsolicited_is_counted(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  rig.host.begin();
  runUntil(rig.host, 0, 507);  // I + T preamble, no poll outstanding
  CMRIPacket scratch;
  while (rig.transport.takeSent(scratch)) { }

  const CMRIHostStatistics before = rig.host.statistics();
  // Inject during the post-T gap (idle, no poll outstanding).
  rig.transport.injectPacketAt(makePacketWithWireUA(200, 'R'), 508);
  runUntil(rig.host, 508, 510);

  const CMRIHostStatistics after = rig.host.statistics();
  TEST_ASSERT_EQUAL_UINT32(before.illegalWireUAFaults + 1,
                           after.illegalWireUAFaults);
  // unsolicitedPackets did not move — the gate caught it first.
  TEST_ASSERT_EQUAL_UINT32(before.unsolicitedPackets, after.unsolicitedPackets);
  TEST_ASSERT_EQUAL_INT(1, log.illegalWireUA);
}

// A legal UA during a poll passes through the gate transparently — no
// illegal-UA counter increment, normal reply processing.
static void test_legal_wire_ua_passes_through_gate(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);

  const CMRIHostStatistics before = rig.host.statistics();
  rig.transport.onSendReplyPacket(
      5 + kWireUAOffset, 'P', makePacket(5, 'R', kInputsA5, sizeof(kInputsA5)));
  runUntil(rig.host, base, base + 2);

  const CMRIHostStatistics after = rig.host.statistics();
  TEST_ASSERT_EQUAL_UINT32(before.illegalWireUAFaults,
                           after.illegalWireUAFaults);
  TEST_ASSERT_EQUAL_UINT32(before.repliesAccepted + 1, after.repliesAccepted);
  TEST_ASSERT_EQUAL_INT(0, log.illegalWireUA);
  TEST_ASSERT_EQUAL_INT(1, log.accepted);
}

// The 4b miss behavior: an illegal UA during the reply gate does not
// satisfy the poll. The gate stays armed, times out, and the polled
// node takes the miss. The illegal-UA counter and the miss ladder
// climb together at the poll rate — the correlation that localizes a
// chronic offset-omission emitter.
static void test_illegal_wire_ua_during_reply_gate_takes_the_miss(void) {
  Rig rig;
  ListenerLog log;
  rig.host.onEvent(recordEvent, &log);
  uint32_t base = primeToPoll(rig);

  const CMRIHostStatistics before = rig.host.statistics();
  const uint32_t missesBefore = rig.node->consecutiveMisses();

  // Inject an illegal-UA reply during the reply gate. The gate stays
  // armed because the illegal packet was discarded, not accepted.
  rig.transport.injectPacketAt(
      makePacketWithWireUA(10, 'R', kInputsA5, sizeof(kInputsA5)), base);
  // Run past the reply timeout (default 250 ms) so the gate expires.
  runUntil(rig.host, base, base + 260);

  // The polled node took the miss.
  TEST_ASSERT_EQUAL_INT(1, log.timeouts);
  TEST_ASSERT_TRUE(rig.node->consecutiveMisses() > missesBefore);
  // And the illegal-UA counter climbed too.
  TEST_ASSERT_EQUAL_UINT32(before.illegalWireUAFaults + 1,
                           rig.host.statistics().illegalWireUAFaults);
  // No reply was accepted or rejected — the illegal packet was
  // neither a valid reply nor a match-layer reject.
  TEST_ASSERT_EQUAL_UINT32(before.repliesAccepted,
                           rig.host.statistics().repliesAccepted);
  TEST_ASSERT_EQUAL_UINT32(before.repliesRejected,
                           rig.host.statistics().repliesRejected);
}

// ----------------------------------------------------------------- runner


void test_smini_init_body_on_wire(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  SminiInit init;  // ns=0
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(5, init));
  host.begin();
  host.tick(0);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('I', sent.mt);
  TEST_ASSERT_EQUAL_UINT8(4, sent.length);
  TEST_ASSERT_EQUAL_HEX8('M', sent.body[0]);
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[1]);
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[2]);
  TEST_ASSERT_EQUAL_HEX8(0, sent.body[3]);
  TEST_ASSERT_EQUAL(NodeType::kSmini, host.node(5)->nodeType());
  TEST_ASSERT_EQUAL_UINT16(3, host.node(5)->inputLength());
  TEST_ASSERT_EQUAL_UINT16(6, host.node(5)->outputLength());
}

void test_cpnode_opts_on_wire(void) {
  MockCMRITransport transport;
  CMRIHost host(transport);
  CpnodeInit init;
  init.inputBytes = 2;
  init.outputBytes = 2;
  init.opts1 = 0x05;  // USECMRIX | USEBCC
  TEST_ASSERT_EQUAL(CMRIHost::ConfigStatus::kOk, host.addRemoteNode(7, init));
  host.begin();
  host.tick(0);
  CMRIPacket sent;
  TEST_ASSERT_TRUE(transport.takeSent(sent));
  TEST_ASSERT_EQUAL_HEX8('C', sent.body[0]);
  TEST_ASSERT_EQUAL_HEX8(0x05, sent.body[3]);
  TEST_ASSERT_EQUAL_HEX8(2, sent.body[5]);
  TEST_ASSERT_EQUAL_HEX8(2, sent.body[6]);
}

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
  RUN_TEST(test_poll_r_accepted_during_await_send_complete);
  RUN_TEST(test_wrong_ua_reply_is_rejected);
  RUN_TEST(test_wrong_mt_reply_is_rejected);
  RUN_TEST(test_wrong_length_reply_counts_error_without_commit);
  RUN_TEST(test_declared_geometry_disagreement_is_observed_and_reported);
  RUN_TEST(test_conformance_decays_to_unknown_when_contact_is_lost);
  RUN_TEST(test_degraded_when_a_conforming_node_starts_faulting);
  RUN_TEST(test_nonconforming_node_reaches_stale_before_misconfigured);
  RUN_TEST(test_wrong_geometry_after_invalidation_reports_misconfigured);
  RUN_TEST(test_self_echoed_poll_does_not_make_the_node_nonconforming);
  RUN_TEST(test_axes_and_predicates_diverge_at_missing_with_fresh_image);
  RUN_TEST(test_health_implies_usability_but_not_the_reverse);
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
  RUN_TEST(test_listener_registration_is_legal_at_runtime);
  RUN_TEST(test_null_listeners_are_harmless);
  RUN_TEST(test_add_remote_node_validation);
  RUN_TEST(test_rejected_add_does_not_poison_later_adds);
  RUN_TEST(test_add_remote_node_ua_round_trip);
  RUN_TEST(test_node_table_capacity_is_enforced);
  RUN_TEST(test_add_after_begin_bootstraps_the_new_node);
  RUN_TEST(test_delete_removes_the_node_from_the_rotation);
  RUN_TEST(test_delete_while_in_flight_attributes_nothing);
  RUN_TEST(test_delete_frees_a_slot_in_a_full_table);
  RUN_TEST(test_reused_slot_is_a_new_subject);
  RUN_TEST(test_geometry_change_invalidates_image_and_forces_reinit);
  RUN_TEST(test_full_mutation_lifecycle);
  RUN_TEST(test_delete_event_names_the_departing_node);
  RUN_TEST(test_add_event_fires_for_runtime_add);
  RUN_TEST(test_geometry_event_carries_old_and_new_geometry);
  RUN_TEST(test_rejected_mutation_fires_no_event);
  RUN_TEST(test_mutation_before_first_tick_stamps_zero_clock);
  RUN_TEST(test_dirty_output_cannot_starve_poll_forever);
  RUN_TEST(test_anti_starvation_does_not_starve_transmit);
  RUN_TEST(test_poll_backoff_doubles_and_clears_on_reply);
  RUN_TEST(test_degraded_participation_is_bounded);
  RUN_TEST(test_gates_alone_bound_degraded_share);
  RUN_TEST(test_degraded_class_is_never_starved_to_zero);
  RUN_TEST(test_gates_do_not_engage_without_healthy_contention);
  RUN_TEST(test_answering_nonconforming_node_reaches_misconfigured);
  RUN_TEST(test_zero_configured_reinit_attempts_still_invalidates);
  RUN_TEST(test_breaker_trips_then_recovers_after_reflash);
  RUN_TEST(test_geometry_correction_closes_the_breaker);
  // Illegal wire-UA detection (#96)
  RUN_TEST(test_illegal_wire_ua_during_poll_is_counted_not_attributed);
  RUN_TEST(test_illegal_wire_ua_while_unsolicited_is_counted);
  RUN_TEST(test_legal_wire_ua_passes_through_gate);
  RUN_TEST(test_illegal_wire_ua_during_reply_gate_takes_the_miss);
  RUN_TEST(test_smini_init_body_on_wire);
  RUN_TEST(test_cpnode_opts_on_wire);
  return UNITY_END();
}
