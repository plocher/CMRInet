// AvrSerialPort.h — StreamSerialPort that owns the AVR UART
// configuration.
//
// The AVR sibling of Esp32SerialPort: the constructor takes a
// HardwareSerial (Serial on single-USART parts like the ATmega328P,
// Serial1 on multi-USART parts like the ATmega32U4) and the port's
// begin() configures it — line rate and character framing — so the
// sketch no longer calls Serial.begin() itself. The duplicate
// initializer and its ordering constraint ("configure the UART
// before node.begin()") disappear: the constructor's baud, which
// StreamSerialPort already stored for the wire-time math, is now
// also the value that configures the wire, so the two cannot skew.
//
// Why AVR needs a subclass: StreamSerialPort binds Stream&, and the
// abstract Stream has no begin() — that generality is deliberate
// (SoftwareSerial, USB-CDC streams, test-rig streams all bind). A
// configuration-owning port must bind the concrete HardwareSerial&,
// the same trade Esp32SerialPort makes. Nothing is lost for an
// RS-485 CMRI bus: 8N2 framing needs a real USART anyway, so the
// streams that cannot be configured this way were never candidates.
//
// What this class does NOT change: drain semantics. AVR keeps
// StreamSerialPort's buffer-level transmitDrained() answer plus the
// transport's wire-time estimate — the permitted
// optimistic-by-ignorance floor of the seam contract. The TXCn bit
// of UCSRnA is the AVR hardware-truth source, and a
// transmitDrained() override reading it is the contribution pattern
// sketched in serialStream.h — still bench-gated: do not add it
// without a wire tap proving the timing. On the ProMini target the
// TXEN line is not even port-owned (the MRCS AutoRTS 555 circuit
// holds driver enable past the last stop bit), so the estimate
// gates engine sequencing only, never wire integrity.
//
// Platform guard: the whole file is inside #if defined(ARDUINO) &&
// defined(ARDUINO_ARCH_AVR). That is a platform guard on a
// platform-specific port, not a feature #ifdef (Design v1.1 D7
// clarifies: platform guards on platform-specific ports are not
// feature toggles). Non-AVR builds see an empty file; non-including
// AVR sketches pay nothing (header-only, linker-drop).
//
// VALIDATION: Interop v1.1 2.5.1: transmit 8N2 by default; accept
// 8N1 configuration where a network requires it (erratum E2). The
// `config` parameter carries the sketch's framing choice into
// HardwareSerial::begin(); `bitsPerChar` must match it (11 for 8N2,
// 10 for 8N1) so byteDurationMicros() stays honest.

#pragma once

#if defined(ARDUINO) && defined(ARDUINO_ARCH_AVR)

#include <Arduino.h>

#include "serialStream.h"

namespace CMRInet {

/// StreamSerialPort that configures the AVR HardwareSerial in its
/// own begin(), removing the sketch-owned Serial.begin() step and
/// its ordering constraint. Drain answers stay at the
/// StreamSerialPort floor: the TXCn hardware-truth override is a
/// separate, bench-gated contribution (serialStream.h).
class AvrSerialPort : public StreamSerialPort {
 public:
  /// Construct the port. `stream`: the bus USART (Serial,
  /// Serial1, ...). `txenPin`: RS-485 driver-enable pin, or
  /// kNoTxenPin for auto-direction hardware. `baud`: line rate.
  /// `config`: UART framing passed to HardwareSerial::begin()
  /// (SERIAL_8N2 default). `bitsPerChar`: bit times per character
  /// for byteDurationMicros() (11 for 8N2, 10 for 8N1; must match
  /// `config`).
  AvrSerialPort(HardwareSerial& stream, int txenPin, uint32_t baud,
                uint8_t config = SERIAL_8N2, uint8_t bitsPerChar = 11)
      : StreamSerialPort(stream, txenPin, baud, bitsPerChar),
        hwStream_(stream),
        uartBaud_(baud),
        uartConfig_(config) {}

  /// Configure the UART, then delegate to the parent. The UART is
  /// initialized here (from setup() via the begin() chain), not in
  /// the constructor — so the sketch no longer calls Serial.begin().
  void begin() override {
    hwStream_.begin(uartBaud_, uartConfig_);
    StreamSerialPort::begin();
  }

 private:
  HardwareSerial& hwStream_;
  uint32_t uartBaud_;
  uint8_t uartConfig_;
};

}  // namespace CMRInet

#endif  // ARDUINO && ARDUINO_ARCH_AVR
