// CMRIHost.h — the CMRInet polled-strategy Host engine.
//
// The engine initiates every exchange. It speaks all four polled-strategy
// packet types: I (Initialize Session), P (Poll), T (Transmit Output Image),
// and R (Response from Node to a Poll). Per-node, the on-the-wire order is 
// I → T → P (interop 2.3.1: I precedes the first P, and a full T immediately 
// follows every I). I and T expect no reply (interop E8); P expects R.
//
// Default timing values track JMRI — the dominant fielded Host.
// JMRI is an evidence source behind this profile, but not the
// reference specification for this engine; the profile and
// Design are authoritative, and JMRI's behavior may change.
//
// VALIDATION: Design v1.1 D4: protocol-level concerns live in the
// strategy: poll schedule, reply-gate timeout, UA/MT reply
// verification, health. Byte-level concerns live in the transport.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>  // millis(), for the tick() convenience overload
#endif

#include "CMRIPacket.h"
#include "CMRITime.h"
#include "CMRITransport.h"
#include "RemoteNodeHandle.h"

// Geometry knob: the maximum number of remote nodes one Host manages.
// Each node costs a RemoteNodeHandle (image buffer included). Shrink
// the knob, or CMRINET_HOST_MAX_INPUT_BYTES, for small targets.
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time
// knobs.
#ifndef CMRINET_HOST_MAX_NODES
#define CMRINET_HOST_MAX_NODES 16
#endif

namespace CMRInet {

/// Host-wide polled-strategy defaults. Every value has a per-node
/// override path through CMRIHost::RemoteNodePolicy where one exists.
// VALIDATION: Design v1.1 D9: policy defaults match what JMRI-tuned
// Nodes expect: 250 ms reply-gate timeout, ~5 ms poll pacing, and a
// re-init trigger after more than 5 consecutive misses.
struct CMRIHostConfig {
  /// Reply-gate timeout: how long the Host waits for R after a poll
  /// finishes sending.
  // VALIDATION: Design v1.1 D2: the reply-gate timeout is strategy
  // policy, kept apart from the staleness threshold in the handle
  // contract.
  uint32_t replyTimeoutMs = 250;

  /// Idle time between the end of one exchange and the next poll.
  uint32_t pollPacingMs = 5;

  /// A node whose consecutive-miss count exceeds this value reports
  /// kOffline. Polling continues.
  // VALIDATION: Interop v1.1 2.3.10: a silent Node is polled forever
  // and its health state is exposed to the application.
  uint32_t missThreshold = 5;

  /// Settle time after an I: how long the engine waits before sending the
  /// immediate full T. I receives no reply (interop E8); this value paces
  /// the full T that follows every I (interop 2.3.1).
  // VALIDATION: Interop v1.1 2.3.7: ~500 ms settle after I.
  uint32_t postInitSettleMs = 500;

  /// Gap after a T before the next exchange. T receives no reply (interop
  /// E8); this gap paces the next outbound.
  // VALIDATION: Interop v1.1 2.3.7: ~2 ms after T.
  uint32_t postTxGapMs = 2;

  /// Optional periodic full-T refresh interval. 0 (default) disables it:
  //  T is sent only on change (Design v1.1 D9). A nonzero value re-sends a
  //  full T when now - lastTxMs_ >= transmitRefreshMs, covering node
  //  brownouts between output changes.
  // VALIDATION: Design v1.1 D9: T on change plus optional periodic
  //  refresh, off by default.
  uint32_t transmitRefreshMs = 0;

  /// Anti-starvation bound (map issue #41): the longest a node's due poll
  /// may be deferred in favor of a pending output transmit before the
  /// scheduler forces the poll through instead, regardless of
  /// outputsDirty_. Bounds input staleness to a fixed, human-factors-
  /// relevant window (e.g. a button press), independent of output-update
  /// cadence or how many other nodes share the round-robin. Relies on
  /// the poll-retry backoff below keeping the round-robin's healthy-node
  /// cycle time well under this value; without that, a chronically
  /// offline node sharing the rotation can invert this into transmit
  /// starvation instead of poll starvation.
  uint32_t maxOutputPreemptMs = 250;

  /// Poll-retry backoff for a node with consecutive misses: the first
  /// miss backs its next poll attempt off by this many ms; each further
  /// consecutive miss doubles the backoff, capped at maxPollBackoffMs.
  /// Any accepted reply clears the backoff immediately -- recovery is
  /// never throttled. Independent of missThreshold, which governs
  /// reported health and the re-init ladder, not scheduling.
  ///
  /// This is per-node pacing *within* the degraded lane. The aggregate
  /// bound across all degraded nodes is the two gates below (D17).
  uint32_t initialPollBackoffMs = 250;

  /// Ceiling clamp, and the degraded class's guaranteed-service floor.
  ///
  /// Two jobs, which is why it is not named for either one. It caps the
  /// exponential poll-retry backoff above, and it bounds how long a
  /// degraded node may be denied by the two gates: once its last
  /// granted slot is older than this, it is admitted with both gates
  /// bypassed.
  ///
  /// Demoted from primary throttle to clamp by D17 -- the
  /// operator-meaningful knob is now the budget. The bypass may push
  /// the aggregate briefly over budget, deliberately: slightly over
  /// budget beats never noticed, and a degraded class starved to zero
  /// can never demonstrate recovery.
  // VALIDATION: Design v1.5 D17: maxPollBackoffMs is demoted from
  // primary knob to ceiling clamp, and the clamp is what guarantees the
  // degraded class is never starved to zero.
  uint32_t maxPollBackoffMs = 32000;

  /// Gate A: the largest share of granted exchange slots, as a
  /// percentage, that degraded nodes may take collectively.
  ///
  /// Bounds participation share and trace noise. Nonconforming-but-
  /// answering nodes bind here: they are cheap in milliseconds and
  /// expensive in turns, so a wall-clock budget alone barely notices
  /// them. 100 disables the gate; 0 leaves only the ceiling clamp.
  // VALIDATION: Design v1.5 D17: Gate A bounds rotation slots.
  uint8_t degradedSlotSharePercent = 20;

  /// Gate B: the largest share of wall-clock time, as a percentage,
  /// that degraded exchanges may consume collectively.
  ///
  /// Bounds cycle latency for healthy nodes. Silent nodes bind here:
  /// each probe burns a full reply gate, which a slot budget barely
  /// notices. 100 disables the gate.
  // VALIDATION: Design v1.5 D17: Gate B bounds wall-clock bandwidth.
  uint8_t degradedBandwidthPercent = 10;

  /// Gate B burst ceiling: the most unspent degraded time, in ms, the
  /// budget may accumulate. Bounds what an idle spell can bank, so a
  /// quiet layout cannot fund one long unthrottled degraded burst.
  uint32_t degradedBurstMs = 1000;

  /// A run of this many consecutive nonconforming replies arms one
  /// corrective re-init. The conformance ladder's counterpart to
  /// missThreshold, and deliberately the same default: the two ladders
  /// answer the same shape of question about different evidence.
  ///
  /// Thresholding on a run rather than a single mismatch is what keeps
  /// D16's chronology intact -- with realistic staleness thresholds the
  /// image ages out before the ladder arms, so DEGRADED precedes STALE
  /// precedes MISCONFIGURED.
  // VALIDATION: Design v1.5 D17: the breaker arms on a run of
  // nonconforming replies, not on the first one.
  uint32_t conformanceReinitThreshold = 5;

  /// How many corrective re-init attempts the breaker makes before it
  /// trips. Bounded, never zero.
  ///
  /// LOAD-BEARING, and not an optimization target. The corrective
  /// re-init is the only mechanism that completes STALE ->
  /// MISCONFIGURED for a node that keeps answering, because interop
  /// 2.3.10's ladder is armed by silence and such a node is never
  /// silent. Set this to 0 and every previously-healthy misconfigured
  /// node strands at STALE with a growing age, reporting "your data is
  /// old" for what is actually "your geometry is wrong".
  // VALIDATION: Design v1.5 D17: the bounded corrective re-init must
  // run before the breaker trips; it is the sole writer that completes
  // the STALE -> MISCONFIGURED transition for an answering node.
  uint32_t conformanceReinitAttempts = 3;

  /// How often a tripped breaker probes, in ms. The probe is a bare P,
  /// never a re-init sequence: the post-I settle would stall the
  /// round-robin. Also clamped by maxPollBackoffMs, so the probe rate
  /// is never zero -- zero traffic means no evidence of recovery can
  /// ever arrive.
  // VALIDATION: Design v1.5 D17: a tripped breaker probes at a low rate
  // with a bare P and is never a hard stop.
  uint32_t breakerProbeIntervalMs = 30000;
};

/// What the Host engine is reporting through its event listener.
// VALIDATION: Design v1.1 D7: observability is listener registration
// (JMRI pattern): metrics, monitor, and trace hooks are optional
// listeners the linker drops when unused.
enum class CMRIHostEventType : uint8_t {
  kReplyAccepted,     ///< verified reply committed to the node image
  kReplyRejected,     ///< reply discarded: UA/MT mismatch or bad geometry
  kReplyTimeout,      ///< reply gate expired with no reply (a miss)
  kReinitScheduled,   ///< re-init ladder armed: I + full T + invalidation owed
  kPollBackoffChanged,///< per-node poll backoff duration changed
  kNodeStateChanged,  ///< node health moved between states
  kBreakerTripped,    ///< conformance breaker opened: bare-P probes only
  kBreakerClosed,     ///< conformance breaker re-closed; re-init ladder armed
  // Runtime node-table mutation (issue #91). These fire from inside the
  // mutators, so a listener sees the table change whether it came from a
  // C&C verb or a direct API call — the gap that left a mid-run topology
  // change invisible in the trace stream (only the roster hinted at it).
  kNodeAdded,         ///< a node was added to the table (node is the new handle)
  kNodeDeleted,       ///< a node was deleted (node is null; departedUA carries identity)
  kGeometryChanged,   ///< a node's declared NI/NO changed in place (old + new carried)
};

/// Why a reply was rejected. Meaningful only when a CMRIHostEvent
/// carries kReplyRejected; kNone otherwise. Lets a conformance-checking
/// sketch print "expected N in, got M" or "polled UA x, got UA y"
/// without reflashing the remote node.
///
/// This is the polled strategy's own local vocabulary. It maps *into*
/// the role-neutral ConformanceFault taxonomy through
/// conformanceFaultFor() below, and is not promoted to it: a push
/// strategy has geometry disagreements but no notion of a rejected
/// reply (Design v1.3 D14).
enum class ReplyRejectReason : uint8_t {
  kNone,              ///< not a rejection event
  kWireUAMismatch,        ///< reply UA != the polled node's UA
  kMtMismatch,        ///< reply MT != 'R' (kReceiveData)
  kGeometryMismatch,  ///< reply length != configured inputBytes
};

/// Map a polled-strategy reject reason into the shared fault taxonomy.
///
/// Total and deterministic, which is why the event can carry the
/// classified fault without the emitting call site passing one: the
/// reason already determines it. Callers derive layer and attribution
/// from the result with layerOf() and attributionOf() rather than
/// reading stored fields (D14).
// VALIDATION: Design v1.3 D14: the polled engine's ReplyRejectReason
// maps into the role-neutral fault vocabulary rather than being
// promoted to it.
ConformanceFault conformanceFaultFor(ReplyRejectReason reason);

/// Why pollBackoffMs_ changed for a node.
enum class PollBackoffChangeReason : uint8_t {
  kNone,               ///< not a backoff-change event
  kMiss,               ///< timeout miss increased backoff
  kAccept,             ///< accepted reply cleared backoff
  kGeometryMismatch,   ///< geometry-mismatch reply cleared backoff
  kInitial,            ///< first miss initialized backoff from zero
};

/// One engine event. `node` is the node of the exchange or state
/// change. The storage outlives the callback -- slots are never
/// deallocated -- but a listener must not retain the pointer past the
/// call: the node it names can be deleted, and its slot reused by a
/// different device. `previousState`/`newState` are meaningful only for
/// kNodeStateChanged.
struct CMRIHostEvent {
  CMRIHostEventType type = CMRIHostEventType::kReplyAccepted;
  const RemoteNodeHandle* node = nullptr;
  uint32_t nowMs = 0;
  RemoteNodeState previousState = RemoteNodeState::kUninitialized;
  RemoteNodeState newState = RemoteNodeState::kUninitialized;
  /// Reject details. Meaningful only when type == kReplyRejected.
  ReplyRejectReason rejectReason = ReplyRejectReason::kNone;
  uint16_t replyLength = 0;  ///< body byte count of the rejected reply
  uint8_t  replyWireUA = 0;      ///< UA byte of the rejected reply
  uint8_t  replyMt = 0;      ///< MT byte of the rejected reply

  /// `rejectReason` classified into the shared taxonomy. Derived from
  /// the reason, so it needs no separate argument at the emitting call
  /// site and cannot disagree with it.
  ///
  /// Every reject reason maps, including the two that do not move the
  /// node's stored conformance axis: a listener should see the full
  /// classification of what arrived on the wire even when the Host
  /// declines to attribute it to the polled node. Ask layerOf() and
  /// attributionOf() for the axes; they are not stored here.
  ConformanceFault fault = ConformanceFault::kNone;

  /// The Host's assumption that `replyLength` contradicted -- the node's
  /// declared input byte count. Populated only when `fault` is an
  /// image-layer fault, where a length comparison is what was made;
  /// zero otherwise, since "expected length" means nothing for a UA or
  /// MT disagreement.
  uint16_t expectedLength = 0;
  /// Backoff details. Meaningful only when type == kPollBackoffChanged.
  PollBackoffChangeReason pollBackoffReason = PollBackoffChangeReason::kNone;
  uint32_t previousPollBackoffMs = 0;
  uint32_t newPollBackoffMs = 0;

  /// Identity for kNodeDeleted. The slot is cleaned before the event
  /// fires (D5: delete tombstones and resets in place), so `node` is
  /// null by the time a listener runs and the departing node's UA
  /// rides by value here. A handle cached across the delete would read
  /// UA 0; this is the value the listener needs to name the node
  /// that just left.
  uint8_t departedUA = 0;

  /// Geometry carried by kGeometryChanged, in data bytes. `previous*`
  /// is the declared NI/NO before the in-place change; the new values
  /// are on `node->inputLength()` / `node->outputLength()` (the handle
  /// already holds the new geometry by the time the event fires). A
  /// reader can tell what changed without dereferencing the handle —
  /// which matters for the bench capture, where the question "was a
  /// card added or removed" has different remedies (D14: a geometry
  /// disagreement is a configuration fix, not a firmware fix).
  uint16_t previousInputBytes = 0;
  uint16_t previousOutputBytes = 0;
};

/// Event listener. Plain function pointer with a context cookie
/// (Design v1.1 D7: no std::function). Called from inside tick();
/// listeners must not block and must not call back into the engine.
using CMRIHostEventListener = void (*)(void* context,
                                       const CMRIHostEvent& event);

/// Trace listener: every packet the engine hands to the transport
/// (transmit == true) and every packet the transport hands up
/// (transmit == false), before verification. The packet reference is
/// valid only for the duration of the call.
using CMRIHostTraceListener = void (*)(void* context, bool transmit,
                                       const CMRIPacket& packet);

/// Host-wide counters. All counters start at 0 at begin(), increase
/// monotonically, and never reset.
struct CMRIHostStatistics {
  uint32_t pollsSent = 0;          ///< P packets accepted by the transport
  uint32_t pollSendRetries = 0;    ///< sendPacket() refusals, retried later
  uint32_t repliesAccepted = 0;    ///< R packets committed to a node image
  uint32_t repliesRejected = 0;    ///< packets discarded while a poll was outstanding
  uint32_t unsolicitedPackets = 0; ///< packets received with no poll outstanding

  /// Exchanges whose node was mutated away mid-flight. The frame
  /// completed, any reply was discarded, and nothing was attributed to
  /// any node -- so the packet ledger would otherwise lose the frame
  /// silently. Host-wide by construction: an orphan has no node to
  /// charge it to.
  // VALIDATION: Design v1.2 D5: a mutation of the outstanding
  // exchange's node orphans that exchange rather than aborting a send
  // already on the wire (D13).
  uint32_t orphanedExchanges = 0;

  /// Degraded-lane allocator ledger (D17). Host-wide because the bound
  /// is on the aggregate, not per node -- organic rot produces several
  /// broken nodes at once, and a per-node bound would let their sum
  /// grow without limit.
  ///
  /// Grants plus the two denial counters do not sum to a fixed total: a
  /// candidate denied by Gate A is never tested against Gate B, so each
  /// denial names the gate that actually bound. That asymmetry is the
  /// point -- it says which failure mode is costing the layout.
  uint32_t degradedGrants = 0;          ///< slots granted to degraded nodes
  uint32_t degradedSlotDenials = 0;     ///< Gate A refusals
  uint32_t degradedBandwidthDenials = 0;///< Gate B refusals

  /// Grants that bypassed both gates because the ceiling clamp expired.
  /// The never-starved invariant made countable: a layout where this is
  /// the only source of degraded grants is one where the budget is too
  /// tight for its degraded population.
  uint32_t degradedClampBypasses = 0;
};

/// The polled-strategy Host engine.
///
/// Lifecycle: construct with a transport, add nodes, then begin().
/// begin() moves the engine from configuration to running; it does not
/// lock the node table. After begin(), the engine allocates nothing.
// VALIDATION: Design v1.2 D7: the embedded profile allocates only
// during setup and never frees.
// VALIDATION: Design v1.2 D8: table *capacity* is the compile-time knob
// CMRINET_HOST_MAX_NODES.
// VALIDATION: Design v1.2 D5: table *membership* is mutable at runtime
// within that ceiling -- add, delete, and geometry change are all legal
// after begin(). Capacity and membership are independent: an add
// searches for a free slot, so deleting from a full table frees one and
// the next add succeeds.
///
/// Runtime: call tick(nowMs) from loop(). The engine advances only
/// inside tick() and never blocks, sleeps, or busy-waits.
// VALIDATION: Design v1.1 D6: non-blocking tick with injected time.
// The engine runs against a mock clock and mock transport in desktop
// tests.
class CMRIHost {
 public:
  static constexpr size_t kMaxNodes = CMRINET_HOST_MAX_NODES;

  /// Per-node polled-strategy overrides. Nested here so ownership is
  /// unambiguous: these knobs exist only while this engine runs the
  /// exchange.
  struct RemoteNodePolicy {
    /// Use the host-wide CMRIHostConfig value.
    static constexpr uint32_t kInheritHost = 0;

    /// Reply-gate timeout for this node. kInheritHost selects the
    /// host-wide default.
    // VALIDATION: Interop v1.1 2.3.7: timeouts are wall-clock and
    // configurable per Node, because reply latency is Node-version
    // dependent (2.3.8).
    uint32_t replyTimeoutMs = kInheritHost;

    /// Transmission delay dH (high byte) and dL (low byte), in 10 µs units
    /// per the spec. 0/0 (default) means no inter-character pacing. Exposed
    /// per node for slow-receiver compatibility (erratum E4); modern Hosts
    /// send zero. Carried in the CPNODE I body (interop E3).
    // VALIDATION: Interop v1.1 E4: the delay is the minimum idle time the
    // Node inserts after each transmitted character; one unit = 10 µs.
    // VALIDATION: Design v1.1 D2: a polled-strategy knob, kept in policy,
    // not the strategy-neutral handle config.
    uint8_t transmissionDelayDh = 0;
    uint8_t transmissionDelayDl = 0;
  };

  /// Result of one configuration call. Returned directly by each
  /// mutator; there is no sticky host-wide status and no chaining.
  ///
  /// A rejected call affects only itself: the next call is evaluated on
  /// its own merits. The previous batch model short-circuited every
  /// later add after the first rejection, which suited a hardcoded test
  /// rig but silently disabled runtime reconfiguration through the
  /// verb-based C&C shell (Design v1.2 D5).
  enum class ConfigStatus : uint8_t {
    kOk,
    kTooManyNodes,        ///< every slot is occupied (CMRINET_HOST_MAX_NODES)
    kUAOutOfRange,   ///< UA > 127
    kUAInUse,        ///< a live node already holds that UA
    kNoSuchNode,          ///< no live node holds that UA
    kInputBytesTooLarge,  ///< inputBytes > RemoteNodeHandle::kMaxInputBytes
    kOutputBytesTooLarge, ///< outputBytes > RemoteNodeHandle::kMaxOutputBytes
  };

  /// The engine holds the transport reference for its whole life.
  explicit CMRIHost(CMRITransport& transport,
                    const CMRIHostConfig& config = CMRIHostConfig());

  /// Add one remote node. Returns this call's own status: kOk, or the
  /// reason this call was rejected. The result of one add never affects
  /// another.
  ///
  /// `UA` is the node UA (0..127). The wire UA is
  /// UA + 65. Legal before and after begin(). A call is rejected
  /// when no slot is free, when the UA is out of range or already
  /// held by a live node, or when config.inputBytes exceeds
  /// RemoteNodeHandle::kMaxInputBytes.
  ///
  /// The add takes the first free slot -- a cleaned tombstone, or a
  /// slot never used. Capacity is therefore slot availability, not a
  /// high-water count, so deleting from a full table frees a slot and
  /// the next add succeeds.
  ///
  /// A reused slot starts pristine across all three substrates,
  /// statistics included: delete ended the previous subject, and the
  /// new occupant is a different logical device whose counters begin at
  /// zero. Nothing was reset mid-life, so monotonicity is intact.
  ///
  /// Callers registering several nodes decide for themselves how to
  /// treat a partial failure; the engine holds no opinion and no
  /// residual state.
  // VALIDATION: Interop v1.1 2.3.4: a Host supports UA 0-127 and
  // flags addresses above 64, which the cpNode family cannot use.
  // VALIDATION: Design v1.2 D5: every mutator reports its own outcome
  // immediately; no sticky, chain-poisoning status.
  // VALIDATION: Design v1.2 D5: slot reuse resets all three substrates
  // (D15) -- freshness especially, or a newly added node reports data
  // it never sent.
  ConfigStatus addRemoteNode(uint8_t UA,
                             const RemoteNodeConfig& config,
                             const RemoteNodePolicy& policy);

  /// As above, with the host-wide policy defaults.
  ConfigStatus addRemoteNode(uint8_t UA,
                             const RemoteNodeConfig& config);

  /// Convenience: register a node from its UA and input/output byte
  /// counts, with host-wide policy defaults and default staleness/enabled.
  /// Equivalent to building a RemoteNodeConfig and calling the overload
  /// above. The flat form keeps simple sketches readable (issue #31).
  ConfigStatus addRemoteNode(uint8_t UA,
                             uint16_t inputBytes,
                             uint16_t outputBytes);

  /// Delete the node at `UA`. Returns kNoSuchNode when no live node
  /// holds it. Legal before and after begin().
  ///
  /// The slot is tombstoned and cleaned, never compacted: no surviving
  /// handle relocates. Cleaning at delete rather than at reuse is what
  /// makes UA() a working self-check -- a handle cached across the
  /// delete reports 0, not the UA it used to serve.
  ///
  /// Deleting the node of the outstanding exchange is legal. If the
  /// packet has not yet been accepted by the transport the exchange is
  /// cancelled outright, because nothing is on the wire to protect. Once
  /// accepted it cannot be aborted, so the exchange is orphaned instead:
  /// the frame completes, any reply is discarded, and no counter and no
  /// event is attributed to any node.
  // VALIDATION: Design v1.2 D5: delete tombstones the slot; a later add
  // may reuse a cleaned tombstone. Never compaction, so handles never
  // relocate.
  // VALIDATION: Design v1.2 D5: UA is identity, so changing a
  // node's UA is delete + add, never an in-place mutation.
  ConfigStatus deleteRemoteNode(uint8_t UA);

  /// Change the declared geometry of the node at `UA`, in place.
  /// Returns kNoSuchNode when no live node holds it, or the same byte-
  /// ceiling rejections addRemoteNode() applies.
  ///
  /// Identity is preserved -- same UA, same wire UA, same
  /// statistics -- because this is the same logical device with its IO
  /// cards rearranged. The cached input image is invalidated and a
  /// re-init is forced (I, then a full T), because the NI/NO announced
  /// in the I body has changed. The output image is cleared: bit
  /// positions under the old geometry mean nothing under the new one.
  ///
  /// Conformance degrades to unknown, since prior evidence was gathered
  /// against a geometry that no longer applies (D16: conformance is
  /// current evidence, not latched).
  ///
  /// Mutating the node of the outstanding exchange follows the same
  /// cancel-or-orphan rule as deleteRemoteNode(): the in-flight I
  /// announced the geometry we just replaced.
  // VALIDATION: Design v1.2 D5: geometry change is in place, identity
  // preserved; it invalidates the cached input image and forces a
  // re-init because the NI/NO announced in the I body has changed.
  ConfigStatus setRemoteNodeGeometry(uint8_t UA,
                                     uint16_t inputBytes,
                                     uint16_t outputBytes);

  /// Register the optional event listener (nullptr to clear). Legal at
  /// any time, before or after begin().
  ///
  /// This used to be refused after begin(), silently, back when begin()
  /// locked the whole configuration. D5 retired that doctrine for the
  /// node table, and keeping the listener half of it would have left
  /// registration as the one mutator that fails without saying so --
  /// exactly the failure surface D5 forbids. There is nothing to report
  /// now: the call always succeeds, so it returns void honestly rather
  /// than by omission.
  ///
  /// Safe to swap between ticks: the engine reads the pointer only
  /// inside tick(), and listeners are forbidden from calling back into
  /// the engine, so there is no reentrancy to protect against.
  // VALIDATION: Design v1.2 D5: begin() marks the config->running
  // transition and locks nothing.
  // VALIDATION: Design v1.1 D7: observability is listener registration;
  // a null listener costs one branch.
  void onEvent(CMRIHostEventListener listener, void* context = nullptr) {
    eventListener_ = listener;
    eventContext_ = context;
  }

  /// Register the optional TX/RX packet trace listener (nullptr to
  /// clear). Legal at any time; see onEvent() above.
  void onTrace(CMRIHostTraceListener listener, void* context = nullptr) {
    traceListener_ = listener;
    traceContext_ = context;
  }

  /// Move from configuration to running and begin() the transport.
  /// Idempotent. Reports nothing, because each configuration call has
  /// already reported itself.
  // VALIDATION: Design v1.2 D5: begin() marks the config->running
  // transition; it is not a deferred error channel.
  void begin();

  /// Advance the engine to `nowMs`. Ticks the transport first, then
  /// runs the poll schedule. `nowMs` must be monotonic.
  void tick(uint32_t nowMs);

#ifdef ARDUINO
  /// Convenience for sketches: tick with the Arduino clock.
  void tick() { tick(millis()); }
#endif

  /// Cumulative host-wide counters.
  const CMRIHostStatistics& statistics() const { return statistics_; }

  /// Live nodes in the table. Tombstoned slots do not count, so this
  /// falls when a node is deleted and the invariant
  /// nodeCount() <= kMaxNodes always holds.
  size_t nodeCount() const { return nodeCount_; }

  /// Look up a live node by UA. Returns nullptr when no live node
  /// holds that UA. Valid before and after begin().
  ///
  /// This is the canonical access path and is cheap enough to call at
  /// the point of use. A handle stays valid until that node is deleted;
  /// caching one across a mutation is not a supported pattern, and
  /// UA() is the self-check for code that does it anyway.
  // VALIDATION: Design v1.2 D5: host.node(addr) is the canonical access
  // path; a handle is valid until that node is deleted.
  RemoteNodeHandle* node(uint8_t UA);

 private:
  enum class Phase : uint8_t {
    kIdle,              ///< between exchanges; pacing gate runs here
    kSendOutbound,      ///< packet built; transport has not accepted it yet
    kAwaitSendComplete, ///< packet accepted; waiting for full delivery
    kAwaitWait,         ///< post-send wait armed (reply gate, settle, or gap)
  };

  /// Which kind of packet the outstanding exchange carries. Drives the
  /// post-send wait: kPoll arms the reply gate; kInit arms the post-I
  /// settle; kTransmit arms the post-T gap.
  enum class OutboundKind : uint8_t {
    kInit,     ///< I — session setup, no reply expected (interop E8)
    kPoll,     ///< P — media access, R reply expected
    kTransmit, ///< T — full output image, no reply expected (interop E8)
  };

  /// Find the slot holding a live node at `UA`. Occupancy is
  /// checked first: a cleaned tombstone has UA_ == 0, which is a
  /// perfectly valid UA for some other node.
  bool findSlot_(uint8_t UA, size_t& slot) const;

  /// Return a slot to pristine state across all three substrates (D15).
  /// Assignment from a default-constructed handle is deliberate: it is
  /// structurally exhaustive, so a substrate added later cannot be
  /// forgotten here the way a hand-written field list would forget it.
  void resetSlot_(size_t slot);

  /// Detach the outstanding exchange from `slot` when a mutation makes
  /// that slot's identity a lie. Cancels outright when the packet has
  /// not reached the transport; orphans it when the bytes are already
  /// committed to the wire (D13 forbids truncating those).
  void detachExchangeFrom_(size_t slot);

  void drainReceive_(uint32_t nowMs);
  void runSchedule_(uint32_t nowMs);
  bool selectNextNode_(uint32_t nowMs);

  /// Two-gate admission for one degraded candidate (D17). Healthy
  /// candidates never reach here. Charges the ledger and the gates on
  /// success; counts the binding gate on refusal.
  bool admitDegraded_(RemoteNodeHandle& node, uint32_t nowMs);

  /// Credit Gate B's leaky bucket for wall clock elapsed since the last
  /// refill, capped at the burst ceiling.
  void refillDegradedBudget_(uint32_t nowMs);

  /// Effective probe interval for a tripped breaker: the configured
  /// interval, clamped by maxPollBackoffMs so the probe rate can never
  /// fall below the guaranteed-service floor. Zero traffic means no
  /// evidence of recovery can ever arrive.
  uint32_t breakerProbeIntervalMs_() const {
    return config_.breakerProbeIntervalMs > config_.maxPollBackoffMs
               ? config_.maxPollBackoffMs
               : config_.breakerProbeIntervalMs;
  }

  /// Debit Gate B by the measured duration of the exchange just
  /// finished, when that exchange was granted to the degraded lane.
  void chargeDegradedExchange_(uint32_t nowMs);

  /// Arm one bounded corrective re-init: I, the full T that must follow
  /// it, and the invalidation that clears freshness.
  ///
  /// LOAD-BEARING. This invalidation is the only mechanism that
  /// completes STALE -> MISCONFIGURED for a node that keeps answering.
  void armCorrectiveReinit_(RemoteNodeHandle& node, uint32_t nowMs);

  /// Trip the breaker: bare-P probes only from here.
  void tripBreaker_(RemoteNodeHandle& node, uint32_t nowMs);

  /// Return the breaker to closed and clear the conformance ladder.
  /// Pure state, no event and no re-init arming -- for callers that
  /// have no clock, or that have already armed one themselves.
  void resetBreaker_(RemoteNodeHandle& node);

  /// Re-close the breaker on current evidence, arming the re-init
  /// ladder when it was open, because a reflashed node has lost its
  /// session. Emits kBreakerClosed.
  void closeBreaker_(RemoteNodeHandle& node, uint32_t nowMs);
  void buildInitPacket_(size_t nodeIndex);
  void buildTransmitPacket_(size_t nodeIndex);
  void buildPollPacket_(size_t nodeIndex);
  void invalidateNodeInputs_(RemoteNodeHandle& node);
  void acceptReply_(const CMRIPacket& reply, uint32_t nowMs);
  void finishExchange_(uint32_t nowMs);
  void updateNodeStates_(uint32_t nowMs);
  uint32_t replyTimeoutFor_(size_t nodeIndex) const;
  void emitEvent_(CMRIHostEventType type, const RemoteNodeHandle& node,
                  uint32_t nowMs,
                  RemoteNodeState previousState = RemoteNodeState::kUninitialized,
                  RemoteNodeState newState = RemoteNodeState::kUninitialized,
                  ReplyRejectReason rejectReason = ReplyRejectReason::kNone,
                  uint16_t replyLength = 0,
                  uint8_t replyWireUA = 0,
                  uint8_t replyMt = 0,
                  PollBackoffChangeReason pollBackoffReason =
                      PollBackoffChangeReason::kNone,
                  uint32_t previousPollBackoffMs = 0,
                  uint32_t newPollBackoffMs = 0);

  /// Deliver a pre-built event to the listener, if one is registered.
  /// This is the dispatch tail shared by emitEvent_() (which builds the
  /// exchange/health events) and the mutators (which build the
  /// node-table-mutation events inline, since those do not fit
  /// emitEvent_'s exchange-shaped parameter list). A null listener costs
  /// one branch, per the D7 observability rule.
  void fire_(CMRIHostEvent& event);
  void emitTrace_(bool transmit, const CMRIPacket& packet);

  CMRITransport& transport_;
  CMRIHostConfig config_;
  CMRIHostStatistics statistics_;

  RemoteNodeHandle nodes_[kMaxNodes];
  RemoteNodePolicy policies_[kMaxNodes];

  // Slot occupancy. The table is not a dense prefix: delete tombstones a
  // slot in place so handles never relocate, which means every scan must
  // consult this rather than stopping at nodeCount_. Occupancy lives
  // here and not on the handle because it describes the engine's slot,
  // not the product-facing node -- and because resetSlot_() wipes the
  // handle wholesale.
  bool occupied_[kMaxNodes] = {false};

  /// Live nodes, not a high-water mark. Bookkeeping only: no scan is
  /// bounded by it, or a tombstone below the bound would be skipped.
  size_t nodeCount_ = 0;

  CMRIHostEventListener eventListener_ = nullptr;
  void* eventContext_ = nullptr;
  CMRIHostTraceListener traceListener_ = nullptr;
  void* traceContext_ = nullptr;

  /// The engine clock as of the most recent tick(). Mutators run outside
  /// tick() and have no clock of their own, so node-table-mutation events
  /// stamp `nowMs` from this. Zero until the first tick(); a mutation
  /// before then honestly reports "no clock yet" (issue #91). Not refused:
  /// a sketch's compiled-in adds before begin() are a legitimate boot-time
  /// path, and refusing them would break node registration.
  uint32_t lastTickMs_ = 0;
  bool began_ = false;
  Phase phase_ = Phase::kIdle;
  OutboundKind outboundKind_ = OutboundKind::kPoll;
  size_t cursor_ = 0;       ///< round-robin position: next slot to consider
  size_t polledIndex_ = 0;  ///< slot of the outstanding exchange

  // The outstanding exchange lost its node to a mutation. polledIndex_
  // is stale from that moment -- the slot may be a tombstone, or may
  // already hold a different logical device -- so this mark, not the
  // index, is what says whether the exchange still has an owner.
  //
  // It lives on the engine rather than the handle deliberately: a mark
  // stored on the handle would be inherited by the next occupant of a
  // reused slot. Cleared by finishExchange_().
  //
  // While set, the exchange attributes nothing: no node counter, and no
  // event either. emitEvent_() takes a const RemoteNodeHandle&, so an
  // event fired here would name whatever now occupies the slot.
  bool exchangeOrphaned_ = false;
  CMRIPacket outbound_;     ///< the packet in flight (I, P, or T)
  Deadline paceGate_;       ///< time gate between exchanges
  Deadline waitGate_;       ///< post-send wait: reply gate / settle / gap
  uint32_t gateArmedMs_ = 0;  ///< when the wait was armed (turnaround base)

  // ---- degraded-lane allocator (D17), host-wide ----
  //
  // Host-wide and not per node, because the bound is on the aggregate:
  // organic rot produces several broken nodes at once, and per-node
  // budgets would let their sum grow without limit.

  /// Gate A. A healthy grant credits degradedSlotSharePercent; a
  /// degraded grant debits (100 - that). Equilibrium is exactly the
  /// configured share, with no division on the hot path and no window
  /// history to store. Signed because the debit lands before the next
  /// credit does.
  int32_t degradedSlotCredit_ = 0;

  /// Gate B. Milliseconds of degraded exchange time still affordable.
  /// Signed deliberately: a slot is granted before its cost is known,
  /// so a single over-budget exchange may drive this negative and is
  /// repaid by refill rather than forgiven.
  int32_t degradedBudgetMs_ = 0;

  /// Sub-millisecond remainder carried between refills, so a slow
  /// percentage does not truncate to zero on every short tick.
  uint32_t degradedRefillRemainderMs_ = 0;

  /// Clock base for the next refill. Started at begin(), because a zero
  /// base would credit the entire epoch on the first tick.
  uint32_t degradedRefillAtMs_ = 0;
  bool degradedRefillStarted_ = false;

  /// The outstanding exchange was granted to the degraded lane, and
  /// when it started. Recorded at grant time rather than read back from
  /// the node at completion: service class can change mid-exchange (a
  /// reply arrives and clears the miss run), and Gate B must be charged
  /// for the lane that was actually spent.
  bool exchangeDegraded_ = false;
  uint32_t exchangeStartedMs_ = 0;
};

/// Human-readable name for a CMRIHost::ConfigStatus value.
const char* configStatusString(CMRIHost::ConfigStatus status);

/// Human-readable name for a ReplyRejectReason value.
const char* replyRejectReasonString(ReplyRejectReason reason);

/// Human-readable name for a PollBackoffChangeReason value.
const char* pollBackoffChangeReasonString(PollBackoffChangeReason reason);

}  // namespace CMRInet
