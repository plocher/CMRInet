// CMRIHost.cpp — the CMRInet polled-strategy Host engine.
//
// Speaks I (session setup), P (poll), and T (full output image); verifies R.
// Per-node on-the-wire order is I → T → P (interop 2.3.1). I and T expect no
// reply (interop E8); the re-init ladder re-sends I + full T after more than
// missThreshold consecutive P misses and invalidates cached inputs (2.3.10).

#include "CMRIHost.h"

#include <string.h>

namespace CMRInet {

CMRIHost::CMRIHost(CMRITransport& transport, const CMRIHostConfig& config)
    : transport_(transport), config_(config) {}

CMRIHost& CMRIHost::addRemoteNode(uint8_t address,
                                  const RemoteNodeConfig& config,
                                  const RemoteNodePolicy& policy) {

  // Configuration is locked after begin() (Design D5). Silently reject;
  // the config phase is over, so the status stays as it was.
  if (began_) {
    configStatus_ = ConfigStatus::kAlreadyBegun;
    return *this;
  }
          // Short-circuit: a prior config-phase rejection poisoned the chain.
  if (configStatus_ != ConfigStatus::kOk) {
    return *this;
  }
  // Validation at intake: reject, never remap. Each check records its
  // reason so begin() can report why the configuration failed.
  // VALIDATION: Interop v1.1 E9: Nodes must reject, not remap,
  // out-of-range addresses. The same rule applies to the Host's own
  // configuration.
  if (nodeCount_ >= kMaxNodes) {
    configStatus_ = ConfigStatus::kTooManyNodes;
    return *this;
  }
  if (address > 127u) {
    configStatus_ = ConfigStatus::kAddressOutOfRange;
    return *this;
  }
  if (config.inputBytes > RemoteNodeHandle::kMaxInputBytes) {
    configStatus_ = ConfigStatus::kInputBytesTooLarge;
    return *this;
  }
  if (config.outputBytes > RemoteNodeHandle::kMaxOutputBytes) {
    configStatus_ = ConfigStatus::kOutputBytesTooLarge;
    return *this;
  }
  for (size_t i = 0; i < nodeCount_; ++i) {
    if (nodes_[i].address_ == address) {
      configStatus_ = ConfigStatus::kAddressInUse;
      return *this;
    }
  }

  RemoteNodeHandle& node = nodes_[nodeCount_];
  node.address_ = address;
  node.ua_ = static_cast<uint8_t>(address + kUaOffset);
  node.config_ = config;
  policies_[nodeCount_] = policy;
  ++nodeCount_;
  return *this;
}

CMRIHost& CMRIHost::addRemoteNode(uint8_t address,
                                  const RemoteNodeConfig& config) {
  return addRemoteNode(address, config, RemoteNodePolicy());
}

CMRIHost& CMRIHost::addRemoteNode(uint8_t address,
                                  uint16_t inputBytes,
                                  uint16_t outputBytes) {
  RemoteNodeConfig config;
  config.inputBytes = inputBytes;
  config.outputBytes = outputBytes;
  return addRemoteNode(address, config);
}

RemoteNodeHandle* CMRIHost::node(uint8_t address) {
  for (size_t i = 0; i < nodeCount_; ++i) {
    if (nodes_[i].address_ == address) {
      return &nodes_[i];
    }
  }
  return nullptr;
}

CMRIHost::ConfigStatus CMRIHost::begin() {
  if (!began_) {
    transport_.begin();
    began_ = true;
  }
  return configStatus_;
}

void CMRIHost::tick(uint32_t nowMs) {
  if (!began_) {
    return;
  }
  transport_.tick(nowMs);
  for (size_t i = 0; i < nodeCount_; ++i) {
    nodes_[i].freshness_.poll(nowMs);
  }
  drainReceive_(nowMs);
  runSchedule_(nowMs);
  updateNodeStates_(nowMs);
}

/// Empty the transport's receive queue.
///
/// Only an R reply to an outstanding P completes an exchange. A packet that
/// arrives at any other time — during an I settle, a T gap, or idle — is
/// unsolicited: I and T expect no reply (interop E8), and fielded nodes MAY
/// emit an end-of-transmission marker after T, so these are counted and
/// discarded, not treated as errors. On 2-wire media the Host also sees its
/// own frames and other Nodes' replies; the UA/MT check below keeps those
/// from mis-committing (interop 2.3.5).
// VALIDATION: Interop v1.1 2.3.5: verify that a reply's UA matches
// the outstanding poll and that its MT is 'R'. Count and discard
// everything else.
void CMRIHost::drainReceive_(uint32_t nowMs) {
  CMRIPacket rx;
  while (transport_.receivePacket(rx)) {
    emitTrace_(/*transmit=*/false, rx);
    if (phase_ != Phase::kAwaitWait ||
        outboundKind_ != OutboundKind::kPoll) {
      ++statistics_.unsolicitedPackets;
      continue;
    }
    RemoteNodeHandle& node = nodes_[polledIndex_];
    // Verify the reply is from the node we polled and that it is an R.
    // Each failure records its reason and the reply's actual bytes so a
    // conformance-checking listener can report what the remote node sent.
    if (rx.ua != node.ua_) {
      ++statistics_.repliesRejected;
      emitEvent_(CMRIHostEventType::kReplyRejected, node, nowMs,
                 RemoteNodeState::kUninitialized,
                 RemoteNodeState::kUninitialized,
                 ReplyRejectReason::kUaMismatch, rx.length, rx.ua, rx.mt);
      continue;
    }
    if (rx.mt != MessageType::kReceiveData) {
      ++statistics_.repliesRejected;
      emitEvent_(CMRIHostEventType::kReplyRejected, node, nowMs,
                 RemoteNodeState::kUninitialized,
                 RemoteNodeState::kUninitialized,
                 ReplyRejectReason::kMtMismatch, rx.length, rx.ua, rx.mt);
      continue;
    }
    acceptReply_(rx, nowMs);
  }
}

/// Handle a UA- and MT-verified reply to the outstanding poll.
void CMRIHost::acceptReply_(const CMRIPacket& reply, uint32_t nowMs) {
  RemoteNodeHandle& node = nodes_[polledIndex_];

  if (reply.length != node.config_.inputBytes) {
    // The node answered with the wrong geometry. The reply proves the
    // node is present, so the miss run ends, but the body is never
    // committed.
    // VALIDATION: Interop v1.1 2.2.8: commit to the application only
    // on a valid frame. A geometry mismatch is not valid data.
    ++node.statistics_.errors;
    ++statistics_.repliesRejected;
    node.statistics_.consecutiveMisses = 0;
    emitEvent_(CMRIHostEventType::kReplyRejected, node, nowMs,
               RemoteNodeState::kUninitialized,
               RemoteNodeState::kUninitialized,
               ReplyRejectReason::kGeometryMismatch, reply.length, reply.ua,
               reply.mt);
    finishExchange_(nowMs);
    return;
  }

  if (node.config_.inputBytes != 0) {
    memcpy(node.inputs_, reply.body, node.config_.inputBytes);
  }
  node.freshness_.mark(nowMs);
  ++node.statistics_.exchanges;
  if (node.statistics_.consecutiveMisses != 0) {
    ++node.statistics_.recoveries;
    node.statistics_.consecutiveMisses = 0;
    node.reinitArmed_ = false;  // a reply ends the miss-run, disarming the ladder
  }
  node.statistics_.lastTurnaroundMs = nowMs - gateArmedMs_;
  ++statistics_.repliesAccepted;
  emitEvent_(CMRIHostEventType::kReplyAccepted, node, nowMs);
  finishExchange_(nowMs);
}

/// Close the outstanding exchange and arm the pacing gate.
void CMRIHost::finishExchange_(uint32_t nowMs) {
  waitGate_.disarm();
  paceGate_.armIn(nowMs, config_.pollPacingMs);
  phase_ = Phase::kIdle;
}

/// Advance the exchange schedule. Later steps run in the same tick when an
/// earlier step completes at once, so a zero-latency transport reaches
/// kAwaitWait in one call.
void CMRIHost::runSchedule_(uint32_t nowMs) {
  if (phase_ == Phase::kIdle) {
    if (paceGate_.armed() && !paceGate_.due(nowMs)) {
      return;
    }
    paceGate_.disarm();
    if (!selectNextNode_()) {
      return;
    }
    // Choose this node's outbound: I before the first P (and after the
    // re-init ladder arms), a full T when outputs are dirty, else P.
    // VALIDATION: Interop v1.1 2.3.1: send I before the first P, and a
    // full T immediately after every I.
    RemoteNodeHandle& node = nodes_[polledIndex_];
    // Optional periodic full-T refresh (Design v1.1 D9; off by default).
    if (config_.transmitRefreshMs != 0 && node.config_.outputBytes != 0 &&
        !node.needsInit_ && node.lastTxMs_ != 0 &&
        (nowMs - node.lastTxMs_) >= config_.transmitRefreshMs) {
      node.outputsDirty_ = true;
    }
    if (node.needsInit_) {
      buildInitPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kInit;
    } else if (node.outputsDirty_) {
      buildTransmitPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kTransmit;
    } else {
      buildPollPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kPoll;
    }
    phase_ = Phase::kSendOutbound;
  }

  if (phase_ == Phase::kSendOutbound) {
    if (!transport_.sendPacket(outbound_)) {
      // Backpressure or link-down. Retry on a later tick. The engine
      // never blocks.
      ++statistics_.pollSendRetries;
      return;
    }
    if (outboundKind_ == OutboundKind::kPoll) {
      ++statistics_.pollsSent;
    }
    emitTrace_(/*transmit=*/true, outbound_);
    phase_ = Phase::kAwaitSendComplete;
  }

  if (phase_ == Phase::kAwaitSendComplete) {
    if (!transport_.sendComplete()) {
      return;
    }
    // Arm the post-send wait by kind. The wait opens when the packet is
    // fully delivered, not when it is accepted.
    // VALIDATION: Design v1.1 "Transport contract (packet seam)":
    // sendComplete() gates the strategy's reply timer.
    gateArmedMs_ = nowMs;
    if (outboundKind_ == OutboundKind::kPoll) {
      waitGate_.armIn(nowMs, replyTimeoutFor_(polledIndex_));
    } else if (outboundKind_ == OutboundKind::kInit) {
      // I receives no reply (E8); the settle paces the immediate full T.
      // VALIDATION: Interop v1.1 2.3.7: ~500 ms settle after I.
      waitGate_.armIn(nowMs, config_.postInitSettleMs);
      nodes_[polledIndex_].needsInit_ = false;
    } else {  // kTransmit
      // T receives no reply (E8); the gap paces the next outbound.
      // VALIDATION: Interop v1.1 2.3.7: ~2 ms after T.
      waitGate_.armIn(nowMs, config_.postTxGapMs);
      nodes_[polledIndex_].lastTxMs_ = nowMs;
    }
    phase_ = Phase::kAwaitWait;
    return;
  }

  if (phase_ == Phase::kAwaitWait) {
    if (!waitGate_.due(nowMs)) {
      return;
    }
    if (outboundKind_ == OutboundKind::kPoll) {
      // No reply inside the gate. Count the miss; the timed-out poll is
      // never retransmitted.
      // VALIDATION: Interop v1.1 2.3.9: do not retransmit a timed-out
      // message. Count the miss and poll the next Node.
      RemoteNodeHandle& node = nodes_[polledIndex_];
      ++node.statistics_.noReplies;
      ++node.statistics_.consecutiveMisses;
      emitEvent_(CMRIHostEventType::kReplyTimeout, node, nowMs);
      // More than missThreshold consecutive misses arms the re-init
      // ladder once per miss-run: the next slot re-sends I + full T and
      // has already invalidated cached inputs.
      // VALIDATION: Interop v1.1 2.3.10: after more than 5 consecutive
      // poll misses, re-send I, then a full T, and invalidate cached
      // input state. Keep polling the silent Node forever.
      if (node.statistics_.consecutiveMisses > config_.missThreshold &&
          !node.reinitArmed_) {
        node.reinitArmed_ = true;
        node.needsInit_ = true;
        node.outputsDirty_ = true;
        invalidateNodeInputs_(node);
        emitEvent_(CMRIHostEventType::kReinitScheduled, node, nowMs);
      }
      finishExchange_(nowMs);
    } else if (outboundKind_ == OutboundKind::kInit) {
      // Settle elapsed: send the immediate full T (interop 2.3.1).
      buildTransmitPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kTransmit;
      phase_ = Phase::kSendOutbound;
    } else {  // kTransmit
      // Post-T gap elapsed: the T is delivered.
      nodes_[polledIndex_].outputsDirty_ = false;
      finishExchange_(nowMs);
    }
  }
}

/// Pick the next enabled node in round-robin order. Returns false when
/// no node is enabled.
bool CMRIHost::selectNextNode_() {
  for (size_t step = 0; step < nodeCount_; ++step) {
    const size_t candidate = (cursor_ + step) % nodeCount_;
    if (nodes_[candidate].config_.enabled) {
      polledIndex_ = candidate;
      cursor_ = (candidate + 1) % nodeCount_;
      return true;
    }
  }
  return false;
}

/// Build the CPNODE 'C' initialization packet for a node.
/// VALIDATION: Interop v1.1 E3: the CPNODE I-body dialect is
/// <'C'> <dH> <dL> <opts1> <opts2> <NI> <NO> <0xFF x6>, a 13-byte body.
/// NI/NO are the wire byte budgets (inputBytes/outputBytes). The six
/// 0xFF pad bytes are raw (the codec never escapes 0xFF, rule 2.1.3);
/// every other body byte equal to 2/3/16 is DLE-escaped by encodeFrame
/// (erratum E1). dH/dL come from the per-node policy (erratum E4).
void CMRIHost::buildInitPacket_(size_t nodeIndex) {
  RemoteNodeHandle& node = nodes_[nodeIndex];
  const RemoteNodePolicy& policy = policies_[nodeIndex];
  outbound_.clear();
  outbound_.ua = node.ua_;
  outbound_.mt = MessageType::kInit;
  uint8_t body[13];
  body[0] = 'C';
  body[1] = policy.transmissionDelayDh;
  body[2] = policy.transmissionDelayDl;
  body[3] = 0;  // opts1: USECMRIX | SENDEOT | USEBCC, all default 0
  body[4] = 0;  // opts2: reserved
  body[5] = static_cast<uint8_t>(node.config_.inputBytes);   // NI
  body[6] = static_cast<uint8_t>(node.config_.outputBytes);  // NO
  body[7] = kSyn;  // six raw 0xFF pad bytes
  body[8] = kSyn;
  body[9] = kSyn;
  body[10] = kSyn;
  body[11] = kSyn;
  body[12] = kSyn;
  outbound_.setBody(body, sizeof(body));
}

/// Build a full-output-image T packet. Never partial: fielded Nodes fill
/// missing bytes from stale memory (interop 2.3.2).
void CMRIHost::buildTransmitPacket_(size_t nodeIndex) {
  RemoteNodeHandle& node = nodes_[nodeIndex];
  outbound_.clear();
  outbound_.ua = node.ua_;
  outbound_.mt = MessageType::kTransmitData;
  outbound_.setBody(node.outputs_, node.config_.outputBytes);
}

/// Build a P (poll) packet: UA + MT, empty body.
void CMRIHost::buildPollPacket_(size_t nodeIndex) {
  outbound_.clear();
  outbound_.ua = nodes_[nodeIndex].ua_;
  outbound_.mt = MessageType::kPoll;
}

/// Invalidate cached input state for a node (re-init ladder). Clears
/// freshness only and keeps the last-good bytes: 0 is a valid consumer
/// value, so zeroing the buffer would assert "all clear" (the QBASIC
/// review's F15 hazard). The next state recomputation reports
/// kUninitialized and inputAgeMs() reports kNeverMarked.
/// VALIDATION: Interop v1.1 2.3.10: invalidate cached input state.
void CMRIHost::invalidateNodeInputs_(RemoteNodeHandle& node) {
  node.freshness_.clear();
}

/// Recompute every node's health from its counters and freshness.
void CMRIHost::updateNodeStates_(uint32_t nowMs) {
  for (size_t i = 0; i < nodeCount_; ++i) {
    RemoteNodeHandle& node = nodes_[i];
    const RemoteNodeState previous = node.state_;
    if (node.statistics_.consecutiveMisses > config_.missThreshold) {
      node.state_ = RemoteNodeState::kOffline;
    } else if (!node.freshness_.marked()) {
      node.state_ = RemoteNodeState::kUninitialized;
    } else if (node.config_.stalenessMs != 0 &&
               node.freshness_.atLeast(nowMs, node.config_.stalenessMs)) {
      node.state_ = RemoteNodeState::kStale;
    } else {
      node.state_ = RemoteNodeState::kOnline;
    }
    if (node.state_ != previous) {
      emitEvent_(CMRIHostEventType::kNodeStateChanged, node, nowMs, previous,
                 node.state_);
    }
  }
}

uint32_t CMRIHost::replyTimeoutFor_(size_t nodeIndex) const {
  const uint32_t perNode = policies_[nodeIndex].replyTimeoutMs;
  return (perNode == RemoteNodePolicy::kInheritHost) ? config_.replyTimeoutMs
                                                     : perNode;
}

/// Fire the event listener, if one is registered.
// VALIDATION: Design v1.1 D7: observability is optional listener
// registration; a null listener costs one branch.
void CMRIHost::emitEvent_(CMRIHostEventType type, const RemoteNodeHandle& node,
                          uint32_t nowMs, RemoteNodeState previousState,
                          RemoteNodeState newState,
                          ReplyRejectReason rejectReason,
                          uint16_t replyLength, uint8_t replyUa,
                          uint8_t replyMt) {
  if (eventListener_ == nullptr) {
    return;
  }
  CMRIHostEvent event;
  event.type = type;
  event.node = &node;
  event.nowMs = nowMs;
  event.previousState = previousState;
  event.newState = newState;
  event.rejectReason = rejectReason;
  event.replyLength = replyLength;
  event.replyUa = replyUa;
  event.replyMt = replyMt;
  eventListener_(eventContext_, event);
}

/// Fire the trace listener, if one is registered.
void CMRIHost::emitTrace_(bool transmit, const CMRIPacket& packet) {
  if (traceListener_ == nullptr) {
    return;
  }
  traceListener_(traceContext_, transmit, packet);
}

const char* configStatusString(CMRIHost::ConfigStatus status) {
  switch (status) {
    case CMRIHost::ConfigStatus::kOk:                 return "ok";
    case CMRIHost::ConfigStatus::kAlreadyBegun:        return "configuration already begun";
    case CMRIHost::ConfigStatus::kTooManyNodes:       return "too many nodes";
    case CMRIHost::ConfigStatus::kAddressOutOfRange:  return "address out of range";
    case CMRIHost::ConfigStatus::kAddressInUse:       return "address in use";
    case CMRIHost::ConfigStatus::kInputBytesTooLarge: return "input bytes too large";
    case CMRIHost::ConfigStatus::kOutputBytesTooLarge:return "output bytes too large";
  }
  return "unknown";
}

const char* replyRejectReasonString(ReplyRejectReason reason) {
  switch (reason) {
    case ReplyRejectReason::kNone:              return "none";
    case ReplyRejectReason::kUaMismatch:        return "ua mismatch";
    case ReplyRejectReason::kMtMismatch:        return "mt mismatch";
    case ReplyRejectReason::kGeometryMismatch:  return "geometry mismatch";
  }
  return "unknown";
}

}  // namespace CMRInet
