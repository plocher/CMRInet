// StreamCMRISerialPort.h — Arduino adapter: CMRISerialPort over an
// Arduino Stream (usually a HardwareSerial) plus an optional TXEN pin.
//
// Arduino-only: this header compiles under the Arduino toolchain and
// is excluded from desktop builds, which exercise the transport
// through a fake port instead. Nothing here allocates.
//
// The sketch owns the UART configuration and calls, for example,
//   Serial1.begin(19200, SERIAL_8N2);
// before this port's begin(). Two stop bits is the transmit default
// the fielded ecosystem expects; pass bitsPerChar 10 for an 8N1
// network.
// VALIDATION: Interop v1.1 2.5.1: transmit 8N2 by default; accept 8N1
// configuration where a network requires it (erratum E2). The
// stop-bit hook is the sketch's Serial.begin() config plus this
// adapter's bitsPerChar, which keeps the wire-time math honest.
//
// Drain fidelity: the Stream API exposes only the software buffer
// (availableForWrite), not the shift register, so transmitDrained()
// answers for the buffer and SerialCMRITransport's wire-time estimate
// covers the rest. On a core that exposes true TX-complete status,
// subclass CMRISerialPort directly for tighter TXEN turnaround.

#pragma once

#ifdef ARDUINO

#include <Arduino.h>

#include "CMRISerialPort.h"

namespace CMRInet {

class StreamCMRISerialPort : public CMRISerialPort {
 public:
  /// No TXEN pin: the converter manages direction itself
  /// (auto-direction hardware), or the medium is not RS-485.
  static constexpr int kNoTxenPin = -1;

  /// `stream`: the configured serial stream (sketch calls its begin()
  /// first). `txenPin`: RS-485 driver-enable pin, or kNoTxenPin.
  /// `baud`: the configured line rate. `bitsPerChar`: bit times per
  /// character on the wire — 11 for 8N2 (default), 10 for 8N1.
  StreamCMRISerialPort(Stream& stream, int txenPin, uint32_t baud,
                       uint8_t bitsPerChar = 11)
      : stream_(stream),
        txenPin_(txenPin),
        baud_(baud ? baud : 1),
        bitsPerChar_(bitsPerChar ? bitsPerChar : 11) {}

  void begin() override {
    if (txenPin_ != kNoTxenPin) {
      pinMode(txenPin_, OUTPUT);
      digitalWrite(txenPin_, LOW);  // receiver by default
    }
    // The TX buffer is empty now, so this reading is its capacity:
    // the later "drained" baseline.
    emptyWriteAvailable_ = stream_.availableForWrite();
  }

  int readByte() override { return stream_.read(); }

  size_t writeBytes(const uint8_t* bytes, size_t length) override {
    // Clamp to free buffer space so Stream::write() cannot block.
    const int room = stream_.availableForWrite();
    if (room <= 0 || length == 0) {
      return 0;
    }
    size_t accepted = static_cast<size_t>(room);
    if (accepted > length) {
      accepted = length;
    }
    return stream_.write(bytes, accepted);
  }

  bool transmitDrained() const override {
    return stream_.availableForWrite() >= emptyWriteAvailable_;
  }

  void setTransmitEnable(bool enabled) override {
    if (txenPin_ != kNoTxenPin) {
      digitalWrite(txenPin_, enabled ? HIGH : LOW);
    }
  }

  uint32_t byteDurationMicros() const override {
    // Ceiling: never under-report a character time.
    return (static_cast<uint32_t>(bitsPerChar_) * 1000000u + baud_ - 1) /
           baud_;
  }

 private:
  Stream& stream_;
  int txenPin_;
  uint32_t baud_;
  uint8_t bitsPerChar_;
  int emptyWriteAvailable_ = 0;
};

}  // namespace CMRInet

#endif  // ARDUINO
