// PosixSerialPort.h — the SerialPort seam over a POSIX termios
// device (macOS / Linux), for the desktop Host harness.
//
// Lives in extras/ so the Arduino build never compiles it. The port is
// fully raw: no IXON/IXOFF software flow control, no ICRNL, no line
// discipline of any kind. An R body legitimately carries raw 0x13
// (DC3/XOFF — only 2/3/16 are escaped), and a cooked port would
// silently freeze host TX on it.
// VALIDATION: Interop v1.1 2.5.1: 8N2 default framing;
// byteDurationMicros() reflects the configured stop bits.
// (Also: docs/research/review-CMRI-Controller-host.md Finding 7 — the
// classic Unix Host shipped with IXON enabled and could be frozen by
// one input byte pattern. termios must be fully raw.)

#pragma once

#include <stdint.h>

#include "transport/serialPort.h"

namespace CMRInet {

/// SerialPort over a POSIX serial device. Non-blocking throughout:
/// the descriptor is opened O_NONBLOCK and every method returns at
/// once (Design v1.1 D6).
class PosixSerialPort : public SerialPort {
 public:
  /// `device` must outlive the port (it is not copied). `stopBits2`
  /// selects 8N2 (the CMRInet default) versus 8N1.
  PosixSerialPort(const char* device, uint32_t baud,
                      bool stopBits2 = true);

  /// Open and configure the device. Idempotent. On failure the port
  /// stays closed: isOpen() is false and lastError() names the step.
  void begin() override;

  int readByte() override;
  size_t writeBytes(const uint8_t* bytes, size_t length) override;
  bool transmitDrained() const override;

  /// No-op: the 4-wire RS-422/485 adapter's driver permanently owns
  /// the poll pair, so there is no direction line to manage.
  /// VALIDATION: Interop v1.1 2.3.14: the port is only the actuator;
  /// a port on auto-direction hardware implements this as a no-op.
  void setTransmitEnable(bool enabled) override;

  uint32_t byteDurationMicros() const override;

  // ------------------------------------------------ desktop extras

  bool isOpen() const { return fd_ >= 0; }

  /// Human-readable failure description from begin() (empty when ok).
  const char* lastError() const { return lastError_; }

  /// Close the descriptor. Desktop-only convenience (the library
  /// itself never destroys a port after begin()).
  void close();

 private:
  const char* device_;
  uint32_t baud_;
  bool stopBits2_;
  int fd_ = -1;
  const char* lastError_ = "";
};

}  // namespace CMRInet
