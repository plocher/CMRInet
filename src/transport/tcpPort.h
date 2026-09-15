// tcpPort.h — the byte-port seam under TcpCMRITransport.
//
// TcpCMRITransport owns the CMRInet byte-level discipline for TCP:
// frame encoding/decoding through CMRIFrameCodec, queue depth, link
// liveness reporting, and error accounting. The port is the thin
// actuator beneath it: raw bytes in and out, socket connection state,
// and platform-specific socket polling. Splitting here keeps every rule
// the profile cares about inside the library sources, so desktop tests
// exercise the exact transport discipline that runs on hardware.
//
// VALIDATION: Design v1.7 D1: transport implementations end with the
// interface they implement (TcpCMRITransport over TcpPort).
// VALIDATION: Design v1.1 D6: every method must be non-blocking.
// VALIDATION: ADR-0004: TCP as the alternate carrier for JMRI interop.
//
// Implementations end with the interface name (Design v1.1 D1):
// ClientTcpPort, FakeTcpPort, PosixTcpPort, ...

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace CMRInet {

/// Abstract byte port for TcpCMRITransport. Every method must be
/// non-blocking (Design v1.1 D6: nothing in the library blocks).
class TcpPort {
 public:
  /// Prepare the port. Allocates nothing after begin().
  virtual void begin() = 0;

  /// True when the TCP connection / socket is active and ready for I/O.
  virtual bool connected() const = 0;

  /// One received byte (0..255), or -1 when none is waiting.
  virtual int readByte() = 0;

  /// Queue up to `length` bytes for transmission without blocking.
  /// Returns how many bytes were accepted (0 when the socket buffer is
  /// full or link is down). The transport retries the remainder on a
  /// later tick.
  virtual size_t writeBytes(const uint8_t* bytes, size_t length) = 0;

  /// True when the port believes its socket transmit buffer is drained.
  virtual bool transmitDrained() const { return true; }

  /// Periodic maintenance hook called from TcpCMRITransport::tick().
  /// Used by implementations for connection listening, accept, or
  /// keepalive checks without blocking.
  virtual void poll(uint32_t nowMs) { (void)nowMs; }

  /// Cumulative count of socket/link errors (disconnects, resets,
  /// write faults). Ports without access keep the default 0. Never resets.
  virtual uint32_t socketErrorCount() const { return 0; }

 protected:
  // The transport never destroys a port through the seam: nothing is
  // deallocated after begin() (Design v1.2 D7). Protected non-virtual
  // destructor, matching CMRITransport and SerialPort.
  ~TcpPort() = default;
};

}  // namespace CMRInet
