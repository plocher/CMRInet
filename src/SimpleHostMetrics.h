// SimpleHostMetrics.h — pure, platform-neutral helpers the SimpleHost
// example's OLED status panel uses: a rolling count of events inside a
// time window (for "recent errors" / "recent misses"), a rolling
// count-per-second rate (for polling cadence), and a fixed-width latency
// formatter. None of this depends on the Arduino runtime or the display
// library; it is shared between the example sketch and the desktop test
// suite so the arithmetic is verifiable under a deterministic mock clock.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/// Clamp a counter for a fixed-width field (2 or 3 digits). Values at
/// or above the max print as the max (e.g. 99, 999) so columns stay put.
inline unsigned clampCount(uint32_t v, unsigned maxShown) {
  return (v > maxShown) ? maxShown : static_cast<unsigned>(v);
}

/// The shared Host status panel logic: owns the rolling metric state
/// (polling rate, per-node error and miss windows) and formats the
/// header, host-totals, and per-node row strings a sketch renders to
/// its display. Pure — no display or Arduino dependency — so the
/// arithmetic and alternation timing are verifiable under a
/// deterministic mock clock.
///
/// Layout contract (21-char rows, 6px font on 128px):
///   HOST           <cadence>          // size-2 HOST + size-1 rate
///   P:nnnn R:nnnn m:nn                // host totals (this window)
///   UA30  165ms  12m  0e              // online: no state tag
///   UA31 OFF  ---ms   5m  0e          // non-online: compact tag
///
/// "ON" is omitted when the node is online — live counters already say
/// the node is answering. Misses (noReplies) are first-class: the
/// previous "0err" line tracked only malformed replies and stayed at
/// zero through reply-gate timeouts that still stall TXEN.
class HostStatusPanel {
 public:
  static constexpr size_t kMaxNodes = 16;
  static constexpr uint32_t kErrorWindowMs = 5000;
  static constexpr uint32_t kAltPeriodMs = 5000;  // c/s ↔ ms/cycle every 5 s

  void reset() {
    pollRate_.reset(0);
    for (size_t i = 0; i < kMaxNodes; ++i) {
      nodeErrors_[i].reset();
      nodeMisses_[i].reset();
      prevNodeErrors_[i] = 0;
      prevNodeMisses_[i] = 0;
    }
    prevPollsSent_ = 0;
    prevRepliesAccepted_ = 0;
    windowPolls_ = 0;
    windowReplies_ = 0;
    haveBaseline_ = false;
  }

  /// Feed the panel current cumulative counters on each redraw.
  ///
  /// Host: `pollsSent` / `repliesAccepted` from CMRIHost::statistics().
  /// Per node (parallel arrays, length `nodeCount`):
  ///   nodeErrorCounts  — statistics().errors   (malformed replies)
  ///   nodeMissCounts   — statistics().noReplies (reply-gate timeouts)
  /// Either node array may be null (treated as all zeros).
  void sample(uint32_t nowMs, uint32_t pollsSent, uint32_t repliesAccepted,
              const uint32_t* nodeErrorCounts, const uint32_t* nodeMissCounts,
              size_t nodeCount) {
    pollRate_.sample(nowMs, pollsSent);

    if (!haveBaseline_) {
      prevPollsSent_ = pollsSent;
      prevRepliesAccepted_ = repliesAccepted;
      haveBaseline_ = true;
    } else {
      // Rolling window totals: on each sample, credit the delta since
      // the previous sample into a short-lived accumulator that the
      // host-totals line reads. We also feed miss deltas into the
      // per-node ErrorWindows so "12m" is last-5s, not lifetime.
      if (pollsSent >= prevPollsSent_) {
        windowPolls_ = pollsSent - prevPollsSent_;
      }
      if (repliesAccepted >= prevRepliesAccepted_) {
        windowReplies_ = repliesAccepted - prevRepliesAccepted_;
      }
      prevPollsSent_ = pollsSent;
      prevRepliesAccepted_ = repliesAccepted;
    }

    for (size_t i = 0; i < nodeCount && i < kMaxNodes; ++i) {
      if (nodeErrorCounts != nullptr) {
        while (nodeErrorCounts[i] != prevNodeErrors_[i]) {
          nodeErrors_[i].onEvent(nowMs);
          ++prevNodeErrors_[i];
        }
      }
      if (nodeMissCounts != nullptr) {
        while (nodeMissCounts[i] != prevNodeMisses_[i]) {
          nodeMisses_[i].onEvent(nowMs);
          ++prevNodeMisses_[i];
        }
      }
    }
  }

  /// Format the header cadence: alternates "XXXc/s" and "XXXms" every
  /// kAltPeriodMs. Stalled engine shows "---ms".
  void headerText(char* buf, size_t len, uint32_t nowMs) const {
    if ((nowMs / kAltPeriodMs) % 2 == 0) {
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

  /// Host-scope totals for the calibration line:
  /// "P:nnnn R:nnnn m:nn" — polls/replies since last sample (≈ redraw),
  /// and misses in the last kErrorWindowMs across all nodes.
  /// Inter-sample P/R shows the bus is moving without burning digits on
  /// a lifetime counter; m is the same rolling window the node rows use.
  void hostTotalsText(char* buf, size_t len, uint32_t nowMs) const {
    uint32_t missSum = 0;
    for (size_t i = 0; i < kMaxNodes; ++i) {
      missSum += nodeMisses_[i].countInLastMs(nowMs, kErrorWindowMs);
    }
    snprintf(buf, len, "P:%u R:%u m:%u",
             clampCount(windowPolls_, 9999),
             clampCount(windowReplies_, 9999),
             clampCount(missSum, 99));
  }

  /// One per-node row.
  ///
  /// Online (`online == true`): no state tag — counters are the proof.
  ///   "UA30  165ms  12m  0e"
  /// Non-online: compact 3-char tag kept so OFF/CFG/DEG stay visible.
  ///   "UA31 OFF  ---ms   5m  0e"
  ///
  /// `stateTag` is still required for the non-online form (e.g. from
  /// remoteNodeStateTag). `turnaroundMs` is lastTurnaroundMs.
  void nodeRowText(char* buf, size_t len, uint32_t nowMs, size_t nodeIndex,
                   uint8_t UA, bool online, const char* stateTag,
                   uint32_t turnaroundMs) const {
    char lat[8];
    if (online) {
      latencyText(lat, sizeof(lat), turnaroundMs);
    } else {
      // No meaningful turnaround when the node is not answering.
      snprintf(lat, sizeof(lat), "  ---");
    }
    const uint32_t recentErrs = (nodeIndex < kMaxNodes)
        ? nodeErrors_[nodeIndex].countInLastMs(nowMs, kErrorWindowMs)
        : 0;
    const uint32_t recentMisses = (nodeIndex < kMaxNodes)
        ? nodeMisses_[nodeIndex].countInLastMs(nowMs, kErrorWindowMs)
        : 0;
    const unsigned e = clampCount(recentErrs, 99);
    const unsigned m = clampCount(recentMisses, 999);
    if (online) {
      // "UA30  165ms  12m  0e" — tag omitted on purpose.
      // lat already includes the "ms" suffix from latencyText().
      // Worst-case under -Wformat-truncation is ~20 bytes; callers
      // must pass a buffer of at least 24 (display rows use 24).
      snprintf(buf, len, "UA%u %s %3um %2ue", UA, lat, m, e);
    } else {
      const char* tag = (stateTag != nullptr) ? stateTag : "---";
      // "UA31 OFF  ---ms  5m  0e" — lat is 5 chars ("  ---"); tag is
      // 3. Upper bound ~26 with UA=127; keep row buffers >= 28 so the
      // ESP32 -Werror=format-truncation gate stays clean (#112 flash).
      snprintf(buf, len, "UA%u %s %s %3um %2ue", UA, tag, lat, m, e);
    }
  }

 private:
  PollRate pollRate_;
  ErrorWindow nodeErrors_[kMaxNodes] = {};
  ErrorWindow nodeMisses_[kMaxNodes] = {};
  uint32_t prevNodeErrors_[kMaxNodes] = {};
  uint32_t prevNodeMisses_[kMaxNodes] = {};
  uint32_t prevPollsSent_ = 0;
  uint32_t prevRepliesAccepted_ = 0;
  uint32_t windowPolls_ = 0;
  uint32_t windowReplies_ = 0;
  bool haveBaseline_ = false;
};

}  // namespace examples
}  // namespace CMRInet
