// SerialCMRITransport.h — the CMRInet packet transport over a serial
// byte port (UART / RS-485).
//
// This class owns every byte-level concern of the serial medium: frame
// encoding through CMRIFrameCodec, TXEN discipline, drain timing,
// inter-byte timeout, and error accounting. The port beneath it
// (CMRISerialPort) is a dumb byte actuator, so desktop tests drive the
// exact discipline that runs on hardware.
//
// VALIDATION: Design v1.0 D4: framing belongs to the serial transport.
// Byte-level concerns (TXEN discipline, inter-byte timeout,
// dangling-DLE handling, gapless single-write emission, stop-bit
// config, UART error counters, two-SYN emission) live here or in the
// port adapter; protocol-level concerns stay in the strategy.
//
// TXEN discipline (assert, write, flush to full drain, deassert):
// VALIDATION: Interop v1.0 2.3.14: assert TXEN, write the frame, flush
// until the last byte leaves the shift register, then drop TXEN at
// once. Nothing here blocks (Design v1.0 D6), so "flush" is a
// non-blocking drain detector polled from tick(): TXEN drops when the
// wire-time estimate for the accepted bytes has elapsed AND the port
// reports its transmit path empty. A port with hardware drain
// knowledge tightens the timing; a buffer-only port is covered by the
// estimate, which includes the shift register.
//
// Receive never waits for transmit:
// VALIDATION: Interop v1.0 2.3.15: a fast Node begins its reply while
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
// VALIDATION: Design v1.0 D8: geometry ceilings are compile-time knobs.

// Received packets waiting for receivePacket(). The polled strategy
// consumes replies one exchange at a time, so a small queue suffices.
#ifndef CMRINET_SERIAL_RX_QUEUE
#define CMRINET_SERIAL_RX_QUEUE 4
#endif

namespace CMRInet {

class SerialCMRITransport : public CMRITransport {
 public:
  static constexpr size_t kRxQueueCapacity = CMRINET_SERIAL_RX_QUEUE;

  /// The transport drives, and never destroys, the given port. The
  /// port must outlive the transport (Design v1.0 D5: nothing is
  /// deallocated after begin()).
  explicit SerialCMRITransport(CMRISerialPort& port) : port_(port) {}

  // ------------------------------------------------ CMRITransport seam

  /// Initializes the port, releases the TXEN driver, and resets all
  /// runtime state and statistics. Applies the rate-derived default
  /// inter-byte timeout unless setInterByteTimeoutMs() overrode it.
  /// Allocates nothing.
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
  /// override survives begin(). Without an override, begin() derives
  /// the default from the port's character time.
  /// VALIDATION: Interop v1.0 2.2.6: abandon a partial frame when the
  /// inter-byte gap exceeds a limit; two to three character times is a
  /// reasonable default.
  void setInterByteTimeoutMs(uint32_t ms) {
    interByteTimeoutMs_ = ms;
    timeoutOverridden_ = true;
    decoder_.setInterByteTimeoutMs(ms);
  }

  /// The active receive inter-byte timeout (0 = disabled).
  uint32_t interByteTimeoutMs() const { return interByteTimeoutMs_; }

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
  uint32_t defaultInterByteTimeoutMs_() const;

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

  uint32_t lastTickMs_ = 0;
  uint32_t byteMicros_ = 1;  ///< cached port character time (set in begin)
  // The port's cumulative hardware error count at begin(), so stats()
  // reports errors since begin(), not since power-up.
  uint32_t hardwareErrorBaseline_ = 0;
  uint32_t interByteTimeoutMs_ = CMRIFrameDecoder::kDefaultInterByteTimeoutMs;
  bool timeoutOverridden_ = false;
};

}  // namespace CMRInet
