// CMRISerialPort.h — the byte-port seam under SerialCMRITransport.
//
// SerialCMRITransport owns the CMRInet byte-level discipline: codec,
// TXEN ordering, drain timing, inter-byte timeout, error accounting.
// The port is the thin actuator beneath it: raw bytes in and out, the
// TXEN line, and whatever the UART hardware truly knows. Splitting here
// keeps every rule the profile cares about inside the library sources,
// so desktop tests exercise the exact discipline that runs on hardware,
// with only this dumb seam faked.
//
// VALIDATION: Design v1.0 D4: byte-level concerns live in the serial
// adapter. The port carries bytes; the transport above it carries the
// discipline.
//
// Implementations end with the interface name (Design v1.0 D1):
// StreamCMRISerialPort, FakeCMRISerialPort, ...

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace CMRInet {

/// Abstract byte port for SerialCMRITransport. Every method must be
/// non-blocking (Design v1.0 D6: nothing in the library blocks).
class CMRISerialPort {
 public:
  /// Prepare the port. The UART itself (baud, character framing) is
  /// configured by the sketch or adapter before or during begin().
  /// VALIDATION: Interop v1.1 2.5.1: transmit 8N2 by default; accept
  /// 8N1 configuration where a network requires it (erratum E2). The
  /// stop-bit choice is the adapter's configuration hook, and
  /// byteDurationMicros() must reflect it.
  virtual void begin() = 0;

  /// One received byte (0..255), or -1 when none is waiting.
  virtual int readByte() = 0;

  /// Queue up to `length` bytes for transmission without blocking.
  /// Returns how many bytes were accepted (0 when the TX buffer is
  /// full). The transport retries the remainder on a later tick.
  virtual size_t writeBytes(const uint8_t* bytes, size_t length) = 0;

  /// True when the port believes its transmit path is empty. A port
  /// with real hardware knowledge (TXC flag, uart_wait_tx_done poll)
  /// answers for the shift register too; a buffer-only adapter answers
  /// for its software/FIFO buffer and the transport's wire-time
  /// estimate covers the shift register.
  virtual bool transmitDrained() const = 0;

  /// Drive the RS-485 driver-enable line. A port on a converter that
  /// manages direction itself (auto-direction hardware) implements
  /// this as a no-op.
  /// VALIDATION: Interop v1.1 2.3.14: the transport asserts TXEN,
  /// writes the frame, flushes until the last byte leaves the shift
  /// register, then drops TXEN at once. The port is only the actuator;
  /// the ordering lives in SerialCMRITransport.
  virtual void setTransmitEnable(bool enabled) = 0;

  /// Duration of one character on the wire, in microseconds, for the
  /// configured baud rate and framing (11 bit times for 8N2, 10 for
  /// 8N1). The transport derives its drain estimate and its default
  /// inter-byte timeout from this. Must be nonzero.
  virtual uint32_t byteDurationMicros() const = 0;

  /// Cumulative count of UART-level receive errors (framing, parity,
  /// overrun) where the hardware exposes them. Ports without access
  /// keep the default 0. Never resets.
  virtual uint32_t hardwareErrorCount() const { return 0; }

 protected:
  // The transport never destroys a port through the seam: nothing is
  // deallocated after begin() (Design v1.0 D5). Protected non-virtual
  // destructor, matching CMRITransport.
  ~CMRISerialPort() = default;
};

}  // namespace CMRInet
