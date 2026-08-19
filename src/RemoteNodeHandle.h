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
};

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

/// Per-node statistics. All counters start at 0 at begin(), increase
/// monotonically, and never reset.
struct RemoteNodeStatistics {
  uint32_t exchanges = 0;         ///< exchanges that committed fresh inputs
  uint32_t noReplies = 0;         ///< exchanges that ended with no reply
  uint32_t recoveries = 0;        ///< exchanges that ended a no-reply run
  uint32_t errors = 0;            ///< malformed replies from this node
  uint32_t lastTurnaroundMs = 0;  ///< send-complete to reply-accepted, last exchange
  uint32_t consecutiveMisses = 0; ///< current unbroken no-reply run
};

/// The per-node handle a Host sketch holds. It exposes input image
/// reads, freshness, health, and statistics. The engine returns it at
/// configuration time and it stays valid for the life of the program.
///
/// The sketch reads the handle. setEnabled() is the only sketch-side
/// write. The engine that created the handle is the sole writer of
/// image, freshness, state, and statistics.
class RemoteNodeHandle {
 public:
  static constexpr size_t kMaxInputBytes = CMRINET_HOST_MAX_INPUT_BYTES;
  static constexpr size_t kMaxOutputBytes = CMRINET_HOST_MAX_OUTPUT_BYTES;

  RemoteNodeHandle() = default;

  /// The node address (0..127) as configured.
  uint8_t address() const { return address_; }

  /// The unit address byte as transmitted (address + 65).
  uint8_t ua() const { return ua_; }

  /// Last good input bit. Bit 0 is the least significant bit of input
  /// byte 0. Out-of-range bits read false.
  bool inputBit(size_t bit) const {
    const size_t byteIndex = bit / 8u;
    if (byteIndex >= config_.inputBytes) {
      return false;
    }
    return (inputs_[byteIndex] >> (bit % 8u)) & 0x01u;
  }

  /// Last good input byte. Out-of-range indexes read 0.
  uint8_t inputByte(size_t index) const {
    return (index < config_.inputBytes) ? inputs_[index] : 0u;
  }

  /// The last good input image and its configured length.
  const uint8_t* inputs() const { return inputs_; }
  size_t inputLength() const { return config_.inputBytes; }

  /// Last output bit written by the sketch. Bit 0 is the LSB of output
  /// byte 0. Out-of-range bits read false.
  bool outputBit(size_t bit) const {
    const size_t byteIndex = bit / 8u;
    if (byteIndex >= config_.outputBytes) {
      return false;
    }
    return (outputs_[byteIndex] >> (bit % 8u)) & 0x01u;
  }

  /// Last output byte written. Out-of-range indexes read 0.
  uint8_t outputByte(size_t index) const {
    return (index < config_.outputBytes) ? outputs_[index] : 0u;
  }

  /// The configured output image length in bytes.
  size_t outputLength() const { return config_.outputBytes; }

  /// Set one output bit. Out-of-range bits are ignored. Marks the output
  /// image dirty so the engine sends a full T on this node's next slot.
  // VALIDATION: Interop v1.1 2.3.2: the Host sends the full output image
  // in every T frame; a dirty bit triggers a full-image transmit.
  void setOutputBit(size_t bit, bool v) {
    const size_t byteIndex = bit / 8u;
    if (byteIndex >= config_.outputBytes) {
      return;
    }
    if (v) {
      outputs_[byteIndex] |= static_cast<uint8_t>(1u << (bit % 8u));
    } else {
      outputs_[byteIndex] &= static_cast<uint8_t>(~(1u << (bit % 8u)));
    }
    outputsDirty_ = true;
  }

  /// Copy `len` bytes into the output image. Returns false (and leaves the
  /// image unchanged) if len > outputBytes or data is null with a nonzero
  /// length. Marks the image dirty on success.
  bool setOutputs(const uint8_t* data, size_t len) {
    if (len > config_.outputBytes) {
      return false;
    }
    if (data == nullptr && len != 0) {
      return false;
    }
    if (len != 0) {
      memcpy(outputs_, data, len);
    }
    outputsDirty_ = true;
    return true;
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
  RemoteNodeState state() const { return state_; }

  /// Cumulative exchange statistics.
  const RemoteNodeStatistics& statistics() const { return statistics_; }

  bool enabled() const { return config_.enabled; }

  /// Disable or re-enable this node. The exchange schedule skips a
  /// disabled node.
  // VALIDATION: Design v1.1 D5: after begin(), nodes cannot be
  // removed, only disabled.
  void setEnabled(bool enabled) { config_.enabled = enabled; }

 private:
  friend class CMRIHost;

  uint8_t address_ = 0;
  uint8_t ua_ = 0;
  RemoteNodeConfig config_;

  // Output image: sketch-written via the setters above, engine-read when
  // it builds a full T. outputsDirty_ tells the engine a T is owed.
  uint8_t outputs_[kMaxOutputBytes] = {0};
  bool outputsDirty_ = false;

  // Engine-only state below. The engine is the sole writer of these.
  RemoteNodeStatistics statistics_;
  RemoteNodeState state_ = RemoteNodeState::kUninitialized;
  Age freshness_;
  uint8_t inputs_[kMaxInputBytes] = {0};
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
};

}  // namespace CMRInet
