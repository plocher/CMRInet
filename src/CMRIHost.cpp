// CMRIHost.cpp — the CMRInet polled-strategy Host engine, P/R slice.

#include "CMRIHost.h"

#include <string.h>

namespace CMRInet {

CMRIHost::CMRIHost(CMRITransport& transport, const CMRIHostConfig& config)
    : transport_(transport), config_(config) {}

RemoteNodeHandle* CMRIHost::addRemoteNode(uint8_t address,
                                          const RemoteNodeConfig& config,
                                          const RemoteNodePolicy& policy) {
  // Validation at intake: reject, never remap.
  // VALIDATION: Interop v1.0 E9: Nodes must reject, not remap,
  // out-of-range addresses. The same rule applies to the Host's own
  // configuration.
  if (began_) {
    return nullptr;
  }
  if (nodeCount_ >= kMaxNodes) {
    return nullptr;
  }
  if (address > 127u) {
    return nullptr;
  }
  if (config.inputBytes > RemoteNodeHandle::kMaxInputBytes) {
    return nullptr;
  }
  for (size_t i = 0; i < nodeCount_; ++i) {
    if (nodes_[i].address_ == address) {
      return nullptr;
    }
  }

  RemoteNodeHandle& node = nodes_[nodeCount_];
  node.address_ = address;
  node.ua_ = static_cast<uint8_t>(address + kUaOffset);
  node.config_ = config;
  policies_[nodeCount_] = policy;
  ++nodeCount_;
  return &node;
}

RemoteNodeHandle* CMRIHost::addRemoteNode(uint8_t address,
                                          const RemoteNodeConfig& config) {
  return addRemoteNode(address, config, RemoteNodePolicy());
}

void CMRIHost::begin() {
  if (began_) {
    return;
  }
  transport_.begin();
  began_ = true;
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
/// While a poll is outstanding, the first packet whose UA matches the
/// poll and whose MT is 'R' completes the exchange. Every other packet
/// is counted and discarded.
// VALIDATION: Interop v1.0 2.3.5: verify that a reply's UA matches
// the outstanding poll and that its MT is 'R'. Count and discard
// everything else.
void CMRIHost::drainReceive_(uint32_t nowMs) {
  CMRIPacket rx;
  while (transport_.receivePacket(rx)) {
    emitTrace_(/*transmit=*/false, rx);
    if (phase_ != Phase::kAwaitReply) {
      ++statistics_.unsolicitedPackets;
      continue;
    }
    RemoteNodeHandle& node = nodes_[polledIndex_];
    if (rx.ua != node.ua_ || rx.mt != MessageType::kReceiveData) {
      ++statistics_.repliesRejected;
      emitEvent_(CMRIHostEventType::kReplyRejected, node, nowMs);
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
    // VALIDATION: Interop v1.0 2.2.8: commit to the application only
    // on a valid frame. A geometry mismatch is not valid data.
    ++node.statistics_.errors;
    ++statistics_.repliesRejected;
    node.statistics_.consecutiveMisses = 0;
    emitEvent_(CMRIHostEventType::kReplyRejected, node, nowMs);
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
  }
  node.statistics_.lastTurnaroundMs = nowMs - gateArmedMs_;
  ++statistics_.repliesAccepted;
  emitEvent_(CMRIHostEventType::kReplyAccepted, node, nowMs);
  finishExchange_(nowMs);
}

/// Close the outstanding exchange and arm the pacing gate.
void CMRIHost::finishExchange_(uint32_t nowMs) {
  replyGate_.disarm();
  paceGate_.armIn(nowMs, config_.pollPacingMs);
  phase_ = Phase::kIdle;
}

/// Advance the poll schedule. Later steps run in the same tick when an
/// earlier step completes at once, so a zero-latency transport reaches
/// kAwaitReply in one call.
void CMRIHost::runSchedule_(uint32_t nowMs) {
  if (phase_ == Phase::kIdle) {
    if (paceGate_.armed() && !paceGate_.due(nowMs)) {
      return;
    }
    paceGate_.disarm();
    if (!selectNextNode_()) {
      return;
    }
    poll_.clear();
    poll_.ua = nodes_[polledIndex_].ua_;
    poll_.mt = MessageType::kPoll;
    phase_ = Phase::kSendPoll;
  }

  if (phase_ == Phase::kSendPoll) {
    if (!transport_.sendPacket(poll_)) {
      // Backpressure or link-down. Retry on a later tick. The engine
      // never blocks.
      ++statistics_.pollSendRetries;
      return;
    }
    ++statistics_.pollsSent;
    emitTrace_(/*transmit=*/true, poll_);
    phase_ = Phase::kAwaitSendComplete;
  }

  if (phase_ == Phase::kAwaitSendComplete) {
    if (!transport_.sendComplete()) {
      return;
    }
    // The reply gate opens when the poll is fully delivered, not when
    // it is accepted.
    // VALIDATION: Design v1.0 "Transport contract (packet seam)":
    // sendComplete() gates the strategy's reply timer.
    gateArmedMs_ = nowMs;
    replyGate_.armIn(nowMs, replyTimeoutFor_(polledIndex_));
    phase_ = Phase::kAwaitReply;
    return;
  }

  if (phase_ == Phase::kAwaitReply) {
    if (!replyGate_.due(nowMs)) {
      return;
    }
    // No reply inside the gate. Count the miss and move to the next
    // node. The timed-out poll is never retransmitted.
    // VALIDATION: Interop v1.0 2.3.9: do not retransmit a timed-out
    // message. Count the miss and poll the next Node.
    RemoteNodeHandle& node = nodes_[polledIndex_];
    ++node.statistics_.noReplies;
    ++node.statistics_.consecutiveMisses;
    emitEvent_(CMRIHostEventType::kReplyTimeout, node, nowMs);
    finishExchange_(nowMs);
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
// VALIDATION: Design v1.0 D7: observability is optional listener
// registration; a null listener costs one branch.
void CMRIHost::emitEvent_(CMRIHostEventType type, const RemoteNodeHandle& node,
                          uint32_t nowMs, RemoteNodeState previousState,
                          RemoteNodeState newState) {
  if (eventListener_ == nullptr) {
    return;
  }
  CMRIHostEvent event;
  event.type = type;
  event.node = &node;
  event.nowMs = nowMs;
  event.previousState = previousState;
  event.newState = newState;
  eventListener_(eventContext_, event);
}

/// Fire the trace listener, if one is registered.
void CMRIHost::emitTrace_(bool transmit, const CMRIPacket& packet) {
  if (traceListener_ == nullptr) {
    return;
  }
  traceListener_(traceContext_, transmit, packet);
}

}  // namespace CMRInet
