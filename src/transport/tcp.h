// tcp.h — the CMRInet packet transport over a TCP byte stream.
//
// This class owns every byte-level concern of the TCP medium: frame
// encoding/decoding through CMRIFrameCodec, socket write backpressure,
// carrier liveness tracking (linkUp reflects socket connected state),
// inter-byte timeout tuning (default disabled for IP jitter), and error
// accounting. The port beneath it (TcpPort) is a non-blocking byte actuator,
// so desktop tests drive the exact discipline that runs on hardware.
//
// VALIDATION: Design v1.7 D1: Transport implementations end with the
// interface they implement: TcpCMRITransport.
// VALIDATION: Design v1.1 D6: every method must be non-blocking.
// VALIDATION: ADR-0004: TCP as the alternate carrier for JMRI interop.
//
// Sizing and memory:
// This class never allocates memory, not even in begin(). All storage
// is fixed-capacity, sized by the body ceiling (CMRIPacket.h) and the
// queue depth below.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CMRIFrameCodec.h"
#include "CMRIPacket.h"
#include "CMRITransport.h"
#include "tcpPort.h"

// ---- Receive queue depth ----
// Received packets waiting for receivePacket(). Sized identically to
// serial transport: a property of the polled strategy.
#ifndef CMRINET_TCP_RX_QUEUE
#define CMRINET_TCP_RX_QUEUE 4
#endif

namespace CMRInet {

class TcpCMRITransport : public CMRITransport {
 public:
  static constexpr size_t kRxQueueCapacity = CMRINET_TCP_RX_QUEUE;

  /// Default inter-byte timeout for TCP: 0 (disabled). Over IP/WiFi,
  /// packet fragmentation and network jitter introduce arbitrary gaps
  /// between bytes in a single frame. Disabling the inter-byte abort by
  /// default ensures valid frames are not discarded due to network pacing.
  /// Call setInterByteTimeoutMs() to override if a strict limit is desired.
  static constexpr uint32_t kDefaultInterByteTimeoutMs = 0;

  /// The transport drives, and never destroys, the given port. The
  /// port must outlive the transport (Design v1.2 D7: nothing is
  /// deallocated after begin()).
  explicit TcpCMRITransport(TcpPort& port) : port_(port) {}

  // ------------------------------------------------ CMRITransport seam

  /// Initializes the port and resets all runtime state and statistics.
  /// Applies kDefaultInterByteTimeoutMs unless setInterByteTimeoutMs()
  /// overrode it. Allocates nothing.
  void begin() override;

  /// Pumps transmit (remaining frame bytes to socket) and receive
  /// (socket bytes through the frame decoder), polls the port hook,
  /// updates link liveness, and expires stale partial frames if timeout
  /// is enabled. `nowMs` must be monotonic. Never blocks.
  void tick(uint32_t nowMs) override;

  /// Encodes the packet into the staging buffer and writes as much of the
  /// frame as the socket accepts. Returns false, and counts a sendReject,
  /// when the link is down (!port_.connected()), while a previous send is
  /// still in flight (backpressure), or when the body cannot be encoded.
  /// Accepted != on the wire: completion is reported by sendComplete().
  bool sendPacket(const CMRIPacket& packet) override;

  /// True once the last accepted send was fully written to the port and
  /// the port reports transmitDrained(). Gates the strategy's reply timer.
  bool sendComplete() const override;

  /// Pop the oldest received packet, in arrival order, at most one per
  /// call. Whole validated packets only: every packet passed the frame
  /// decoder's integrity checks. Address filtering is not done here.
  bool receivePacket(CMRIPacket& out) override;

  /// Link counters and carrier status. linkUp dynamically reflects
  /// whether the TCP connection is active (port_.connected()).
  const LinkStatistics& stats() const override { return stats_; }

  // ------------------------------------------------ TCP configuration

  /// Override the receive inter-byte timeout (0 disables it, the TCP
  /// default). May be called before or after begin(); survives begin().
  void setInterByteTimeoutMs(uint32_t ms) {
    interByteTimeoutMs_ = ms;
    timeoutOverridden_ = true;
    decoder_.setInterByteTimeoutMs(ms);
  }

  /// The active receive inter-byte timeout (0 = disabled).
  uint32_t interByteTimeoutMs() const { return interByteTimeoutMs_; }

  /// Override the receive gap-observability thresholds.
  void setSlowGapThresholdsMs(uint32_t loMs, uint32_t hiMs) {
    decoder_.setSlowGapThresholdsMs(loMs, hiMs);
  }

  /// Frame-decoder health counters: breakdown of decoding failures.
  const CMRIFrameDecoder::Statistics& decoderStatistics() const {
    return decoder_.statistics();
  }

 private:
  void pumpTransmit_();
  void pumpReceive_(uint32_t nowMs);
  void drainDecoder_();
  void syncErrors_();

  TcpPort& port_;
  CMRIFrameDecoder decoder_;
  LinkStatistics stats_;

  // Receive queue (FIFO ring).
  struct {
    CMRIPacket slots[kRxQueueCapacity];
    size_t head = 0;
    size_t count = 0;
  } rxQueue_;

  // Transmit staging: one fully escaped wire frame (rule 2.1.6 sizing).
  uint8_t txFrame_[kMaxWireFrame] = {0};
  size_t txLength_ = 0;
  size_t txWritten_ = 0;
  bool txInProgress_ = false;

  uint32_t socketErrorBaseline_ = 0;
  uint32_t interByteTimeoutMs_ = kDefaultInterByteTimeoutMs;
  bool timeoutOverridden_ = false;
};

}  // namespace CMRInet
