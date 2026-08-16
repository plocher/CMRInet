// Esp32UartCMRISerialPort.h — StreamCMRISerialPort with hardware
// transmit-drain truth for ESP32 UARTs.
//
// Origin: born sketch-local in examples/XiaoHostTracer during the #21
// bench (wire-tap verified), promoted into src/ by #27 because
// hardware TX-complete truth is the correct shipped behavior for every
// ESP32 target, not an R&D-only fix. Promotion is a relocation: the
// behavior is unchanged from the #21 bench pass.
//
// Bench finding (#21, 2026-08-15, wire-tap verified): the ESP32-C6
// Arduino runtime stalls every ~2 s. StreamCMRISerialPort's buffer-only
// drain answer plus the transport's wire-time estimate assume
// transmission proceeds promptly after acceptance; when the stall
// delays the UART, the estimate expires first and TXEN drops while the
// frame's final byte (ETX) is still shifting out — the receiver sees a
// corrupted tail (`… 5f 50 ff`). cpNode's Node-side discipline uses a
// blocking flush() (hardware truth) and measured 100% clean under the
// same stalls, confirming the discipline gap, not the stall, as the
// defect.
//
// This subclass answers transmitDrained() from the UART hardware:
// uart_wait_tx_done() with a zero timeout is a non-blocking poll that
// reports whether the TX FIFO and shift register are both empty. The
// transport still ANDs this with its wire-time estimate; TXEN now
// holds through any stall and drops as soon as the last stop bit has
// left the wire.
//
// Platform guard: the whole file is inside #if defined(ARDUINO) &&
// defined(ARDUINO_ARCH_ESP32). That is a platform guard on a
// platform-specific port, not a feature #ifdef (Design v1.1 D7
// clarifies: platform guards on platform-specific ports are not feature
// toggles). It is the only mechanism the Arduino build model offers for
// a port that calls into the ESP-IDF UART driver. Header-only means
// non-ESP32 builds see an empty file and non-including ESP32 sketches
// pay nothing (linker-drop, same as StreamCMRISerialPort).
//
// VALIDATION: Interop v1.1 2.3.14: flush until the last byte leaves the
// shift register, then drop TXEN at once. transmitDrained() here is the
// hardware-truth half of the transport's two-gate drain detector; see
// the seam contract on CMRISerialPort::transmitDrained().

#pragma once

#if defined(ARDUINO) && defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <driver/uart.h>

#include "StreamCMRISerialPort.h"

namespace CMRInet {

/// StreamCMRISerialPort whose drain answer is the ESP32 UART's own
/// TX-done status instead of the software buffer level. The transport's
/// wire-time estimate still ANDs with this answer (Design v1.1 D13: the
/// estimate never outlives a real drain, so the conjunction costs
/// nothing and covers ports that are optimistic by ignorance).
class Esp32UartCMRISerialPort : public StreamCMRISerialPort {
 public:
  /// `uartNum` names the hardware unit behind `stream` (UART_NUM_1 for
  /// Serial1). All other parameters as StreamCMRISerialPort.
  Esp32UartCMRISerialPort(Stream& stream, uart_port_t uartNum, int txenPin,
                          uint32_t baud, uint8_t bitsPerChar = 11)
      : StreamCMRISerialPort(stream, txenPin, baud, bitsPerChar),
        uartNum_(uartNum) {}

  /// Hardware truth: true only when the TX FIFO and the shift register
  /// are both empty. Zero timeout keeps this non-blocking (Design v1.1
  /// D6: nothing in the library blocks). This is the permitted
  /// non-optimistic-by-design answer to the transmitDrained() seam
  /// contract.
  bool transmitDrained() const override {
    return uart_wait_tx_done(uartNum_, 0) == ESP_OK;
  }

 private:
  uart_port_t uartNum_;
};

}  // namespace CMRInet

#endif  // ARDUINO && ARDUINO_ARCH_ESP32
