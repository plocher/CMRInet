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

/// A rolling count-per-second over a fixed window, same ring-buffer shape
/// as ErrorWindow. The window is tens of seconds long so the displayed
/// rate is smooth and readable at a glance on a 150 ms redraw cadence --
/// not flickering with every transient -- while a genuine stall still
/// shows up as a declining number within a few seconds. Capacity is sized
/// for the bench's ~55 Hz cadence over the window with comfortable margin.
class PollRate {
 public:
  static constexpr size_t kCapacity = 768;
  static constexpr uint32_t kWindowMs = 10000;

  void reset(uint32_t /*nowMs*/) { head_ = 0; count_ = 0; }

  void onPoll(uint32_t nowMs) {
    slots_[(head_ + count_) % kCapacity] = nowMs;
    if (count_ == kCapacity) {
      head_ = (head_ + 1) % kCapacity;
    } else {
      ++count_;
    }
  }

  /// Polls per second, evaluated at `nowMs` over the window. The window
  /// is half-open, `(nowMs - kWindowMs, nowMs]`: an event exactly
  /// kWindowMs old does not count toward the current window, which keeps
  /// the rate deterministic for a caller sampling at exact window
  /// boundaries.
  float cyclesPerSecondAt(uint32_t nowMs) const {
    uint32_t n = 0;
    for (size_t i = 0; i < count_; ++i) {
      const uint32_t t = slots_[(head_ + i) % kCapacity];
      if (nowMs >= t && (nowMs - t) < kWindowMs) {
        ++n;
      }
    }
    return static_cast<float>(n) * (1000.0f / kWindowMs);
  }

 private:
  uint32_t slots_[kCapacity] = {};
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

}  // namespace examples
}  // namespace CMRInet
