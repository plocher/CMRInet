// CMRITransport.h — the packet seam: transports for the CMRInet polled
// strategy.
//
// This is not a product-wide layer. It is the polled strategy's carrier
// boundary, so the CMRInet protocol can ride serial, mock, TCP, or MQTT
// carriers. Implementations end with the interface name:
// SerialCMRITransport, MockCMRITransport, TcpCMRITransport, ...
//
// VALIDATION: Design v1.1 "Transport contract (packet seam)": the
// clause documentation on CMRITransport mirrors the contract text.

#pragma once

#include <stdint.h>

#include "CMRIPacket.h"

namespace CMRInet {

/// Link liveness and error counters, in transport-neutral terms, so the
/// strategy folds them into node health without knowing the medium.
/// All counters start at 0 and never reset except via the owner.
struct LinkStatistics {
  uint32_t packetsSent = 0;      ///< packets accepted by sendPacket()
  uint32_t packetsReceived = 0;  ///< packets handed out by receivePacket()
  uint32_t sendRejects = 0;      ///< sendPacket() refusals (backpressure, link down)
  uint32_t receiveDrops = 0;     ///< inbound packets lost to full buffers
  uint32_t decodeErrors = 0;     ///< inbound integrity failures (framing, escaping)
  bool linkUp = true;            ///< carrier liveness as the transport knows it
};

/// Abstract carrier for the polled strategy. Packet-in / packet-out: the
/// engine above never sees bytes, framing, or the medium.
///
/// Clause semantics (normative):
/// - begin() may allocate. After it, no allocation and no deallocation,
///   ever. This applies to every implementation.
/// - tick(nowMs) is the sole CPU entry and never blocks. All client
///   work (UART pump, MQTT keepalive, reconnect) happens inside it.
/// - sendPacket() must not block. Accepted != on the wire. It returns
///   false only for backpressure or link-down. The caller retries on a
///   later tick.
/// - sendComplete() gates the strategy's reply timer. Serial: the last
///   byte left the shift register and TXEN dropped. Message transports:
///   the client accepted the message for delivery. An idle transport
///   reports true.
/// - receivePacket() returns packets in arrival order, at most one per
///   call, and only packets that passed the transport's integrity checks
///   (framing, escaping). Address filtering is not the transport's job.
/// - stats() surfaces link liveness and error counters in
///   transport-neutral terms.
class CMRITransport {
 public:
  virtual void begin() = 0;                        // may allocate
  virtual void tick(uint32_t nowMs) = 0;           // sole CPU entry, never blocks
  virtual bool sendPacket(const CMRIPacket&) = 0;  // accepted != on the wire
  virtual bool sendComplete() const = 0;           // true once fully delivered
  virtual bool receivePacket(CMRIPacket&) = 0;     // whole validated packets only
  virtual const LinkStatistics& stats() const = 0; // link errors and liveness

 protected:
  // The library never destroys a transport through the seam: nothing is
  // deallocated after begin(). The base destructor is therefore
  // protected and non-virtual.
  ~CMRITransport() = default;
};

}  // namespace CMRInet
