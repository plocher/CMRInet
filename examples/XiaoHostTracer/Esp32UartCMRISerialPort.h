// Esp32UartCMRISerialPort.h — StreamCMRISerialPort with hardware
// transmit-drain truth for ESP32 UARTs.
//
// Bench finding (map issue #21, 2026-08-15, wire-tap verified): the
// ESP32-C6 Arduino runtime stalls every ~2 s. StreamCMRISerialPort's
// buffer-only drain answer plus the transport's wire-time estimate
// assume transmission proceeds promptly after acceptance; when the
// stall delays the UART, the estimate expires first and TXEN drops
// while the frame's final byte (ETX) is still shifting out — the
// receiver sees a corrupted tail. cpNode's Node-side discipline uses
// a blocking flush() (hardware truth) and measured 100% clean under
// the same stalls, confirming the discipline gap, not the stall, as
// the defect.
//
// This subclass answers transmitDrained() from the UART hardware:
// uart_wait_tx_done() with a zero timeout is a non-blocking poll that
// reports whether the TX FIFO and shift register are both empty. The
// transport still ANDs this with its wire-time estimate; TXEN now
// holds through any stall and drops as soon as the last stop bit has
// left the wire.
//
// VALIDATION: Interop v1.0 2.3.14: flush until the last byte leaves
// the shift register, then drop TXEN at once. StreamCMRISerialPort's
// own header directs cores with true TX-complete status to subclass
// for exactly this.

#pragma once

#if defined(ARDUINO) && defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <driver/uart.h>

#include "StreamCMRISerialPort.h"

/// StreamCMRISerialPort whose drain answer is the ESP32 UART's own
/// TX-done status instead of the software buffer level.
class Esp32UartCMRISerialPort : public CMRInet::StreamCMRISerialPort {
 public:
  /// `uartNum` names the hardware unit behind `stream` (UART_NUM_1
  /// for Serial1). All other parameters as StreamCMRISerialPort.
  Esp32UartCMRISerialPort(Stream& stream, uart_port_t uartNum, int txenPin,
                          uint32_t baud, uint8_t bitsPerChar = 11)
      : StreamCMRISerialPort(stream, txenPin, baud, bitsPerChar),
        uartNum_(uartNum) {}

  /// Hardware truth: true only when the TX FIFO and the shift
  /// register are empty. Zero timeout keeps this non-blocking
  /// (Design v1.0 D6).
  bool transmitDrained() const override {
    return uart_wait_tx_done(uartNum_, 0) == ESP_OK;
  }

 private:
  uart_port_t uartNum_;
};

#endif  // ARDUINO && ARDUINO_ARCH_ESP32
