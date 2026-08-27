// SimpleHostMetrics.h — pure, platform-neutral helpers the SimpleHost
// example's OLED status panel uses: a rolling count of events inside a
// time window (for "recent errors"), a rolling count-per-second rate
// (for polling cadence), and a fixed-width latency formatter. None of
// this depends on the Arduino runtime or the display library; it is
// shared between the example sketch and the desktop test suite so the
// arithmetic is verifiable under a deterministic mock clock.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace CMRInet {
namespace examples {

/// A rolling window of events, implemented as a small ring buffer of
/// timestamps. Big enough to comfortably cover a noisy bench without
/// ever allocating.
class ErrorWindow {
 public:
  static constexpr size_t kCapacity = 64;

  void reset() { head_ = 0; count_ = 0; }

  /// Record an event at time `nowMs`.
  void onEvent(uint32_t nowMs) {
    slots_[(head_ + count_) % kCapacity] = nowMs;
    if (count_ == kCapacity) {
      head_ = (head_ + 1) % kCapacity;  // oldest evicted
    } else {
      ++count_;
    }
  }

  /// Number of recorded events at times within `[nowMs - windowMs, nowMs]`.
  uint32_t countInLastMs(uint32_t nowMs, uint32_t windowMs) const {
    uint32_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
      const uint32_t t = slots_[(head_ + i) % kCapacity];
      if (nowMs >= t && (nowMs - t) <= windowMs) {
        ++n;
      }
    }
    return n;
  }

 private:
  uint32_t slots_[kCapacity] = {};
  size_t head_ = 0;
  size_t count_ = 0;
};

/// A rolling count-per-second computed from *cumulative* counter samples,
/// not per-event timestamps. The panel is sampled at display redraw
/// cadence (150 ms), not at each poll, so recording one timestamp per
/// sample would undercount polls by the redraw-to-poll ratio. Instead,
/// each sample records (nowMs, cumulativePolls); the rate is the delta
/// of the cumulative count over the delta of time across the window.
/// Correct at any sampling cadence, and handles bursts (many polls
/// between samples) without losing them.
class PollRate {
 public:
  static constexpr size_t kCapacity = 128;  // 128 samples * 150 ms ≈ 19 s
  static constexpr uint32_t kWindowMs = 10000;

  void reset(uint32_t /*nowMs*/) { head_ = 0; count_ = 0; }

  /// Record a cumulative-counter sample at `nowMs`. `cumulativePolls` is
  /// CMRIHost::statistics().pollsSent at this instant. Multiple polls
  /// that landed between samples are captured by the count delta, not
  /// lost.
  void sample(uint32_t nowMs, uint32_t cumulativePolls) {
    slots_[(head_ + count_) % kCapacity].ms = nowMs;
    slots_[(head_ + count_) % kCapacity].polls = cumulativePolls;
    if (count_ == kCapacity) {
      head_ = (head_ + 1) % kCapacity;
    } else {
      ++count_;
    }
  }

  /// Polls per second, evaluated at `nowMs` over the window. Finds the
  /// oldest sample within `(nowMs - kWindowMs, nowMs]` and computes
  /// (polls_now - polls_then) / ((nowMs - then) / 1000). Returns 0 when
  /// fewer than two samples exist or the window has no span.
  float cyclesPerSecondAt(uint32_t nowMs) const {
    const Sample* oldest = nullptr;
    const Sample* newest = nullptr;
    for (size_t i = 0; i < count_; ++i) {
      const Sample& s = slots_[(head_ + i) % kCapacity];
      if (nowMs >= s.ms && (nowMs - s.ms) < kWindowMs) {
        if (oldest == nullptr) oldest = &s;
        newest = &s;
      }
    }
    if (oldest == nullptr || newest == nullptr) return 0.0f;
    const uint32_t dtMs = newest->ms - oldest->ms;
    if (dtMs == 0) return 0.0f;
    const uint32_t dp = newest->polls - oldest->polls;
    return static_cast<float>(dp) * 1000.0f / static_cast<float>(dtMs);
  }

  /// Average polling interval in ms over the window: the inverse of the
  /// rate, computed from the same sample span. Returns 0 when stalled.
  uint32_t intervalMsAt(uint32_t nowMs) const {
    const float cps = cyclesPerSecondAt(nowMs);
    if (cps <= 0.0f) return 0u;
    return static_cast<uint32_t>(1000.0f / cps + 0.5f);
  }

 private:
  struct Sample {
    uint32_t ms = 0;
    uint32_t polls = 0;
  };

  Sample slots_[kCapacity] = {};
  size_t head_ = 0;
  size_t count_ = 0;
};

/// Format a millisecond latency as fixed-width, right-justified text in
/// the form "<N>ms", N right-justified in a 4-char numeric field. Total
/// output width is 6 chars ("   6ms", " 273ms", "1200ms", "9999+ms"
/// clamps). `buf` must hold at least 7 bytes (6 + NUL).
inline void latencyText(char* buf, size_t len, uint32_t ms) {
  if (len < 7) {
    if (len > 0) buf[0] = '\0';
    return;
  }
  if (ms > 9999u) {
    snprintf(buf, len, "9999+ms");
    return;
  }
  snprintf(buf, len, "%4lums", static_cast<unsigned long>(ms));
}

/// The shared Host status panel logic: owns the rolling metric state
/// (polling rate, per-node error windows) and formats the header and
/// per-node row strings a sketch renders to its display. Pure — no
/// display or Arduino dependency — so the arithmetic and alternation
/// timing are verifiable under a deterministic mock clock. The sketch
/// calls sample() on each redraw with the current host/node counters,
/// then reads headerText()/nodeRowText() and feeds the strings to its
/// display at the cursor positions it chooses.
class HostStatusPanel {
 public:
  static constexpr size_t kMaxNodes = 16;
  static constexpr uint32_t kErrorWindowMs = 5000;
  static constexpr uint32_t kAltPeriodMs = 5000;  // c/s ↔ ms/cycle every 5 s

  void reset() {
    pollRate_.reset(0);
    for (size_t i = 0; i < kMaxNodes; ++i) nodeErrors_[i].reset();
    for (size_t i = 0; i < kMaxNodes; ++i) prevNodeErrors_[i] = 0;
  }

  /// Feed the panel the current cumulative counters on each redraw.
  /// `pollsSent` is CMRIHost::statistics().pollsSent; `nodeErrorCounts`
  /// is an array of per-node statistics().errors values, one per node
  /// up to `nodeCount`. The panel detects deltas and records timestamps.
  void sample(uint32_t nowMs, uint32_t pollsSent,
              const uint32_t* nodeErrorCounts, size_t nodeCount) {
    pollRate_.sample(nowMs, pollsSent);
    for (size_t i = 0; i < nodeCount && i < kMaxNodes; ++i) {
      if (nodeErrorCounts == nullptr) continue;
      while (nodeErrorCounts[i] != prevNodeErrors_[i]) {
        nodeErrors_[i].onEvent(nowMs);
        ++prevNodeErrors_[i];
      }
    }
  }

  /// Format the header line: alternates between "XX.Xc/s" (cycles per
  /// second) and "XXXms" (ms per cycle) every kAltPeriodMs, so both
  /// views of the same underlying rate are visible without crowding.
  /// A stalled engine (no polls in the window) shows "---ms".
  void headerText(char* buf, size_t len, uint32_t nowMs) const {
    if ((nowMs / kAltPeriodMs) % 2 == 0) {
      // Integer c/s (rounded, not one-decimal) to cut visual jitter: a
      // smoothed 10 s rate still wobbles in the tenths, and the eye reads
      // a stable whole number far more easily.
      const float cps = pollRate_.cyclesPerSecondAt(nowMs);
      snprintf(buf, len, "%uc/s", static_cast<unsigned>(cps + 0.5f));
    } else {
      const uint32_t interval = pollRate_.intervalMsAt(nowMs);
      if (interval == 0) {
        snprintf(buf, len, "---ms");
      } else {
        snprintf(buf, len, "%ums", static_cast<unsigned>(interval));
      }
    }
  }

  /// Format one per-node row: "UAxx:STATE  XXXms  XXerr" — state tag,
  /// right-justified latency, and right-justified rolling error count,
  /// all in fixed-width fields so columns align across rows.
  /// `stateTag` is the 3-char state string the sketch computes (e.g.
  /// "ON ", "OFF", "---"). `turnaroundMs` is the node's
  /// statistics().lastTurnaroundMs.
  void nodeRowText(char* buf, size_t len, uint32_t nowMs, size_t nodeIndex,
                   uint8_t UA, const char* stateTag,
                   uint32_t turnaroundMs) const {
    char lat[8];
    latencyText(lat, sizeof(lat), turnaroundMs);
    const uint32_t recentErrs = (nodeIndex < kMaxNodes)
        ? nodeErrors_[nodeIndex].countInLastMs(nowMs, kErrorWindowMs)
        : 0;
    snprintf(buf, len, "UA%u:%s %s %2uerr", UA,
             stateTag ? stateTag : "---", lat, static_cast<unsigned>(recentErrs));
  }

 private:
  PollRate pollRate_;
  ErrorWindow nodeErrors_[kMaxNodes] = {};
  uint32_t prevNodeErrors_[kMaxNodes] = {};
};

}  // namespace examples
}  // namespace CMRInet
