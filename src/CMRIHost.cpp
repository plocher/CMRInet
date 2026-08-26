// CMRIHost.cpp — the CMRInet polled-strategy Host engine.
//
// Speaks I (session setup), P (poll), and T (full output image); verifies R.
// Per-node on-the-wire order is I → T → P (interop 2.3.1). I and T expect no
// reply (interop E8); the re-init ladder re-sends I + full T after more than
// missThreshold consecutive P misses and invalidates cached inputs (2.3.10).

#include "CMRIHost.h"

#include <string.h>

namespace CMRInet {

namespace {

/// Percentages come from a sketch-writable config, so clamp rather than
/// trust. A share above 100 would make Gate A's debit negative and turn
/// the credit counter into a source of free slots.
uint32_t clampPercent(uint8_t percent) {
  return percent > 100u ? 100u : static_cast<uint32_t>(percent);
}

/// The longest elapsed span one Gate B refill will credit. Bounds the
/// multiply inside uint32_t and costs nothing in practice: the bucket
/// saturates at its burst ceiling long before this much time passes.
constexpr uint32_t kMaxRefillSpanMs = 60000u;

}  // namespace

CMRIHost::CMRIHost(CMRITransport& transport, const CMRIHostConfig& config)
    : transport_(transport), config_(config) {}

/// Find the slot holding a live node at `address`.
///
/// Occupancy is tested before the address, and that order matters: a
/// cleaned tombstone holds address_ == 0, and 0 is a legal node address.
/// Testing the address first would hand out tombstones to node(0).
bool CMRIHost::findSlot_(uint8_t address, size_t& slot) const {
  for (size_t i = 0; i < kMaxNodes; ++i) {
    if (occupied_[i] && nodes_[i].address_ == address) {
      slot = i;
      return true;
    }
  }
  return false;
}

/// Return a slot to pristine state across all three substrates (D15).
///
/// Whole-object assignment rather than a field-by-field checklist: every
/// member of RemoteNodeHandle carries a default initializer, so this
/// clears control, belief, and observation at once and cannot fall out
/// of date when a substrate gains a field. That matters because #85 and
/// #87 both add substrate state, and a hand-written field list would rot
/// silently at exactly the moment it mattered. The policy is reset too,
/// so a reused slot inherits no per-node override from its predecessor.
///
/// PREMISE: this is exhaustive only while every member of
/// RemoteNodeHandle is a copyable value type. If one ever becomes
/// non-copyable, self-referential, or holds a pointer into its own
/// storage, assignment stops clearing it correctly and stops doing so
/// *quietly* -- no compiler error, just a slot that remembers its
/// predecessor. Anything of that shape needs explicit handling here.
// VALIDATION: Design v1.2 D5: slot reuse resets all three substrates
// (D15). Freshness in particular must be cleared, or a newly added node
// reports data it never sent.
// VALIDATION: Design v1.3 D15: the substrate split gives slot reuse a
// checklist rather than a hunt.
void CMRIHost::resetSlot_(size_t slot) {
  nodes_[slot] = RemoteNodeHandle();
  policies_[slot] = RemoteNodePolicy();
}

/// Detach the outstanding exchange when a mutation invalidates the
/// identity of the slot it names.
///
/// Two cases, and the difference is whether bytes have reached the wire:
/// - kSendOutbound: the packet was built but the transport has not
///   accepted it. Nothing is on the wire, so cancel it outright rather
///   than orphan it -- transmitting an I to a node the operator just
///   deleted would be pointless traffic, and could re-initialize a node
///   that was removed on purpose.
/// - kAwaitSendComplete / kAwaitWait: the transport owns the bytes and
///   TXEN is asserted. Truncating would violate the transmit-drain
///   doctrine, so the frame completes and the exchange is orphaned.
// VALIDATION: Design v1.2 D5: mutating the node of the outstanding
// exchange is legal; the send cannot be aborted, so the exchange is
// orphaned -- the frame completes, any reply is discarded, and nothing
// is attributed to any node.
// VALIDATION: Design v1.1 D13: a send already on the wire drains; the
// engine never truncates it.
void CMRIHost::detachExchangeFrom_(size_t slot) {
  if (phase_ == Phase::kIdle || polledIndex_ != slot ||
      exchangeOrphaned_) {
    return;
  }
  if (phase_ == Phase::kSendOutbound) {
    // Not yet handed to the transport: drop it and let the schedule
    // pick a fresh node next tick.
    waitGate_.disarm();
    phase_ = Phase::kIdle;
    return;
  }
  exchangeOrphaned_ = true;
  ++statistics_.orphanedExchanges;
}

// Each rejection returns its own reason and leaves no residue: the next
// call is judged on its own merits. Callers adding several nodes decide
// how to treat a partial failure.
// VALIDATION: Design v1.2 D5: mutators report their own outcome; no
// sticky, chain-poisoning status.
// VALIDATION: Design v1.2 D5: add is legal before and after begin();
// begin() does not lock the node table.
CMRIHost::ConfigStatus CMRIHost::addRemoteNode(
    uint8_t address, const RemoteNodeConfig& config,
    const RemoteNodePolicy& policy) {
  // Validation at intake: reject, never remap.
  // VALIDATION: Interop v1.1 E9: Nodes must reject, not remap,
  // out-of-range addresses. The same rule applies to the Host's own
  // configuration.
  if (address > 127u) {
    return ConfigStatus::kAddressOutOfRange;
  }
  if (config.inputBytes > RemoteNodeHandle::kMaxInputBytes) {
    return ConfigStatus::kInputBytesTooLarge;
  }
  if (config.outputBytes > RemoteNodeHandle::kMaxOutputBytes) {
    return ConfigStatus::kOutputBytesTooLarge;
  }

  // Capacity is slot availability, not a count comparison. A count
  // that only ever rose would refuse an add while cleaned tombstones
  // sat free -- exactly the case D5 says must succeed.
  // VALIDATION: Design v1.2 D8: the invariant is nodeCount_ <=
  // kMaxNodes, checked per add.
  size_t free = kMaxNodes;
  for (size_t i = 0; i < kMaxNodes; ++i) {
    if (occupied_[i]) {
      if (nodes_[i].address_ == address) {
        return ConfigStatus::kAddressInUse;
      }
    } else if (free == kMaxNodes) {
      free = i;  // first free slot; keep scanning for a duplicate
    }
  }
  if (free == kMaxNodes) {
    return ConfigStatus::kTooManyNodes;
  }

  // A tombstone was cleaned at delete, but reset again rather than
  // trusting that: the invariant this add depends on is "the slot is
  // pristine", and asserting it here is cheaper than reasoning about
  // every path that could have left it otherwise.
  resetSlot_(free);
  RemoteNodeHandle& node = nodes_[free];
  node.address_ = address;
  node.ua_ = static_cast<uint8_t>(address + kUaOffset);
  node.config_ = config;
  policies_[free] = policy;
  occupied_[free] = true;
  ++nodeCount_;
  return ConfigStatus::kOk;
}

CMRIHost::ConfigStatus CMRIHost::deleteRemoteNode(uint8_t address) {
  size_t slot = 0;
  if (!findSlot_(address, slot)) {
    return ConfigStatus::kNoSuchNode;
  }

  // Order matters: detach while the slot still names this node, so the
  // orphan decision is made against the identity being removed.
  detachExchangeFrom_(slot);

  // Tombstone in place. No compaction, so every surviving handle keeps
  // its address. Cleaning now rather than at reuse is what makes a
  // stale handle detectable: address() reads 0 instead of continuing to
  // report the node it used to serve.
  occupied_[slot] = false;
  resetSlot_(slot);
  --nodeCount_;
  return ConfigStatus::kOk;
}

CMRIHost::ConfigStatus CMRIHost::setRemoteNodeGeometry(uint8_t address,
                                                       uint16_t inputBytes,
                                                       uint16_t outputBytes) {
  if (inputBytes > RemoteNodeHandle::kMaxInputBytes) {
    return ConfigStatus::kInputBytesTooLarge;
  }
  if (outputBytes > RemoteNodeHandle::kMaxOutputBytes) {
    return ConfigStatus::kOutputBytesTooLarge;
  }
  size_t slot = 0;
  if (!findSlot_(address, slot)) {
    return ConfigStatus::kNoSuchNode;
  }

  // The in-flight I announced the geometry being replaced, and a reply
  // sized for the old image would be scored against the new one -- a
  // geometry-mismatch error charged to a node that did nothing wrong.
  detachExchangeFrom_(slot);

  RemoteNodeHandle& node = nodes_[slot];
  node.config_.inputBytes = inputBytes;
  node.config_.outputBytes = outputBytes;

  // Belief: the cached image described the old shape, so it is not
  // merely stale, it is meaningless. Reporting kNone is honest;
  // retaining bytes under a new geometry would not be. Clearing
  // freshness is what makes the image invalid -- validity is derived
  // from it (RemoteNodeHandle::hasValidImage_), not separately stored.
  memset(node.inputs_, 0, sizeof(node.inputs_));
  node.freshness_.clear();
  memset(node.outputs_, 0, sizeof(node.outputs_));

  // LOAD-BEARING, and it does not look it. imageState_ is a *stored*
  // axis (D16) that updateNodeStates_ recomputes every tick, so this
  // assignment reads redundant -- but the recomputation has not run
  // yet. Without it the handle contradicts itself between this call
  // returning and the next tick, and reading a handle straight after
  // mutating it is the obvious thing for a sketch to do. Deleting this
  // line reintroduces that window silently.
  node.imageState_ = RemoteNodeImageState::kNone;

  // Control: owe this node a fresh session. I re-announces NI/NO, and
  // the full T that follows carries the cleared output image.
  // VALIDATION: Interop v1.1 2.3.1: send I before the first P, and a
  // full T immediately after every I.
  node.needsInit_ = true;
  node.outputsDirty_ = true;
  node.reinitArmed_ = false;

  // Content evaluation: whatever we believed about this node's
  // conformance was measured against a geometry that no longer applies.
  // VALIDATION: Design v1.3 D16: conformance is current evidence, not
  // latched.
  node.conformance_ = RemoteNodeConformance::kUnknown;

  // A tripped breaker re-closes on a runtime geometry change: the
  // operator just corrected the Host's side of the disagreement, which
  // is one of the two ways the fault can be fixed. The other is a
  // reflash, which the bare-P probe discovers on its own.
  //
  // The re-init this close would arm is already armed above, so only
  // the ladder's bookkeeping needs resetting. resetBreaker_ rather than
  // closeBreaker_ because this is a configuration call with no clock,
  // and an event stamped with a fabricated timestamp is worse than no
  // event: every other listener field here is real.
  // VALIDATION: Design v1.5 D17: the breaker re-closes on either a
  // conforming reply or a runtime change to the declared geometry.
  resetBreaker_(node);

  // Observation is untouched: same address, same logical device, so the
  // counters keep running -- and observedInputBytes_ in particular
  // survives deliberately. What changed is the Host's *claim*, not the
  // Node's physical card complement, so the last demonstrated length is
  // still the best evidence available. Keeping it is what lets an
  // operator correct a declaration and see observed and declared agree
  // (D14: declared is a claim, observed is evidence).
  return ConfigStatus::kOk;
}

CMRIHost::ConfigStatus CMRIHost::addRemoteNode(
    uint8_t address, const RemoteNodeConfig& config) {
  return addRemoteNode(address, config, RemoteNodePolicy());
}

CMRIHost::ConfigStatus CMRIHost::addRemoteNode(uint8_t address,
                                               uint16_t inputBytes,
                                               uint16_t outputBytes) {
  RemoteNodeConfig config;
  config.inputBytes = inputBytes;
  config.outputBytes = outputBytes;
  return addRemoteNode(address, config);
}

RemoteNodeHandle* CMRIHost::node(uint8_t address) {
  size_t slot = 0;
  return findSlot_(address, slot) ? &nodes_[slot] : nullptr;
}

void CMRIHost::begin() {
  if (!began_) {
    transport_.begin();
    began_ = true;
  }
}

void CMRIHost::tick(uint32_t nowMs) {
  if (!began_) {
    return;
  }
  transport_.tick(nowMs);
  for (size_t i = 0; i < kMaxNodes; ++i) {
    if (occupied_[i]) {
      nodes_[i].freshness_.poll(nowMs);
    }
  }
  // Gate B accrues against wall clock, so it refills here rather than
  // in the scheduler: time passes during an exchange too, and a bucket
  // that only refilled between exchanges would under-credit exactly the
  // long silent probes it exists to bound.
  refillDegradedBudget_(nowMs);
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
    if (exchangeOrphaned_) {
      // The poll was solicited, but its node was mutated away while the
      // reply was in flight. Counting it as unsolicited would misreport
      // the bus, and matching it against polledIndex_ would score a
      // tombstone or the slot's new occupant. Discard it: the frame is
      // already visible on the trace, and orphanedExchanges records the
      // loss at host scope where it honestly belongs.
      // VALIDATION: Design v1.2 D5: any reply to an orphaned exchange is
      // discarded and nothing is attributed to any node.
      continue;
    }
    RemoteNodeHandle& node = nodes_[polledIndex_];
    // Verify the reply is from the node we polled and that it is an R.
    // Each failure records its reason and the reply's actual bytes so a
    // conformance-checking listener can report what the remote node sent.
    //
    // Neither failure below moves the node's conformance axis, and that
    // is deliberate. Both are packet-rung observations about traffic on
    // the bus, not image-rung evidence about this node:
    //
    // - A reply carrying somebody else's UA is, definitionally,
    //   somebody else's. Scoring it against the node we happened to be
    //   polling would charge one device for another's behavior.
    // - An MT mismatch looks attributable and is not. On 2-wire media
    //   the Host sees its own frames (see this function's header), so
    //   its own P echoes back with rx.ua == node.ua_ and mt == 'P' and
    //   lands right here. Wiring that to the axis would park every node
    //   on every 2-wire Host in DEGRADED permanently.
    //
    // So both stay at host scope on repliesRejected, matching where the
    // per-node `errors` counter already draws the same line. The event
    // still carries the classified fault, so nothing is hidden -- it is
    // reported without being attributed.
    // VALIDATION: Design v1.4 D14: only image-rung faults are evidence
    // about a Node's conformance; packet-rung observations are named
    // and reported without moving the stored verdict.
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

  // L1 truth acquisition. The body length of a reply that carries this
  // node's UA and an R type is the geometry the node just demonstrated,
  // whatever it is. Recorded before the comparison below, so the
  // evidence survives the path that throws the body away -- which is
  // exactly the path where somebody needs it.
  // VALIDATION: Design v1.3 D14: declared geometry is a claim and reply
  // length is evidence (L1). L1 is the only rung covering a fielded
  // Node that ignores I and so never sends the I-ack L2 would need.
  node.observedInputBytes_ = static_cast<uint16_t>(reply.length);

  if (reply.length != node.config_.inputBytes) {
    // The node answered with the wrong geometry. The reply proves the
    // node is present, so the miss run ends, but the body is never
    // committed.
    // VALIDATION: Interop v1.1 2.2.8: commit to the application only
    // on a valid frame. A geometry mismatch is not valid data.
    ++node.statistics_.errors;
    ++statistics_.repliesRejected;
    node.consecutiveMisses_ = 0;

    // Content evaluation: current, first-hand evidence that the Host's
    // declared geometry and the Node's actual geometry disagree. The
    // Node is doing exactly what it was built to do, so this is a
    // disagreement to be fixed in configuration, not a defect -- see
    // attributionOf(kImageGeometryMismatch).
    // VALIDATION: Design v1.3 D16: the conformance axis is populated
    // from current evidence and is not latched.
    node.conformance_ = RemoteNodeConformance::kNonconforming;
    node.lastFault_.fault = ConformanceFault::kImageGeometryMismatch;
    node.lastFault_.expected = node.config_.inputBytes;
    node.lastFault_.observed = static_cast<uint16_t>(reply.length);
    node.lastFault_.atMs = nowMs;
    // The node answered at all, so it is not chronically offline; clear
    // any *liveness* backoff the same as a clean accept (map issue #41).
    //
    // This clearing is what made the #80 node an attractive nuisance: it
    // left a permanently-broken node immediately eligible on every
    // rotation, 1206 polls to a silent node's 7. It stays, because the
    // reading is correct -- this is not a liveness fault. What changed
    // in D17 is that the node is now in the degraded lane, so the two
    // gates bound it instead of nothing bounding it.
    const uint32_t previousBackoffMs = node.pollBackoffMs_;
    node.pollBackoffMs_ = 0;
    node.pollBackoff_.disarm();

    ++node.consecutiveNonconforming_;
    emitEvent_(CMRIHostEventType::kReplyRejected, node, nowMs,
               RemoteNodeState::kUninitialized,
               RemoteNodeState::kUninitialized,
               ReplyRejectReason::kGeometryMismatch, reply.length, reply.ua,
               reply.mt);
    emitEvent_(CMRIHostEventType::kPollBackoffChanged, node, nowMs,
               RemoteNodeState::kUninitialized,
               RemoteNodeState::kUninitialized, ReplyRejectReason::kNone, 0,
               0, 0, PollBackoffChangeReason::kGeometryMismatch,
               previousBackoffMs, node.pollBackoffMs_);

    // The conformance ladder, mirroring the liveness ladder in
    // runSchedule_ -- including its event order. Both report the
    // exchange's own outcome first and the consequence second, so a
    // listener never sees a ladder arm before the reply that armed it.
    //
    // A run past the threshold arms one bounded corrective re-init;
    // exhausting the attempts trips the breaker. Thresholding on a run
    // rather than a single mismatch is what leaves room for D16's
    // chronology to play out: the image ages to STALE first.
    // VALIDATION: Design v1.5 D17: a run of nonconforming replies arms
    // bounded corrective re-inits, then trips the breaker.
    if (node.breakerState_ != ConformanceBreakerState::kOpen &&
        node.consecutiveNonconforming_ >
            config_.conformanceReinitThreshold) {
      // At least one attempt, always. A configured zero would trip the
      // breaker without ever invalidating, and the node would strand at
      // STALE reporting "your data is old" for what is really "your
      // geometry is wrong" -- the concealment #80 exists to remove. The
      // invariant is enforced here rather than documented and hoped for.
      // VALIDATION: Design v1.5 D17: the bounded corrective re-init must
      // run before the breaker trips; it is the sole writer that
      // completes STALE -> MISCONFIGURED for a node that keeps
      // answering.
      const uint32_t attempts = config_.conformanceReinitAttempts == 0
                                    ? 1u
                                    : config_.conformanceReinitAttempts;
      if (node.breakerReinitAttempts_ < attempts) {
        armCorrectiveReinit_(node, nowMs);
      } else {
        tripBreaker_(node, nowMs);
      }
    }
    finishExchange_(nowMs);
    return;
  }

  if (node.config_.inputBytes != 0) {
    memcpy(node.inputs_, reply.body, node.config_.inputBytes);
  }
  // Marking freshness is what makes the image valid: validity is
  // derived from the mark rather than tracked alongside it, so there is
  // no second flag here to forget.
  node.freshness_.mark(nowMs);

  // Current positive evidence, and no more latched than the negative
  // kind: the next mismatch takes it away again, and losing contact
  // degrades it to kUnknown in updateNodeStates_ below.
  // VALIDATION: Design v1.3 D16: conformance may only be asserted from
  // current evidence.
  node.conformance_ = RemoteNodeConformance::kConforming;
  ++node.statistics_.exchanges;
  if (node.consecutiveMisses_ != 0) {
    ++node.statistics_.recoveries;
    node.consecutiveMisses_ = 0;
    node.reinitArmed_ = false;  // a reply ends the miss-run, disarming the ladder
  }
  // A conforming reply ends the conformance run too, and re-closes the
  // breaker if it was open or mid-ladder. This is the recovery path a
  // reflashed node takes: the bare-P probe gets a correctly-shaped
  // answer, and the Host resumes without being restarted.
  // VALIDATION: Design v1.5 D17: the breaker re-closes on a conforming
  // reply, arming the re-init ladder because a reflashed node has lost
  // its session.
  if (node.breakerState_ != ConformanceBreakerState::kClosed ||
      node.consecutiveNonconforming_ != 0) {
    closeBreaker_(node, nowMs);
  }
  // A reply proves the node is present and responsive: clear any poll
  // backoff immediately rather than ramping it down (map issue #41).
  const uint32_t previousBackoffMs = node.pollBackoffMs_;
  node.pollBackoffMs_ = 0;
  node.pollBackoff_.disarm();
  node.statistics_.lastTurnaroundMs = nowMs - gateArmedMs_;
  ++statistics_.repliesAccepted;
  emitEvent_(CMRIHostEventType::kReplyAccepted, node, nowMs);
  emitEvent_(CMRIHostEventType::kPollBackoffChanged, node, nowMs,
             RemoteNodeState::kUninitialized,
             RemoteNodeState::kUninitialized, ReplyRejectReason::kNone, 0, 0,
             0, PollBackoffChangeReason::kAccept, previousBackoffMs,
             node.pollBackoffMs_);
  finishExchange_(nowMs);
}

/// Close the outstanding exchange and arm the pacing gate.
///
/// Clearing the orphan mark here is what bounds its lifetime to exactly
/// one exchange: every path out of an exchange funnels through this.
void CMRIHost::finishExchange_(uint32_t nowMs) {
  // Charge Gate B before the phase resets, while the grant record still
  // describes the exchange that just ended.
  chargeDegradedExchange_(nowMs);
  waitGate_.disarm();
  paceGate_.armIn(nowMs, config_.pollPacingMs);
  phase_ = Phase::kIdle;
  exchangeOrphaned_ = false;
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
    if (!selectNextNode_(nowMs)) {
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
    // Anti-starvation (map issue #41): outputsDirty_ may preempt a poll
    // with a transmit, but not indefinitely. pollDueBy_ is unarmed until
    // the first real poll (so the mandatory I->T bootstrap, interop
    // 2.3.1, is unaffected); once armed and due, force the poll through
    // regardless of outputsDirty_.
    const bool pollOverdue = node.pollDueBy_.due(nowMs);
    if (node.breakerState_ == ConformanceBreakerState::kOpen) {
      // A tripped breaker probes with a bare P and nothing else. Never a
      // re-init sequence: its ~500 ms post-I settle occupies the single
      // exchange slot, so probing a shelf of broken nodes that way would
      // stall the round-robin for the healthy ones. The whole point of
      // tripping was to stop paying that.
      // VALIDATION: Design v1.5 D17: probe with a bare P, never a
      // re-init sequence, whose post-I settle would stall the
      // round-robin.
      buildPollPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kPoll;
      node.pollDueBy_.armIn(nowMs, config_.maxOutputPreemptMs);
    } else if (node.needsInit_) {
      buildInitPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kInit;
    } else if (node.outputsDirty_ && !pollOverdue) {
      buildTransmitPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kTransmit;
    } else {
      buildPollPacket_(polledIndex_);
      outboundKind_ = OutboundKind::kPoll;
      node.pollDueBy_.armIn(nowMs, config_.maxOutputPreemptMs);
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
      if (!exchangeOrphaned_) {
        nodes_[polledIndex_].needsInit_ = false;
      }
    } else {  // kTransmit
      // T receives no reply (E8); the gap paces the next outbound.
      // VALIDATION: Interop v1.1 2.3.7: ~2 ms after T.
      waitGate_.armIn(nowMs, config_.postTxGapMs);
      if (!exchangeOrphaned_) {
        nodes_[polledIndex_].lastTxMs_ = nowMs;
      }
    }
    phase_ = Phase::kAwaitWait;
    return;
  }

  if (phase_ == Phase::kAwaitWait) {
    if (!waitGate_.due(nowMs)) {
      return;
    }
    if (exchangeOrphaned_) {
      // The gate expired on an exchange with no owner. Every branch
      // below either charges a node or sends it a follow-up frame, and
      // both would name the wrong device. In particular the kInit branch
      // would transmit a full T built from a tombstone -- or worse, from
      // whatever now occupies the slot.
      // VALIDATION: Design v1.2 D5: an orphaned exchange attributes
      // nothing to any node; the miss is not charged and no event is
      // emitted.
      finishExchange_(nowMs);
      return;
    }
    if (outboundKind_ == OutboundKind::kPoll) {
      // No reply inside the gate. Count the miss; the timed-out poll is
      // never retransmitted.
      // VALIDATION: Interop v1.1 2.3.9: do not retransmit a timed-out
      // message. Count the miss and poll the next Node.
      RemoteNodeHandle& node = nodes_[polledIndex_];
      ++node.statistics_.noReplies;
      // VALIDATION: Design v1.3 D15: miss-run counters are control
      // state and gate behavior, so they are not stored in observation
      // statistics.
      ++node.consecutiveMisses_;
      emitEvent_(CMRIHostEventType::kReplyTimeout, node, nowMs);
      // Poll-retry backoff (map issue #41): back off this node's next
      // poll attempt so a chronically offline node cannot tax the
      // round-robin's baseline cycle time -- pollDueBy_ depends on that
      // cycle time staying well under maxOutputPreemptMs. Doubles per
      // consecutive miss, capped at maxPollBackoffMs; independent of
      // missThreshold (that governs reported health / the reinit ladder,
      // not scheduling).
      const uint32_t previousBackoffMs = node.pollBackoffMs_;
      node.pollBackoffMs_ = (node.pollBackoffMs_ == 0)
                                ? config_.initialPollBackoffMs
                                : node.pollBackoffMs_ * 2;
      if (node.pollBackoffMs_ > config_.maxPollBackoffMs) {
        node.pollBackoffMs_ = config_.maxPollBackoffMs;
      }
      node.pollBackoff_.armIn(nowMs, node.pollBackoffMs_);
      emitEvent_(
          CMRIHostEventType::kPollBackoffChanged, node, nowMs,
          RemoteNodeState::kUninitialized,
          RemoteNodeState::kUninitialized, ReplyRejectReason::kNone, 0, 0, 0,
          previousBackoffMs == 0 ? PollBackoffChangeReason::kInitial
                                 : PollBackoffChangeReason::kMiss,
          previousBackoffMs, node.pollBackoffMs_);
      // More than missThreshold consecutive misses arms the re-init
      // ladder once per miss-run: the next slot re-sends I + full T and
      // has already invalidated cached inputs.
      // VALIDATION: Interop v1.1 2.3.10: after more than 5 consecutive
      // poll misses, re-send I, then a full T, and invalidate cached
      // input state. Keep polling the silent Node forever.
      if (node.consecutiveMisses_ > config_.missThreshold &&
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
/// no node is enabled, or when all enabled nodes are still backoff-armed.
///
/// Skips a node still serving its poll-retry backoff (map issue #41)
/// so the scheduler never resets another node's wall-clock deadline early.
///
/// The rotation walks slots, not live nodes, and the cursor is modulo
/// kMaxNodes. Modular arithmetic over nodeCount_ would be wrong the
/// moment a tombstone exists: the table is no longer a dense prefix, so
/// a live node above the count would never be reached.
// VALIDATION: Design v1.2 D5: delete tombstones a slot; the cursor must
// skip tombstones rather than assume a compacted table.
bool CMRIHost::selectNextNode_(uint32_t nowMs) {
  // Is there any healthy work to protect? The gates exist to bound
  // degraded impact *on healthy nodes* -- that is D17's whole
  // justification, both gates included. With no healthy node in the
  // rotation there is nothing to protect, and throttling anyway would
  // buy nothing while delaying every recovery: a lone silent node would
  // jump straight from the 250 ms backoff ladder to the ceiling clamp's
  // 32 s on its very first miss, pushing the interop 2.3.10 re-init
  // ladder from ~16 s out to over three minutes.
  //
  // So the gates are conditional on contention, and the per-node
  // liveness backoff below is what paces a degraded-only bus. That
  // backoff is unchanged by D17 and the bench shows it already works:
  // it is what held the silent node to 7 polls in 60 s.
  // VALIDATION: Design v1.5 D17: the gates bound degraded impact on
  // healthy nodes; the degraded class is never starved to zero.
  bool healthyContenders = false;
  for (size_t i = 0; i < kMaxNodes && !healthyContenders; ++i) {
    healthyContenders =
        occupied_[i] && nodes_[i].config_.enabled &&
        nodes_[i].serviceClass() == RemoteNodeServiceClass::kHealthy;
  }

  for (size_t step = 0; step < kMaxNodes; ++step) {
    const size_t candidate = (cursor_ + step) % kMaxNodes;
    if (!occupied_[candidate]) {
      continue;
    }
    RemoteNodeHandle& node = nodes_[candidate];
    if (!node.config_.enabled) {
      continue;
    }
    if (node.pollBackoff_.armed() && !node.pollBackoff_.due(nowMs)) {
      continue;  // still backed off; try the next enabled node first
    }
    // A tripped breaker paces its own probes. Kept separate from the
    // liveness backoff above because the two answer different failure
    // modes: the breaker is unconditional on conformance evidence, the
    // gates below are conditional on healthy contention. A tripped node
    // is degraded, so under contention its probe still passes both
    // gates; without contention the probe interval's own clamp is the
    // floor.
    // VALIDATION: Design v1.5 D17: the gates and the breaker have
    // different engagement conditions — gates under contention, breaker
    // always. A tripped node's probes ride the degraded lane.
    if (node.breakerState_ == ConformanceBreakerState::kOpen &&
        node.breakerProbe_.armed() && !node.breakerProbe_.due(nowMs)) {
      continue;
    }

    // Service class decides whether the two gates are consulted at all.
    // A healthy node is admitted without reference to either, so a
    // layout with no degraded node schedules exactly as it did before
    // D17 existed.
    // VALIDATION: Design v1.5 D17: the gates are consulted only for the
    // degraded class.
    const bool degraded =
        node.serviceClass() == RemoteNodeServiceClass::kDegraded;
    if (degraded && healthyContenders) {
      if (!admitDegraded_(node, nowMs)) {
        // Gated out. Keep walking: a healthy node further round the
        // rotation may still be admissible, and cursor_ is untouched so
        // the refusal costs this node no rotation position.
        continue;
      }
    } else if (!degraded) {
      // Healthy grants are what fund the degraded lane. Capped at one
      // banked degraded slot so a long healthy run cannot buy a burst.
      // VALIDATION: Design v1.5 D17: Gate A is a signed slot credit,
      // clamped at a burst ceiling.
      const int32_t share =
          static_cast<int32_t>(clampPercent(config_.degradedSlotSharePercent));
      const int32_t debit = 100 - share;
      degradedSlotCredit_ += share;
      if (degradedSlotCredit_ > debit) {
        degradedSlotCredit_ = debit;
      }
    }

    // Grant. Record the lane now rather than re-deriving it at
    // completion: a reply can end the miss run mid-exchange, and Gate B
    // must be charged for the lane that was actually spent.
    node.lastGrant_.mark(nowMs);
    exchangeDegraded_ = degraded;
    exchangeStartedMs_ = nowMs;
    if (node.breakerState_ == ConformanceBreakerState::kOpen) {
      node.breakerProbe_.armIn(nowMs, breakerProbeIntervalMs_());
    }
    polledIndex_ = candidate;
    cursor_ = (candidate + 1) % kMaxNodes;
    return true;
  }
  // Every enabled node is currently backed off (or none is enabled).
  // Do not force a poll ahead of any node's own backoff deadline.
  // Returning false lets tick() idle this iteration while preserving each
  // node's backoff timeline.
  // VALIDATION: Design v1.1 D6 compatibility: the engine still runs every
  // tick; backoff deadlines are the schedule, so idling here preserves
  // progress without violating interop's "poll forever" behavior (2.3.10).
  return false;
}

/// Two-gate admission for one degraded candidate.
///
/// Healthy candidates never reach here. Both gates must pass, except
/// under the ceiling clamp, which outranks them.
bool CMRIHost::admitDegraded_(RemoteNodeHandle& node, uint32_t nowMs) {
  const int32_t share =
      static_cast<int32_t>(clampPercent(config_.degradedSlotSharePercent));
  const int32_t debit = 100 - share;

  // The ceiling clamp outranks both gates, and it is not a nicety. When
  // every node is degraded there is no healthy grant to fund Gate A, so
  // the credit never rises, so the degraded class would be starved to
  // zero -- and a class served at zero rate can never demonstrate its
  // own recovery. Bypassing may push the aggregate briefly over budget,
  // deliberately: slightly over budget beats never noticed.
  // VALIDATION: Design v1.5 D17: the degraded class is never starved to
  // zero; the ceiling clamp guarantees this and may exceed budget.
  if (node.lastGrant_.atLeast(nowMs, config_.maxPollBackoffMs)) {
    ++statistics_.degradedClampBypasses;
    ++statistics_.degradedGrants;
    // Charged anyway. The bypass does not ask permission, but it does
    // pay, so a run of them drives the credit negative and the long-run
    // average is pulled back toward budget rather than drifting above
    // it forever.
    degradedSlotCredit_ -= debit;
    return true;
  }

  // Gate A: rotation slots. Nonconforming-but-answering nodes bind
  // here. They cost only turnaround, so a wall-clock budget alone
  // barely notices them -- 1206 polls in 60 s at ~15 ms each is a small
  // fraction of the wall clock and most of the rotation.
  // VALIDATION: Design v1.5 D17: Gate A bounds participation share and
  // trace noise.
  if (degradedSlotCredit_ < debit) {
    ++statistics_.degradedSlotDenials;
    return false;
  }

  // Gate B: wall-clock bandwidth. Silent nodes bind here. They cost a
  // full reply gate per probe, which a slot budget barely notices --
  // one slot, 250 ms.
  // VALIDATION: Design v1.5 D17: Gate B bounds cycle latency for
  // healthy nodes, and both gates must pass.
  if (clampPercent(config_.degradedBandwidthPercent) < 100u &&
      degradedBudgetMs_ <= 0) {
    ++statistics_.degradedBandwidthDenials;
    return false;
  }

  degradedSlotCredit_ -= debit;
  ++statistics_.degradedGrants;
  return true;
}

/// Credit Gate B for wall clock elapsed since the last refill.
void CMRIHost::refillDegradedBudget_(uint32_t nowMs) {
  if (!degradedRefillStarted_) {
    // Seeded from the first tick, never from zero. A sketch's first
    // tick(millis()) can arrive at any large value after boot, and
    // crediting that whole span would hand the degraded lane an
    // unbounded opening burst -- the same class of bug the explicit
    // never-marked state in CMRITime.h exists to prevent.
    degradedRefillAtMs_ = nowMs;
    degradedRefillStarted_ = true;
    return;
  }
  uint32_t elapsed = nowMs - degradedRefillAtMs_;
  if (elapsed == 0) {
    return;
  }
  if (elapsed > kMaxRefillSpanMs) {
    elapsed = kMaxRefillSpanMs;
  }
  degradedRefillAtMs_ = nowMs;

  // The remainder is carried, not discarded. Truncating each tick would
  // refill nothing at all whenever the tick interval times the
  // percentage falls below one millisecond -- which is the common case
  // for a fast loop() and a small budget.
  const uint32_t numerator =
      elapsed * clampPercent(config_.degradedBandwidthPercent) +
      degradedRefillRemainderMs_;
  degradedBudgetMs_ += static_cast<int32_t>(numerator / 100u);
  degradedRefillRemainderMs_ = numerator % 100u;

  const int32_t burst = static_cast<int32_t>(config_.degradedBurstMs);
  if (degradedBudgetMs_ > burst) {
    degradedBudgetMs_ = burst;
  }
}

/// Debit Gate B by what the finished degraded exchange actually cost.
void CMRIHost::chargeDegradedExchange_(uint32_t nowMs) {
  if (!exchangeDegraded_) {
    return;
  }
  exchangeDegraded_ = false;
  // Measured, never estimated per packet kind. Measuring is what keeps
  // the two gates independent: this same exchange already cost Gate A
  // exactly one turn, and it costs Gate B whatever it really took --
  // ~250 ms for a silent probe, ~15 ms for an answering one. A cost
  // table would have to be maintained against real hardware; a clock
  // difference does not.
  //
  // An orphaned exchange is charged too. The wall clock was spent
  // whether or not it can be attributed to a node, and Gate B is a
  // host-wide budget, so declining to charge would let repeated
  // mutation-during-exchange leak real time straight past the bound.
  // VALIDATION: Design v1.5 D17: Gate B is debited the measured
  // duration of each degraded exchange; an orphaned exchange still
  // debits it.
  degradedBudgetMs_ -= static_cast<int32_t>(nowMs - exchangeStartedMs_);
}

/// Arm one bounded corrective re-init: I, the full T that follows it,
/// and the invalidation.
///
/// LOAD-BEARING, and it does not look it. The invalidation here is the
/// only mechanism that completes STALE -> MISCONFIGURED for a node that
/// keeps answering. Interop 2.3.10's ladder is armed by silence, and a
/// node replying with the wrong geometry is never silent -- acceptReply_
/// zeroes its miss run on every mismatch, correctly, because the reply
/// does prove presence. Delete this call and such a node parks at STALE
/// with a growing age, reporting "your data is old" for what is really
/// "your geometry is wrong": the exact concealment #80 was filed about.
// VALIDATION: Design v1.5 D16: whether MISCONFIGURED depends on the
// breaker is path-dependent; the previously-conformed, still-answering
// path reaches it only through this corrective re-init.
// VALIDATION: Design v1.5 D17: bounded corrective re-init attempts
// precede the trip, each arming I, the full T, and the invalidation.
void CMRIHost::armCorrectiveReinit_(RemoteNodeHandle& node, uint32_t nowMs) {
  node.breakerState_ = ConformanceBreakerState::kReinitializing;
  ++node.breakerReinitAttempts_;
  // Reset the run so the next attempt needs a fresh one. Without this,
  // every later mismatch would arm another re-init on the spot and the
  // whole bounded budget would be spent in as many replies.
  node.consecutiveNonconforming_ = 0;
  node.needsInit_ = true;
  node.outputsDirty_ = true;
  invalidateNodeInputs_(node);
  emitEvent_(CMRIHostEventType::kReinitScheduled, node, nowMs);
}

/// Trip the breaker. Bare-P probes only from here, and never zero rate.
// VALIDATION: Design v1.5 D17: the breaker trips after a bounded number
// of corrective re-init attempts and is never a hard stop.
void CMRIHost::tripBreaker_(RemoteNodeHandle& node, uint32_t nowMs) {
  node.breakerState_ = ConformanceBreakerState::kOpen;
  node.conformanceBreakerOpen_ = true;
  node.consecutiveNonconforming_ = 0;
  node.breakerProbe_.armIn(nowMs, breakerProbeIntervalMs_());
  emitEvent_(CMRIHostEventType::kBreakerTripped, node, nowMs);
}

/// Return the breaker to closed. Pure state: no event, no re-init.
void CMRIHost::resetBreaker_(RemoteNodeHandle& node) {
  node.breakerState_ = ConformanceBreakerState::kClosed;
  node.conformanceBreakerOpen_ = false;
  node.consecutiveNonconforming_ = 0;
  node.breakerReinitAttempts_ = 0;
  node.breakerProbe_.disarm();
}

/// Re-close the breaker on current evidence.
void CMRIHost::closeBreaker_(RemoteNodeHandle& node, uint32_t nowMs) {
  const bool wasEngaged =
      node.breakerState_ != ConformanceBreakerState::kClosed;
  const bool wasOpen = node.breakerState_ == ConformanceBreakerState::kOpen;
  resetBreaker_(node);
  if (wasOpen) {
    // A reflashed node has lost its session, so it is owed a fresh I and
    // the full T that must follow. Only from open: a mid-ladder close
    // means the node answered correctly to a re-init already sent, so it
    // has a session and re-arming would buy nothing but another ~500 ms
    // settle.
    // VALIDATION: Design v1.5 D17: re-close arms the re-init ladder,
    // because a reflashed node has lost its session.
    node.needsInit_ = true;
    node.outputsDirty_ = true;
  }
  if (wasEngaged) {
    // Only when something actually closed. A stray mismatch followed by
    // a good reply clears the run counter without the breaker ever
    // having engaged, and reporting that as a close would put noise in
    // the event stream an analyzer has to filter back out.
    emitEvent_(CMRIHostEventType::kBreakerClosed, node, nowMs);
  }
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

/// Invalidate freshness for a node (re-init ladder) and keep the
/// last-good bytes: 0 is a valid consumer value, so zeroing the buffer
/// would assert "all clear" (the QBASIC review's F15 hazard). The next
/// state recomputation reports a stale image verdict when the node had
/// prior good data, and inputAgeMs() reports kNeverMarked.
/// VALIDATION: Interop v1.1 2.3.10: invalidate cached input state.
void CMRIHost::invalidateNodeInputs_(RemoteNodeHandle& node) {
  node.freshness_.clear();
}

/// Recompute every live node's health axes from control and belief
/// state. Tombstones are skipped: a cleaned slot has no health to
/// report, and recomputing one would emit a state-change event for a
/// node that no longer exists.
void CMRIHost::updateNodeStates_(uint32_t nowMs) {
  for (size_t i = 0; i < kMaxNodes; ++i) {
    if (!occupied_[i]) {
      continue;
    }
    RemoteNodeHandle& node = nodes_[i];
    const RemoteNodeState previous = node.state();
    if (node.consecutiveMisses_ > config_.missThreshold) {
      node.liveness_ = RemoteNodeLiveness::kSilent;
    } else if (node.consecutiveMisses_ != 0) {
      node.liveness_ = RemoteNodeLiveness::kMissing;
    } else {
      node.liveness_ = RemoteNodeLiveness::kResponsive;
    }

    // The image axis answers "is the cached image valid, and if so how
    // old" -- a belief-substrate question with two inputs and no
    // history component. A node that never replied, one whose image
    // re-init invalidation cleared (interop 2.3.10), and one whose
    // geometry changed under it all report kNone, because in all three
    // the cached image is equally unusable. "Has this node ever worked"
    // is statistics_.exchanges, which is observation, where D15 puts
    // history.
    // VALIDATION: Design v1.4 D15: belief carries the current verdict
    // only; history belongs to observation.
    // VALIDATION: Design v1.4 D16: the image axis is a validity claim,
    // so "never acquired" and "invalidated" both report kNone.
    if (!node.hasValidImage_()) {
      node.imageState_ = RemoteNodeImageState::kNone;
    } else if (node.config_.stalenessMs != 0 &&
               node.freshness_.atLeast(nowMs, node.config_.stalenessMs)) {
      node.imageState_ = RemoteNodeImageState::kStale;
    } else {
      node.imageState_ = RemoteNodeImageState::kFresh;
    }

    // VALIDATION: Design v1.3 D16: conformance is current evidence,
    // not latched. When a node is silent, conformance degrades to
    // unknown until current evidence returns.
    if (node.liveness_ == RemoteNodeLiveness::kSilent) {
      node.conformance_ = RemoteNodeConformance::kUnknown;
    }

    const RemoteNodeState current = node.state();
    if (current != previous) {
      emitEvent_(CMRIHostEventType::kNodeStateChanged, node, nowMs, previous,
                 current);
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
                          uint8_t replyMt,
                          PollBackoffChangeReason pollBackoffReason,
                          uint32_t previousPollBackoffMs,
                          uint32_t newPollBackoffMs) {
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
  // Classification is derived here rather than passed in, so the fault
  // and the reason cannot disagree at any call site.
  event.fault = conformanceFaultFor(rejectReason);
  // "Expected length" is only a comparison that was actually made at
  // the image rung. Asking layerOf() instead of re-testing the reason
  // keeps this from drifting if the mapping ever grows a rung (D14).
  event.expectedLength = (layerOf(event.fault) == ConformanceLayer::kImage)
                             ? node.config_.inputBytes
                             : 0;
  event.pollBackoffReason = pollBackoffReason;
  event.previousPollBackoffMs = previousPollBackoffMs;
  event.newPollBackoffMs = newPollBackoffMs;
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
    case CMRIHost::ConfigStatus::kTooManyNodes:       return "too many nodes";
    case CMRIHost::ConfigStatus::kAddressOutOfRange:  return "address out of range";
    case CMRIHost::ConfigStatus::kAddressInUse:       return "address in use";
    case CMRIHost::ConfigStatus::kNoSuchNode:         return "no such node";
    case CMRIHost::ConfigStatus::kInputBytesTooLarge: return "input bytes too large";
    case CMRIHost::ConfigStatus::kOutputBytesTooLarge:return "output bytes too large";
  }
  return "unknown";
}

ConformanceFault conformanceFaultFor(ReplyRejectReason reason) {
  switch (reason) {
    case ReplyRejectReason::kNone:
      return ConformanceFault::kNone;
    case ReplyRejectReason::kUaMismatch:
      return ConformanceFault::kPacketUnexpectedAddress;
    case ReplyRejectReason::kMtMismatch:
      return ConformanceFault::kPacketUnexpectedType;
    case ReplyRejectReason::kGeometryMismatch:
      return ConformanceFault::kImageGeometryMismatch;
  }
  return ConformanceFault::kNone;
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

const char* pollBackoffChangeReasonString(PollBackoffChangeReason reason) {
  switch (reason) {
    case PollBackoffChangeReason::kNone:              return "none";
    case PollBackoffChangeReason::kMiss:              return "miss";
    case PollBackoffChangeReason::kAccept:            return "accept";
    case PollBackoffChangeReason::kGeometryMismatch:  return "geometry-mismatch";
    case PollBackoffChangeReason::kInitial:           return "initial";
  }
  return "unknown";
}

}  // namespace CMRInet
