// RemoteNodeHandle.h — the Host's view of one remote node.
//
// These types are strategy-neutral. They carry a perspective qualifier
// (Remote), never a protocol qualifier. A Host sketch reads other
// nodes through this family, whatever engine runs the exchange.
//
// VALIDATION: Design v1.1 D2: handle, image, freshness, and health
// types live in a strategy-neutral header owned by no engine. The
// handle exposes input reads, input age, link state, and statistics,
// and contains no poll vocabulary.
// VALIDATION: Design v1.1 D1: no RemoteNode type exists. Its absence
// gives every RemoteNode* name exactly one parse.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "CMRITime.h"
#include "ConformanceFault.h"
#include "IOBuffer.h"

// Geometry knob: the per-node input image capacity in data bytes.
//
// The default is 118 — JMRI's exact data-byte ceiling (JMRI caps a
// reply at 120 elements INCLUDING the UA and MT bytes, so 118 data
// bytes). Matching the dominant fielded Host means any geometry this
// Host accepts also works under JMRI: no configuration can work on
// the bench and then silently fail in the field.
//
// Raising the knob:
// - 128 covers the largest Node ever fielded (a full SUSIC backplane:
//   two 16-bay racks, 32 cards, 32-bit input cards in every slot) for
//   beyond-JMRI bench work.
// - 256 is the protocol ceiling (Interop E7); useful only for spec
//   conformance testing.
// Shrink it for memory-limited targets: a CPNODE with all 8x IO16
// cards as inputs plus 16 onboard bits needs 18 bytes.
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time
// knobs.
// VALIDATION: Interop v1.1 2.3.6: default to the JMRI-compatible
// ceiling; larger capacities are bench/conformance options.
#ifndef CMRINET_HOST_MAX_INPUT_BYTES
#define CMRINET_HOST_MAX_INPUT_BYTES 118
#endif

// Geometry knob: the per-node output image capacity in data bytes. Same
// JMRI-compatible default and raise rationale as CMRINET_HOST_MAX_INPUT_BYTES
// above. A CPNODE reports NO output bytes; NI/NO are the wire byte budgets
// the I body carries (interop E3). Shrink for memory-limited targets.
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time knobs.
#ifndef CMRINET_HOST_MAX_OUTPUT_BYTES
#define CMRINET_HOST_MAX_OUTPUT_BYTES 118
#endif

namespace CMRInet {

class CMRIHost;

/// Node health as the Host currently believes it.
// VALIDATION: Design v1.1 D2: RemoteNodeState is strategy-neutral.
enum class RemoteNodeState : uint8_t {
  kUninitialized,  ///< no input image has been received yet
  kOnline,         ///< the node answers and its inputs are fresh
  kStale,          ///< inputs are older than the staleness threshold
  kOffline,        ///< the node stopped answering (polling continues)
  kMisconfigured,  ///< nonconforming with no usable image
  kDegraded,       ///< image exists, but current faults are present
};

/// Telemetry and log spelling of a node health state.
///
/// Lives beside the enum, following configStatusString(),
/// replyRejectReasonString(), and conformanceFaultString(): a
/// human-readable rendering belongs with the vocabulary it renders, so
/// adding an enumerator and forgetting its rendering is one edit to
/// notice rather than three places to hunt.
///
/// Every desktop test TU compiles this header under -Wall -Wextra
/// -Werror, so an unhandled enumerator fails the build here. That is
/// precisely what did not happen when kMisconfigured and kDegraded were
/// added: two sketch-local copies of this switch kept rendering "??" on
/// the same commit, unreported, because arduino-cli passes no warning
/// flags to a sketch (#85, #93).
inline const char* remoteNodeStateString(RemoteNodeState state) {
  switch (state) {
    case RemoteNodeState::kUninitialized: return "UNINITIALIZED";
    case RemoteNodeState::kOnline:        return "ONLINE";
    case RemoteNodeState::kStale:         return "STALE";
    case RemoteNodeState::kOffline:       return "OFFLINE";
    case RemoteNodeState::kMisconfigured: return "MISCONFIGURED";
    case RemoteNodeState::kDegraded:      return "DEGRADED";
  }
  return "UNKNOWN";
}

/// Fixed-width state tag for a character-cell display.
///
/// CONTRACT: exactly three characters, always, space-padded where the
/// abbreviation is shorter. Callers align columns against that width --
/// HostStatusPanel::nodeRowText() lays out "UAxx:TAG <lat> <n>err", and
/// a two- or four-character tag shifts every field after it. A new
/// enumerator must therefore pick a three-character abbreviation, not
/// the shortest string that happens to read well.
///
/// Not derivable from remoteNodeStateString() by truncation:
/// "MISCONFIGURED" and "OFFLINE" would both yield "OFF". Hence two
/// renderings rather than one plus a formatter.
inline const char* remoteNodeStateTag(RemoteNodeState state) {
  switch (state) {
    case RemoteNodeState::kUninitialized: return "---";
    case RemoteNodeState::kOnline:        return "ON ";
    case RemoteNodeState::kStale:         return "OLD";
    case RemoteNodeState::kOffline:       return "OFF";
    case RemoteNodeState::kMisconfigured: return "CFG";  // go check config
    case RemoteNodeState::kDegraded:      return "DEG";
  }
  return "???";
}

/// Stored liveness axis (control substrate).
// VALIDATION: Design v1.3 D16: node health stores liveness separately
// from image validity and conformance.
enum class RemoteNodeLiveness : uint8_t {
  kResponsive,  ///< no current miss run
  kMissing,     ///< misses exist, but not yet beyond the silent threshold
  kSilent,      ///< misses exceeded the silent threshold
};

/// Telemetry spelling of the liveness axis.
inline const char* remoteNodeLivenessString(RemoteNodeLiveness liveness) {
  switch (liveness) {
    case RemoteNodeLiveness::kResponsive: return "RESPONSIVE";
    case RemoteNodeLiveness::kMissing:    return "MISSING";
    case RemoteNodeLiveness::kSilent:     return "SILENT";
  }
  return "UNKNOWN";
}

/// Stored input-image axis (belief substrate).
// VALIDATION: Design v1.2 D16: input validity is an independent stored axis.
// VALIDATION: Design v1.4 D16: it is a validity claim, not a record that
// an image once arrived; kNone covers "never acquired" and
// "invalidated" alike.
enum class RemoteNodeImageState : uint8_t {
  kNone,   ///< no valid image: never acquired, or invalidated
  kFresh,  ///< image is valid and inside staleness threshold
  kStale,  ///< image is valid but older than staleness threshold
};

/// Telemetry spelling of the image-validity axis.
inline const char* remoteNodeImageStateString(RemoteNodeImageState image) {
  switch (image) {
    case RemoteNodeImageState::kNone:  return "NONE";
    case RemoteNodeImageState::kFresh: return "FRESH";
    case RemoteNodeImageState::kStale: return "STALE";
  }
  return "UNKNOWN";
}

/// Stored conformance axis (content-evaluation substrate).
// VALIDATION: Design v1.3 D16: conformance is current evidence, not latched.
enum class RemoteNodeConformance : uint8_t {
  kUnknown,
  kConforming,
  kNonconforming,
};

/// Telemetry spelling of the conformance axis.
inline const char* remoteNodeConformanceString(RemoteNodeConformance c) {
  switch (c) {
    case RemoteNodeConformance::kUnknown:        return "UNKNOWN";
    case RemoteNodeConformance::kConforming:     return "CONFORMING";
    case RemoteNodeConformance::kNonconforming:  return "NONCONFORMING";
  }
  return "UNKNOWN";
}

/// Which scheduling lane a node is served from.
///
/// Derived, never stored, for the same reason RemoteNodeState is: it
/// reads axes that already exist, so a stored copy would be a standing
/// synchronization obligation with no independent content.
// VALIDATION: Design v1.5 D17: two service classes, healthy and
// degraded, with different guardrails. The degraded lane is rate
// limited by two gates; the healthy lane consults neither.
enum class RemoteNodeServiceClass : uint8_t {
  kHealthy,   ///< served at full rate, ungated
  kDegraded,  ///< served through the rate-limited lane
};

/// Telemetry spelling of the service class.
inline const char* remoteNodeServiceClassString(RemoteNodeServiceClass c) {
  switch (c) {
    case RemoteNodeServiceClass::kHealthy:  return "HEALTHY";
    case RemoteNodeServiceClass::kDegraded: return "DEGRADED";
  }
  return "UNKNOWN";
}

/// Conformance breaker position (D17).
///
/// Control substrate: it gates scheduling, so it is engine-written and
/// never derived from observation (D15).
// VALIDATION: Design v1.5 D17: the breaker's states are closed,
// re-initializing, and open. A run of nonconforming replies past a
// threshold arms bounded corrective re-inits; exhausting them opens it.
enum class ConformanceBreakerState : uint8_t {
  kClosed,          ///< no conformance fault run in progress
  kReinitializing,  ///< corrective re-init attempts in progress, bounded
  kOpen,            ///< tripped: bare-P probes only, at a low rate
};

/// Telemetry spelling of the breaker position.
inline const char* conformanceBreakerStateString(ConformanceBreakerState s) {
  switch (s) {
    case ConformanceBreakerState::kClosed:         return "CLOSED";
    case ConformanceBreakerState::kReinitializing: return "REINITIALIZING";
    case ConformanceBreakerState::kOpen:           return "OPEN";
  }
  return "UNKNOWN";
}

/// Per-node configuration, independent of exchange discipline.
struct RemoteNodeConfig {
  /// Input image size in logical bytes. This is the exact reply body
  /// length the node reports. addRemoteNode() rejects values above
  /// RemoteNodeHandle::kMaxInputBytes.
  uint16_t inputBytes = 0;

  /// Output image size in logical bytes. This is the exact T body length
  /// the engine transmits. addRemoteNode() rejects values above
  /// RemoteNodeHandle::kMaxOutputBytes. On the wire this is the CPNODE
  /// I body's NO field (interop E3).
  uint16_t outputBytes = 0;

  /// Staleness threshold. Input data older than this makes the node
  /// report kStale. 0 disables the check.
  // VALIDATION: Design v1.1 D2: the staleness threshold is universal
  // and lives in the handle contract. The reply-gate timeout is
  // strategy policy and lives elsewhere.
  uint32_t stalenessMs = 0;

  /// Initial enabled state.
  bool enabled = true;
};

/// Per-node statistics. All counters start at 0, increase monotonically,
/// and are never reset.
///
/// Monotonicity is a property of a counter for a given subject, and
/// delete ends that subject: the next occupant of a reused slot is a
/// different logical device and counts from zero. Nothing is reset
/// mid-life, so the guarantee above is intact (Design v1.2 D5).
struct RemoteNodeStatistics {
  uint32_t exchanges = 0;         ///< exchanges that committed fresh inputs
  uint32_t noReplies = 0;         ///< exchanges that ended with no reply
  uint32_t recoveries = 0;        ///< exchanges that ended a no-reply run
  uint32_t errors = 0;            ///< malformed replies from this node
  uint32_t lastTurnaroundMs = 0;  ///< send-complete to reply-accepted, last exchange
};

/// The last conformance fault attributed to a node: what it was, what
/// the Host expected, what the Node demonstrated, and when.
///
/// Observation substrate (D15): reporting only, never read back to gate
/// behavior. Deliberately coarse -- one slot, overwritten each time. The
/// event stream carries every fault as it happens, so an analyzer
/// aggregates from there; this exists so a handle read long afterwards
/// can still say what went wrong without having subscribed.
///
/// There is no companion fault *counter*. Only image-layer faults are
/// attributable to a node, so one would duplicate
/// RemoteNodeStatistics::errors exactly, and the packet-layer total is
/// already derivable at the scope it belongs to: host repliesRejected
/// minus the sum of per-node errors.
///
/// Layer and attribution are not stored. layerOf() and attributionOf()
/// derive them from `fault`, which is what keeps an invalid
/// (layer, attribution) pair unconstructible (D14).
struct ConformanceFaultRecord {
  ConformanceFault fault = ConformanceFault::kNone;

  /// The Host's assumption at the time. For an image-layer geometry
  /// fault, the declared input byte count.
  uint16_t expected = 0;

  /// What the Node actually demonstrated. For an image-layer geometry
  /// fault, the reply body length.
  uint16_t observed = 0;

  /// Engine clock when the fault was observed. Meaningful only when
  /// fault != kNone.
  uint32_t atMs = 0;
};

/// The per-node handle a Host sketch holds. It exposes input image
/// reads, freshness, health, and statistics.
///
/// Obtain one from CMRIHost::node(addr) at the point of use. It is valid
/// until that node is deleted; the storage outlives the node, but the
/// slot may be reused by a different logical device, so a handle cached
/// across a mutation can silently describe somebody else. UA() is
/// the self-check for code that caches one anyway -- a deleted node
/// reads back as UA 0.
// VALIDATION: Design v1.2 D5: host.node(addr) is the canonical access
// path; caching a handle across a mutation is not a supported pattern.
///
/// The sketch reads the handle. setEnabled() is the only sketch-side
/// write. The engine that created the handle is the sole writer of
/// image, freshness, state, and statistics.
class RemoteNodeHandle {
 public:
  static constexpr size_t kMaxInputBytes = CMRINET_HOST_MAX_INPUT_BYTES;
  static constexpr size_t kMaxOutputBytes = CMRINET_HOST_MAX_OUTPUT_BYTES;

  /// observedInputBytes() before any reply has demonstrated a geometry.
  /// An explicit named value rather than 0, because 0 is a legal
  /// geometry -- the same reasoning that gives Age its kNeverMarked.
  static constexpr uint16_t kGeometryNeverObserved = 0xFFFFu;

  RemoteNodeHandle() = default;

  /// The node UA (0..127) as configured.
  uint8_t UA() const { return UA_; }

  /// The unit UA byte as transmitted (UA + 65).
  uint8_t wireUA() const { return wireUA_; }

  /// Last good input bit. Bit 0 is the least significant bit of the
  /// named byte. Out-of-range indexes read false.
  bool inputBit(size_t byte, size_t bit) const {
    return input_.getBit(byte, bit);
  }

  /// Last good input byte. Out-of-range indexes read 0.
  uint8_t inputByte(size_t index) const {
    return input_.byte(index);
  }

  /// The last good input image and its configured length.
  const uint8_t* inputs() const { return input_.data(); }
  size_t inputLength() const { return config_.inputBytes; }

  /// Last output bit written by the sketch. Bit 0 is the LSB of the
  /// named byte. Out-of-range indexes read false.
  bool outputBit(size_t byte, size_t bit) const {
    return output_.getBit(byte, bit);
  }

  /// Last output byte written. Out-of-range indexes read 0.
  uint8_t outputByte(size_t index) const {
    return output_.byte(index);
  }

  /// The configured output image length in bytes.
  size_t outputLength() const { return config_.outputBytes; }

  /// Set one output bit. Bit 0 is the LSB of the named
  /// byte. Out-of-range bits are ignored. Marks the output
  /// image dirty so the engine sends a full T on this node's next slot.
  // VALIDATION: Interop v1.1 2.3.2: the Host sends the full output image
  // in every T frame; a dirty bit triggers a full-image transmit.
  void setOutputBit(size_t byte, size_t bit, bool v) {
    output_.setBit(byte, bit, v);
    outputsDirty_ = true;
  }

  /// Copy `len` bytes into the output image. Returns false (and leaves the
  /// image unchanged) if len > outputBytes or data is null with a nonzero
  /// length. Marks the image dirty on success.
  bool setOutputs(const uint8_t* data, size_t len) {
    const bool ok = output_.setData(data, len);
    if (ok) {
      outputsDirty_ = true;
    }
    return ok;
  }

  /// Force the engine to re-send a full T on this node's next slot, even
  /// when no output bit changed (e.g. after a suspected node reset).
  void forceTransmit() { outputsDirty_ = true; }

  /// Age of the input image at `nowMs` (the caller's injected clock).
  /// Returns Age::kNeverMarked before the first good reply.
  // VALIDATION: Interop v1.1 2.3.12: per-node freshness is API state.
  // A Host must not hold stale inputs silently.
  uint32_t inputAgeMs(uint32_t nowMs) const { return freshness_.ms(nowMs); }

  /// Node health as computed at the engine's most recent tick(nowMs).
  RemoteNodeState state() const { return deriveState_(); }

  /// Stored health axes.
  RemoteNodeLiveness liveness() const { return liveness_; }
  RemoteNodeImageState imageState() const { return imageState_; }
  RemoteNodeConformance conformance() const { return conformance_; }

  /// Current miss run length (control state).
  uint32_t consecutiveMisses() const { return consecutiveMisses_; }

  /// Current nonconforming-reply run length (control state).
  ///
  /// The conformance ladder's counterpart to consecutiveMisses_. Two
  /// ladders, one per failure mode: this one counts replies that
  /// arrived with the wrong shape, that one counts replies that never
  /// arrived. Both are bounded, and both end in an invalidation.
  uint32_t consecutiveNonconforming() const {
    return consecutiveNonconforming_;
  }

  /// Conformance breaker position (D17).
  ConformanceBreakerState breakerState() const { return breakerState_; }

  /// True while the breaker is tripped. Read by isHealthy().
  bool conformanceBreakerOpen() const { return conformanceBreakerOpen_; }

  /// Corrective re-init attempts spent in the current fault run.
  uint32_t breakerReinitAttempts() const { return breakerReinitAttempts_; }

  /// Which scheduling lane this node is served from.
  ///
  /// Degraded when a miss run is live or the conformance verdict is
  /// nonconforming. `kUnknown` conformance is deliberately not degraded:
  /// a newly added node has demonstrated no cost yet and must get a
  /// full-rate first poll, or the table's newest member is the one that
  /// starves.
  // VALIDATION: Design v1.5 D17: service class is derived from axes
  // that already exist, never stored.
  RemoteNodeServiceClass serviceClass() const {
    const bool degraded =
        consecutiveMisses_ != 0 ||
        conformance_ == RemoteNodeConformance::kNonconforming;
    return degraded ? RemoteNodeServiceClass::kDegraded
                    : RemoteNodeServiceClass::kHealthy;
  }

  /// Operator predicate: true only when health is fully green.
  // VALIDATION: Design v1.3 D16: operator and application predicates are
  // separate; this predicate is stricter than input usability.
  bool isHealthy() const {
    return liveness_ == RemoteNodeLiveness::kResponsive &&
           imageState_ == RemoteNodeImageState::kFresh &&
           conformance_ == RemoteNodeConformance::kConforming &&
           !conformanceBreakerOpen_;
  }

  /// Application predicate: true when this image can be acted on.
  // VALIDATION: Design v1.3 D16: application usability is a separate
  // question from operator-facing health.
  bool inputsUsable() const {
    return imageState_ == RemoteNodeImageState::kFresh &&
           liveness_ != RemoteNodeLiveness::kSilent &&
           conformance_ != RemoteNodeConformance::kNonconforming;
  }

  /// The input geometry this node has most recently *demonstrated*: the
  /// body length of the last reply carrying this node's UA and an R
  /// message type, whatever that length was (D14 L1).
  ///
  /// Evidence, as against config_.inputBytes, which is a claim. Declared
  /// geometry flows outward in the I body and nothing returns, so this
  /// is the only channel by which a Host learns it was wrong -- and the
  /// only one that covers the fielded population, since a Node that
  /// ignores I never sends the I-ack that L2 would need.
  ///
  /// Reads kGeometryNeverObserved before the first such reply.
  ///
  /// Input only. Output geometry is not observable from a reply and
  /// stays unfalsifiable until L2 (D14). The asymmetry is real; the
  /// accessor name states it rather than implying a symmetry.
  // VALIDATION: Design v1.3 D14: reply length reveals actual NI (L1);
  // input geometry is observable and output geometry is not.
  uint16_t observedInputBytes() const { return observedInputBytes_; }

  /// The last conformance fault attributed to this node. `fault` reads
  /// kNone when there has never been one.
  const ConformanceFaultRecord& lastConformanceFault() const {
    return lastFault_;
  }

  /// Cumulative exchange statistics.
  const RemoteNodeStatistics& statistics() const { return statistics_; }

  bool enabled() const { return config_.enabled; }

  /// Disable or re-enable this node. The exchange schedule skips a
  /// disabled node, but the node keeps its slot, its image, and its
  /// counters -- this is "out of service", not "gone". To remove a node
  /// outright, use CMRIHost::deleteRemoteNode().
  // VALIDATION: Design v1.2 D5: enable/disable is unchanged from v1.1
  // and is a separate axis from membership.
  void setEnabled(bool enabled) { config_.enabled = enabled; }

 private:
  friend class CMRIHost;

  uint8_t UA_ = 0;
  uint8_t wireUA_ = 0;
  RemoteNodeConfig config_;

  // Output image: sketch-written via the setters above, engine-read when
  // it builds a full T. outputsDirty_ tells the engine a T is owed.
  IOBuffer output_;
  bool outputsDirty_ = false;

  // Engine-only state below. The engine is the sole writer of these.
  RemoteNodeStatistics statistics_;
  RemoteNodeLiveness liveness_ = RemoteNodeLiveness::kResponsive;
  RemoteNodeImageState imageState_ = RemoteNodeImageState::kNone;
  RemoteNodeConformance conformance_ = RemoteNodeConformance::kUnknown;
  bool conformanceBreakerOpen_ = false;
  uint32_t consecutiveMisses_ = 0;

  // Conformance breaker (D17). The liveness ladder above counts replies
  // that never arrived; this one counts replies that arrived with the
  // wrong shape. Both are control state: they gate scheduling, so D15
  // keeps them out of the observation substrate.
  //
  // breakerState_ and conformanceBreakerOpen_ are deliberately not one
  // field. The bool is the predicate isHealthy() already reads and is
  // part of the published handle contract; the enum is the scheduler's
  // three-position machine. Collapsing them would either widen the
  // predicate's vocabulary or lose the re-initializing position.
  ConformanceBreakerState breakerState_ = ConformanceBreakerState::kClosed;
  uint32_t consecutiveNonconforming_ = 0;
  uint32_t breakerReinitAttempts_ = 0;

  // Probe pacing while the breaker is open. Bare P only -- a re-init
  // sequence here would stall the round-robin on its post-I settle.
  Deadline breakerProbe_;

  /// Geometry this node has demonstrated, and the last fault charged to
  /// it. Both are observation substrate.
  uint16_t observedInputBytes_ = kGeometryNeverObserved;
  ConformanceFaultRecord lastFault_;

  Age freshness_;
  IOBuffer input_;
  bool needsInit_ = true;       ///< engine owes this node an I (JMRI mustInit)
  bool reinitArmed_ = false;    ///< re-init ladder fired this miss-run
  uint32_t lastTxMs_ = 0;       ///< last T send time (refresh timer base)

  // Anti-starvation (map issue #41): outputsDirty_ may preempt a poll with
  // a transmit, but never indefinitely. pollDueBy_ is (re)armed for
  // maxOutputPreemptMs every time a real poll is sent; once due, the
  // scheduler forces the next poll through regardless of outputsDirty_.
  Deadline pollDueBy_;

  // Poll-retry backoff for a node with consecutive misses, so a
  // chronically offline node cannot tax the round-robin's baseline cycle
  // time -- pollDueBy_ above depends on that cycle time staying well
  // under maxOutputPreemptMs. Doubles per consecutive miss up to
  // maxPollBackoffMs; any accepted reply clears it immediately.
  Deadline pollBackoff_;
  uint32_t pollBackoffMs_ = 0;   ///< current backoff duration (0 = none yet)

  // Ceiling clamp base (D17): when this node was last granted an
  // exchange slot. A degraded node whose last grant is older than
  // maxPollBackoffMs is admitted with both gates bypassed, which is
  // what keeps the degraded class from being starved to zero.
  //
  // Age, not Deadline, because the question is "how long since" rather
  // than "is it time yet". kNeverMarked reads as past every threshold,
  // which is harmless here: a node that has never been granted a slot
  // has neither a miss run nor a conformance verdict, so it is healthy
  // class and never consults the clamp at all.
  Age lastGrant_;

  /// Is the cached input image currently valid?
  ///
  /// Derived, not stored. Freshness is marked when a reply commits, and
  /// cleared by re-init invalidation (interop 2.3.10) and by a geometry
  /// change, so it already tracks validity exactly. A second field
  /// tracking the same thing would be a standing synchronization
  /// obligation with no independent content.
  ///
  /// This is a *validity* question, not a history one, and the
  /// distinction is a D15 substrate boundary rather than a nicety. "Has
  /// this node ever worked" is statistics_.exchanges > 0 -- observation,
  /// where D15 puts history. Belief holds only the current verdict. The
  /// field this replaced stored history in belief, which is what made
  /// D16's chronology unimplementable.
  // VALIDATION: Design v1.4 D15: belief holds the current verdict;
  // history lives in observation.
  bool hasValidImage_() const { return freshness_.marked(); }

  // VALIDATION: Design v1.3 D16: RemoteNodeState is a derived
  // projection over stored axes. This path maps the D16 extensions
  // MISCONFIGURED and DEGRADED.
  RemoteNodeState deriveState_() const {
    if (liveness_ == RemoteNodeLiveness::kSilent) {
      return RemoteNodeState::kOffline;
    }
    if (conformance_ == RemoteNodeConformance::kNonconforming) {
      // D16's chronology, three ways rather than two.
      //
      // A nonconforming node holding a fresh image is DEGRADED: it is
      // faulting while its data remains actionable. One whose image has
      // merely aged out is STALE -- rejected replies stop refreshing
      // freshness, and "your data is old" is the honest report while the
      // last good image is still valid. Only an *invalid* image, never
      // acquired or cleared by re-init invalidation, makes MISCONFIGURED
      // the right answer.
      //
      // Folding the middle case into MISCONFIGURED made
      // STALE-while-nonconforming unreachable and inverted D16's stated
      // order. It survived only because conformance was inert, so this
      // branch had never executed.
      // VALIDATION: Design v1.4 D16: the projection reads the image axis
      // three ways under a nonconforming verdict -- fresh gives
      // DEGRADED, stale gives STALE, none gives MISCONFIGURED.
      switch (imageState_) {
        case RemoteNodeImageState::kFresh:
          return RemoteNodeState::kDegraded;
        case RemoteNodeImageState::kStale:
          return RemoteNodeState::kStale;
        case RemoteNodeImageState::kNone:
          return RemoteNodeState::kMisconfigured;
      }
    }
    if (imageState_ == RemoteNodeImageState::kNone) {
      return RemoteNodeState::kUninitialized;
    }
    if (imageState_ == RemoteNodeImageState::kStale) {
      return RemoteNodeState::kStale;
    }
    return RemoteNodeState::kOnline;
  }
};

}  // namespace CMRInet
