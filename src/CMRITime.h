// CMRITime.h — injected-time helpers: the one place elapsed-time math
// lives.
//
// Adapted from the elapsedMillis idea (PJRC.COM, LLC, MIT license,
// http://www.pjrc.com/teensy/): tiny value types wrapping wrap-safe
// unsigned clock arithmetic so the correctness lives in one header, not
// distributed through every engine and transport. Rewritten for this
// library's injected-time discipline: nothing here calls millis() — the
// caller's tick(nowMs) clock is passed to every query, so the same types
// run on hardware and under a deterministic mock clock in desktop tests.
//
// VALIDATION: Design v1.0 D6: the library advances only from injected
// tick(nowMs) time. Injected time keeps desktop tests deterministic.
//
// Explicit state instead of zero sentinels: 0 is a legitimate uint32_t
// clock value (rollover lands on it), and a sketch's first tick(millis())
// may arrive at any large value after boot. "Never armed" / "never
// marked" is explicit state, never a magic time.
//
// Range limits and the fail-safe policy (uint32_t millisecond clock):
// - The measurable window is kCapacityMs, just under 2^31 ms (~24.8
//   days). Both types detect the boundary instead of silently aliasing,
//   provided they are observed (due()/poll()) at least once per window —
//   guaranteed by any tick-driven caller.
// - Deadline latches sticky-due: once observed due it stays due until
//   disarmed, so a missed window fires a timeout late rather than never.
// - Age saturates: past capacity it reports kExceededCapacity, and when
//   nothing was ever marked it reports kNeverMarked. Both sit above
//   every real threshold, so atLeast() stays true forever and stale
//   data can never wrap back to looking fresh.
// Anything measuring genuine multi-week spans needs a wider clock, not
// these types.

#pragma once

#include <stdint.h>

namespace CMRInet {

/// Wrap-safe "has nowMs reached thenMs". Correct across uint32_t
/// rollover while the true distance between the clock values is less
/// than 2^31 ms (~24.8 days).
constexpr bool timeReached(uint32_t nowMs, uint32_t thenMs) {
  return static_cast<int32_t>(nowMs - thenMs) >= 0;
}

/// One-shot deadline with explicit armed state and sticky trigger.
///
/// Disarmed by default. Arm with a due time, poll due(nowMs) from the
/// owner's tick path, disarm when handled. Once due has been observed it
/// latches: the deadline stays due until disarmed or re-armed, even if
/// later clock values would (after wrap) compare as "not yet". The
/// failure direction is a late timeout, never a lost one.
class Deadline {
 public:
  Deadline() = default;

  /// Arm to fire at the absolute clock value `dueMs`.
  void armAt(uint32_t dueMs) {
    dueMs_ = dueMs;
    armed_ = true;
    reached_ = false;
  }

  /// Arm to fire `delayMs` after `nowMs`.
  void armIn(uint32_t nowMs, uint32_t delayMs) { armAt(nowMs + delayMs); }

  void disarm() {
    armed_ = false;
    reached_ = false;
  }

  bool armed() const { return armed_; }

  /// The armed due time. Meaningful only while armed().
  uint32_t dueMs() const { return dueMs_; }

  /// True once armed and the clock has reached the due time; sticky
  /// until disarm()/armAt(). Poll at least once per capacity window
  /// (~24.8 days) for the latch to be reliable.
  bool due(uint32_t nowMs) {
    if (armed_ && !reached_ && timeReached(nowMs, dueMs_)) {
      reached_ = true;
    }
    return armed_ && reached_;
  }

 private:
  uint32_t dueMs_ = 0;
  bool armed_ = false;
  bool reached_ = false;  ///< sticky: due was observed
};

/// Age of a marked event ("how long since the last good input image"),
/// with saturating capacity and an explicit never-marked answer.
///
/// mark(nowMs) latches the event. ms(nowMs) reports its age. Ages are
/// quantifiable up to kCapacityMs (~24.8 days). Outside that, ms()
/// answers with a named sentinel, never an aliased small number:
/// - kNeverMarked: no event was ever marked, or clear() removed it.
/// - kExceededCapacity: an event was marked, but longer ago than the
///   measurable window.
/// Both sentinels sit above every real threshold. The staleness check
/// atLeast() is therefore true for both: no data and ancient data are
/// both stale, and neither wraps back to fresh.
///
/// Call poll(nowMs) from the owner's tick path at least once per
/// capacity window so saturation latches across clock wrap.
class Age {
 public:
  /// ms() result: no event was ever marked.
  static constexpr uint32_t kNeverMarked = 0xFFFFFFFFu;

  /// ms() result: marked, but older than the measurable window.
  static constexpr uint32_t kExceededCapacity = 0xFFFFFFFEu;

  /// The measurable window: just under 2^31 ms (~24.8 days).
  static constexpr uint32_t kCapacityMs = 0x7FFFFFFFu;

  Age() = default;

  /// Latch `nowMs` as the event time; clears any saturation.
  void mark(uint32_t nowMs) {
    markMs_ = nowMs;
    marked_ = true;
    exceeded_ = false;
  }

  /// Forget the mark: ms() returns kNeverMarked until the next mark().
  /// Use this when a re-initialization invalidates cached inputs.
  void clear() {
    marked_ = false;
    exceeded_ = false;
  }

  bool marked() const { return marked_; }

  /// True once a marked age has outlived the measurable window.
  bool exceeded() const { return exceeded_; }

  /// Drive the saturation latch; call from tick paths.
  void poll(uint32_t nowMs) {
    if (marked_ && !exceeded_ && (nowMs - markMs_) > kCapacityMs) {
      exceeded_ = true;
    }
  }

  /// Age in ms at `nowMs`; kNeverMarked or kExceededCapacity outside the
  /// measurable window. Self-detects within the first window even
  /// without poll(); poll() makes the detection sticky across wrap.
  uint32_t ms(uint32_t nowMs) const {
    if (!marked_) {
      return kNeverMarked;
    }
    if (exceeded_) {
      return kExceededCapacity;
    }
    const uint32_t raw = nowMs - markMs_;
    return (raw > kCapacityMs) ? kExceededCapacity : raw;
  }

  /// The staleness check: has at least `thresholdMs` passed since the
  /// mark? True forever when never marked or capacity is exceeded.
  bool atLeast(uint32_t nowMs, uint32_t thresholdMs) const {
    return ms(nowMs) >= thresholdMs;
  }

 private:
  uint32_t markMs_ = 0;
  bool marked_ = false;
  bool exceeded_ = false;  ///< sticky: capacity was observed exceeded
};

}  // namespace CMRInet
