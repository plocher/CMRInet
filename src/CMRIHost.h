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
  uint32_t initialPollBackoffMs = 250;

  /// Upper bound for the poll-retry backoff above.
  uint32_t maxPollBackoffMs = 32000;
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
  kNodeStateChanged,  ///< node health moved between states
};

/// Why a reply was rejected. Meaningful only when a CMRIHostEvent
/// carries kReplyRejected; kNone otherwise. Lets a conformance-checking
/// sketch print "expected N in, got M" or "polled UA x, got UA y"
/// without reflashing the remote node.
enum class ReplyRejectReason : uint8_t {
  kNone,              ///< not a rejection event
  kUaMismatch,        ///< reply UA != the polled node's UA
  kMtMismatch,        ///< reply MT != 'R' (kReceiveData)
  kGeometryMismatch,  ///< reply length != configured inputBytes
};

/// One engine event. `node` is the node of the exchange or state
/// change; it outlives the callback (nodes are never deallocated).
/// `previousState`/`newState` are meaningful only for
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
  uint8_t  replyUa = 0;      ///< UA byte of the rejected reply
  uint8_t  replyMt = 0;      ///< MT byte of the rejected reply
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
};

/// The polled-strategy Host engine.
///
/// Lifecycle: construct with a transport, add nodes, then begin().
/// begin() locks the configuration. After begin(), the engine
/// allocates nothing and nodes cannot be removed.
// VALIDATION: Design v1.1 D5: two-phase lifecycle. Allocation is
// legal only before begin() locks the configuration.
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

  /// Result of the configuration phase, reported by begin(). kOk means
  /// every addRemoteNode() call was accepted. Any other value is the
  /// reason the first rejected call failed; later calls in the chain
  /// are short-circuited, so only the first reason is reported.
  enum class ConfigStatus : uint8_t {
    kOk,
    kAlreadyBegun,        ///< the time for config changes has ended
    kTooManyNodes,        ///< the node table is full (CMRINET_HOST_MAX_NODES)
    kAddressOutOfRange,   ///< address > 127
    kAddressInUse,        ///< address already added by an earlier call
    kInputBytesTooLarge,  ///< inputBytes > RemoteNodeHandle::kMaxInputBytes
    kOutputBytesTooLarge, ///< outputBytes > RemoteNodeHandle::kMaxOutputBytes
  };

  /// The engine holds the transport reference for its whole life.
  explicit CMRIHost(CMRITransport& transport,
                    const CMRIHostConfig& config = CMRIHostConfig());

  /// Add one remote node during the configuration phase. Returns
  /// *this so calls chain: host.addRemoteNode(30, cfg).addRemoteNode(31,
  /// cfg).begin(). A rejected add records the reason in the host's
  /// config status and short-circuits later adds; begin() reports it.
  ///
  /// `address` is the node address (0..127). The wire UA is
  /// address + 65. A call is rejected when it comes after begin(),
  /// when the node table is full, when the address is out of range
  /// or already added, or when config.inputBytes exceeds
  /// RemoteNodeHandle::kMaxInputBytes.

  // VALIDATION: Interop v1.1 2.3.4: a Host supports UA 0-127 and
  // flags addresses above 64, which the cpNode family cannot use.
  CMRIHost& addRemoteNode(uint8_t address,
                          const RemoteNodeConfig& config,
                          const RemoteNodePolicy& policy);

  /// As above, with the host-wide policy defaults.
  CMRIHost& addRemoteNode(uint8_t address,
                          const RemoteNodeConfig& config);

  /// Convenience: register a node from its address and input/output byte
  /// counts, with host-wide policy defaults and default staleness/enabled.
  /// Equivalent to building a RemoteNodeConfig and calling the overload
  /// above. The flat form keeps simple sketches readable (issue #31).
  CMRIHost& addRemoteNode(uint8_t address,
                          uint16_t inputBytes,
                          uint16_t outputBytes);

  /// Register the optional event listener (nullptr to clear). Part of
  /// the configuration phase: calls after begin() are ignored.
  // VALIDATION: Design v1.1 D5: begin() locks the configuration.
  void onEvent(CMRIHostEventListener listener, void* context = nullptr) {
    if (began_) {
      return;
    }
    eventListener_ = listener;
    eventContext_ = context;
  }

  /// Register the optional TX/RX packet trace listener (nullptr to
  /// clear). Part of the configuration phase: calls after begin() are
  /// ignored.
  void onTrace(CMRIHostTraceListener listener, void* context = nullptr) {
    if (began_) {
      return;
    }
    traceListener_ = listener;
    traceContext_ = context;
  }

  /// Lock the configuration and begin() the transport. Idempotent.
  /// Returns the configuration status: ConfigStatus::kOk when every
  /// addRemoteNode() call was accepted, or the reason the first call
  /// failed. The configuration is locked either way (Design D5).
  ConfigStatus begin();

  /// Advance the engine to `nowMs`. Ticks the transport first, then
  /// runs the poll schedule. `nowMs` must be monotonic.
  void tick(uint32_t nowMs);

#ifdef ARDUINO
  /// Convenience for sketches: tick with the Arduino clock.
  void tick() { tick(millis()); }
#endif

  /// Cumulative host-wide counters.
  const CMRIHostStatistics& statistics() const { return statistics_; }

  /// Nodes added so far.
  size_t nodeCount() const { return nodeCount_; }

  /// Look up a registered node by address. Returns nullptr when no
  /// node was registered at that address. Valid before and after
  /// begin(); the handle stays valid for the life of the program.
  RemoteNodeHandle* node(uint8_t address);

  /// The configuration-phase status so far. ConfigStatus::kOk until an
  /// addRemoteNode() call is rejected. Unchanged after begin() locks.
  ConfigStatus configStatus() const { return configStatus_; }

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

  void drainReceive_(uint32_t nowMs);
  void runSchedule_(uint32_t nowMs);
  bool selectNextNode_(uint32_t nowMs);
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
                  uint8_t replyUa = 0,
                  uint8_t replyMt = 0);
  void emitTrace_(bool transmit, const CMRIPacket& packet);

  CMRITransport& transport_;
  CMRIHostConfig config_;
  CMRIHostStatistics statistics_;

  RemoteNodeHandle nodes_[kMaxNodes];
  RemoteNodePolicy policies_[kMaxNodes];
  size_t nodeCount_ = 0;

  CMRIHostEventListener eventListener_ = nullptr;
  void* eventContext_ = nullptr;
  CMRIHostTraceListener traceListener_ = nullptr;
  void* traceContext_ = nullptr;

  bool began_ = false;
  ConfigStatus configStatus_ = ConfigStatus::kOk;
  Phase phase_ = Phase::kIdle;
  OutboundKind outboundKind_ = OutboundKind::kPoll;
  size_t cursor_ = 0;       ///< round-robin position: next node to consider
  size_t polledIndex_ = 0;  ///< node of the outstanding exchange
  CMRIPacket outbound_;     ///< the packet in flight (I, P, or T)
  Deadline paceGate_;       ///< time gate between exchanges
  Deadline waitGate_;       ///< post-send wait: reply gate / settle / gap
  uint32_t gateArmedMs_ = 0;  ///< when the wait was armed (turnaround base)
};

/// Human-readable name for a CMRIHost::ConfigStatus value.
const char* configStatusString(CMRIHost::ConfigStatus status);

/// Human-readable name for a ReplyRejectReason value.
const char* replyRejectReasonString(ReplyRejectReason reason);

}  // namespace CMRInet
