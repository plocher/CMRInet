// SerialCMRITransport.h — the CMRInet packet transport over a serial
// byte port (UART / RS-485).
//
// This class owns every byte-level concern of the serial medium: frame
// encoding through CMRIFrameCodec, TXEN discipline, drain timing,
// inter-byte timeout, and error accounting. The port beneath it
// (CMRISerialPort) is a dumb byte actuator, so desktop tests drive the
// exact discipline that runs on hardware.
//
// VALIDATION: Design v1.1 D4: framing belongs to the serial transport.
// Byte-level concerns (TXEN discipline, inter-byte timeout,
// dangling-DLE handling, gapless single-write emission, stop-bit
// config, UART error counters, two-SYN emission) live here or in the
// port adapter; protocol-level concerns stay in the strategy.
//
// TXEN discipline (assert, write, flush to full drain, deassert):
// VALIDATION: Interop v1.1 2.3.14: assert TXEN, write the frame, flush
// until the last byte leaves the shift register, then drop TXEN at
// once. Nothing here blocks (Design v1.1 D6), so "flush" is a
// non-blocking drain detector polled from tick(): TXEN drops when the
// wire-time estimate for the accepted bytes has elapsed AND the port
// reports its transmit path empty. A port with hardware drain
// knowledge tightens the timing; a buffer-only port is covered by the
// estimate, which includes the shift register. The conjunction is
// kept even for hardware-truth ports: the estimate never outlives a
// real drain, so it costs nothing and covers ports whose drain answer
// is optimistic by ignorance (Design v1.1 D13; see the seam contract
// on CMRISerialPort::transmitDrained()).
//
// Receive never waits for transmit:
// VALIDATION: Interop v1.1 2.3.15: a fast Node begins its reply while
// the Host's ETX/0x03 still drains, so the receive pump runs on every
// tick, including mid-drain.
//
// This class never allocates memory, not even in begin(). All storage
// is fixed-capacity, sized by CMRINET_MAX_BODY and the knob below.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CMRIFrameCodec.h"
#include "CMRIPacket.h"
#include "CMRISerialPort.h"
#include "CMRITransport.h"

// ---- Geometry knob: shrink for small targets. ----
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time knobs.

// Received packets waiting for receivePacket(). The polled strategy
// consumes replies one exchange at a time, so a small queue suffices.
#ifndef CMRINET_SERIAL_RX_QUEUE
#define CMRINET_SERIAL_RX_QUEUE 4
#endif

namespace CMRInet {

class SerialCMRITransport : public CMRITransport {
 public:
  static constexpr size_t kRxQueueCapacity = CMRINET_SERIAL_RX_QUEUE;

  /// The shipped/deployment inter-byte abort default: a tolerant limit
  /// (order 100 ms), not the rate-derived conformance value. The
  /// reference Host lineage transmitted with interpreter-scale gaps
  /// (interop 2.2.6) and fielded Nodes pace with dH/dL, so a strict
  /// shipped default would fail conforming history. The 250 ms reply
  /// gate is the truncation backstop.
  /// VALIDATION: Design v1.1 D13: the shipped abort is tolerant; the
  /// rate-derived value is a conformance instrument, not a default.
  static constexpr uint32_t kShippedInterByteTimeoutMs = 100;

  /// The transport drives, and never destroys, the given port. The
  /// port must outlive the transport (Design v1.2 D7: nothing is
  /// deallocated after begin()).
  explicit SerialCMRITransport(CMRISerialPort& port) : port_(port) {}

  // ------------------------------------------------ CMRITransport seam

  /// Initializes the port, releases the TXEN driver, and resets all
  /// runtime state and statistics. Applies the tolerant shipped
  /// inter-byte abort default (kShippedInterByteTimeoutMs, Design D13)
  /// unless setInterByteTimeoutMs() overrode it. Allocates nothing.
  void begin() override;

  /// Pumps transmit (remaining frame bytes, TXEN drop on full drain)
  /// and receive (port bytes through the frame decoder), and expires
  /// stale partial frames. `nowMs` must be monotonic. Never blocks.
  void tick(uint32_t nowMs) override;

  /// Encodes the packet into the staging buffer, asserts TXEN, and
  /// writes as much of the frame as the port accepts — the whole frame
  /// in one gapless write when the UART buffer has room (rule 2.1.5).
  /// Returns false, and counts a sendReject, while a previous send is
  /// still draining (backpressure) or when the body cannot be encoded.
  /// Accepted != on the wire: completion is reported by sendComplete().
  bool sendPacket(const CMRIPacket& packet) override;

  /// True once the last accepted send fully left the wire and TXEN
  /// dropped, or when nothing was ever sent. Gates the strategy's
  /// reply timer.
  bool sendComplete() const override { return txState_ == TxState::kIdle; }

  /// Pop the oldest received packet, in arrival order, at most one per
  /// call. Whole validated packets only: every packet passed the frame
  /// decoder's integrity checks. Address filtering is not done here.
  bool receivePacket(CMRIPacket& out) override;

  /// Link counters. decodeErrors folds together the frame decoder's
  /// integrity failures and the port's UART hardware error count, so
  /// the strategy sees one medium-neutral error figure. linkUp stays
  /// true: a UART has no carrier concept.
  const LinkStatistics& stats() const override { return stats_; }

  // ------------------------------------------------ serial configuration

  /// Override the receive inter-byte timeout (0 disables it — a
  /// conformance-grade receiver tolerates arbitrary gaps, interop
  /// 2.2.6 exception). May be called before or after begin(); the
  /// override survives begin(). Without an override, begin() applies
  /// the tolerant shipped default (kShippedInterByteTimeoutMs); call
  /// rateDerivedInterByteTimeoutMs() and pass it here for a
  /// conformance-strict receiver.
  /// VALIDATION: Interop v1.1 2.2.6: abandon a partial frame when the
  /// inter-byte gap exceeds a limit; two to three character times is a
  /// reasonable conformance default.
  /// VALIDATION: Design v1.1 D13: the shipped default is the tolerant
  /// deployment limit; the rate-derived value is a scenario/tracer verb.
  void setInterByteTimeoutMs(uint32_t ms) {
    interByteTimeoutMs_ = ms;
    timeoutOverridden_ = true;
    decoder_.setInterByteTimeoutMs(ms);
  }

  /// The active receive inter-byte timeout (0 = disabled).
  uint32_t interByteTimeoutMs() const { return interByteTimeoutMs_; }

  /// The conformance-strict inter-byte timeout derived from the port's
  /// character time (three character times, rounded up to ms, min 1 —
  /// interop 2.2.6). This is the rate-derived instrument, not the
  /// shipped default: pass it to setInterByteTimeoutMs() to opt into a
  /// conformance-strict receiver. Reads the port's character time
  /// directly, so it is valid before begin() as well as after.
  /// VALIDATION: Design v1.1 D13: rate-derived abort is a conformance
  /// verb, never a compile-time or shipped default.
  uint32_t rateDerivedInterByteTimeoutMs() const;

  /// Echo-cancel mode (issue #104, ADR-0003). On a 2-wire bus
  /// with a misconfigured driver (!RE tied active), the Host's
  /// receiver hears its own TX echo. The byte-level discard
  /// runs while TXEN is asserted — through kWriting and
  /// kDraining, until deassert — because a Node cannot reply
  /// until it has received ETX (interop 2.3.15, E10), so no
  /// legitimate reply arrives inside the TXEN-asserted window.
  /// This is mitigation-of-misconfiguration, not a feature.
  ///
  /// - Auto (the default): watch for RX bytes while TXEN is
  ///   asserted; on the first observation, count it (rxDuringTx),
  ///   arm the discard permanently, and surface a diagnostic via
  ///   rxDuringTx(). The host/shell reads the counter.
  /// - AlwaysOn: always discard RX while TXEN asserted.
  /// - AlwaysOff: never discard; RX bytes feed the decoder as
  ///   usual (4-wire or fixed 2-wire explicit opt-out).
  ///
  /// May be called before or after begin(); the override
  /// survives begin(). Without an override, begin() applies
  /// Auto.
  enum class EchoCancelMode : uint8_t {
    kAuto,       ///< watch for RX during TX; arm on first observation
    kAlwaysOn,   ///< always discard RX while TXEN asserted
    kAlwaysOff,  ///< never discard (4-wire or fixed 2-wire opt-out)
  };

  void setEchoCancelMode(EchoCancelMode mode) {
    echoCancelMode_ = mode;
    echoCancelOverridden_ = true;
  }

  /// The active echo-cancel mode.
  EchoCancelMode echoCancelMode() const { return echoCancelMode_; }

  /// Cumulative count of RX bytes observed while TXEN was
  /// asserted and the discard was not active (AlwaysOff, or
  /// Auto before arming). Zero when the discard is eating the
  /// echo (AlwaysOn, or Auto after arming). Serial-transport-
  /// scoped defect signal; not on the base seam or the host.
  uint32_t rxDuringTx() const { return rxDuringTx_; }

  /// Override the receive gap-observability thresholds (see
  /// CMRIFrameDecoder::setSlowGapThresholdsMs). May be called before or
  /// after begin(); the override survives begin(). Without an override,
  /// begin() derives both from the port's character time: lo = one char
  /// time (the streaming floor), hi = three char times (the suspicion
  /// floor). lo = 0 disables observability.
  void setSlowGapThresholdsMs(uint32_t loMs, uint32_t hiMs) {
    slowGapLoMs_ = loMs;
    slowGapHiMs_ = hiMs;
    slowGapOverridden_ = true;
    decoder_.setSlowGapThresholdsMs(loMs, hiMs);
  }

  /// The active observation floor (0 = observability off).
  uint32_t slowGapLoMs() const { return slowGapLoMs_; }

  /// The active slowGaps trigger (0 or <= lo = slowGaps disabled).
  uint32_t slowGapHiMs() const { return slowGapHiMs_; }

  /// Frame-decoder health counters: the breakdown behind
  /// stats().decodeErrors (timeout aborts, dangling DLEs, restarts,
  /// overflows).
  const CMRIFrameDecoder::Statistics& decoderStatistics() const {
    return decoder_.statistics();
  }

 private:
  enum class TxState : uint8_t {
    kIdle,     ///< nothing in flight; TXEN deasserted
    kWriting,  ///< frame bytes remain to hand to the port
    kDraining  ///< all bytes accepted; waiting for full drain
  };

  void pumpTransmit_(uint32_t nowMs);
  void pumpReceive_(uint32_t nowMs);
  void drainDecoder_();
  void syncErrors_();
  uint32_t wireTimeMs_(size_t bytes) const;
  // Three character times (interop 2.2.6) from a given per-character
  // micros, rounded up to ms, min 1. Shared by the rate-derived abort
  // accessor and the slow-gap suspicion floor so the two cannot drift.
  uint32_t threeCharTimesMs_(uint32_t byteMicros) const;
  // One character time (interop 2.2.6), rounded up to ms, min 1.
  // Used by the echo-cancel guard band in sendPacket so the
  // drain estimate holds TXEN one char time past shift-register-empty.
  uint32_t oneCharTimeMs_() const;
  uint32_t defaultSlowGapLoMs_() const;
  uint32_t defaultSlowGapHiMs_() const;

  CMRISerialPort& port_;
  CMRIFrameDecoder decoder_;
  LinkStatistics stats_;

  // Receive queue (FIFO ring). Arrival order is preserved by keeping
  // the oldest and dropping the newest on overflow.
  struct {
    CMRIPacket slots[kRxQueueCapacity];
    size_t head = 0;
    size_t count = 0;
  } rxQueue_;

  // Transmit staging: one fully escaped wire frame (rule 2.1.6 sizing).
  uint8_t txFrame_[kMaxWireFrame] = {0};
  size_t txLength_ = 0;   ///< frame bytes staged
  size_t txWritten_ = 0;  ///< frame bytes the port has accepted
  TxState txState_ = TxState::kIdle;
  // Wire-drain estimate: the latest of (chunk-accept time + chunk wire
  // time) and (send-start time + whole-frame wire time). Meaningful
  // only while txState_ != kIdle (explicit state, no time sentinel —
  // see CMRITime.h).
  uint32_t drainDueMs_ = 0;
  // Deferred txState_ flip: latched by pumpTransmit_ on drain-complete,
  // committed in tick() after pumpReceive_ so the echo discard
  // sees kDraining for the whole tick (ADR-0003 tick consistency).
  // The seed of a general deferred-state-commit pattern; see
  // the TickState tech-debt issue.
  bool pendingIdle_ = false;

  uint32_t lastTickMs_ = 0;
  uint32_t byteMicros_ = 1;  ///< cached port character time (set in begin)
  // The port's cumulative hardware error count at begin(), so stats()
  // reports errors since begin(), not since power-up.
  uint32_t hardwareErrorBaseline_ = 0;
  uint32_t interByteTimeoutMs_ = kShippedInterByteTimeoutMs;
  bool timeoutOverridden_ = false;
  uint32_t slowGapLoMs_ = 0;   ///< observation floor; 0 = off (derived in begin)
  uint32_t slowGapHiMs_ = 0;   ///< slowGaps trigger; <= lo = watermark-only
  bool slowGapOverridden_ = false;
  /// Echo cancellation (issue #104, ADR-0003): mitigation
  /// of a misconfigured 2-wire bus driver. The byte-level
  /// discard runs while TXEN is asserted (kWriting and
  /// kDraining, until deassert). Default Auto; survives begin().
  EchoCancelMode echoCancelMode_ = EchoCancelMode::kAuto;
  bool echoCancelOverridden_ = false;  ///< setEchoCancelMode() was called
  bool echoCancelArmed_ = false;      ///< Auto: armed on first rxDuringTx
  uint32_t rxDuringTx_ = 0;           ///< RX bytes seen during TX, discard off
};

}  // namespace CMRInet
