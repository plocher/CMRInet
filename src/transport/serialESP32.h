// Esp32SerialPort.h — StreamSerialPort with hardware
// transmit-drain truth for ESP32 UARTs.
//
// The constructor takes a HardwareSerial (Serial1, Serial2, ...) and
// auto-detects the UART number by address comparison, so the sketch
// never sees UART_NUM_1 or calls Serial1.begin() — the port's begin()
// configures the UART with the baud, framing, and pin mapping the
// sketch passed at construction.
//
// Origin: born sketch-local in examples/XiaoHostTracer during the #21
// bench (wire-tap verified), promoted into src/ by #27 because
// hardware TX-complete truth is the correct shipped behavior for every
// ESP32 target, not an R&D-only fix. Promotion is a relocation: the
// behavior is unchanged from the #21 bench pass.
//
// Bench finding (#21, 2026-08-15, wire-tap verified): the ESP32-C6
// Arduino runtime stalls every ~2 s. StreamSerialPort's buffer-only
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
// pay nothing (linker-drop, same as StreamSerialPort).
//
// VALIDATION: Interop v1.1 2.3.14: flush until the last byte leaves the
// shift register, then drop TXEN at once. transmitDrained() here is the
// hardware-truth half of the transport's two-gate drain detector; see
// the seam contract on SerialPort::transmitDrained().

#pragma once

#if defined(ARDUINO) && defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <driver/uart.h>

#include "serialStream.h"

namespace CMRInet {

/// StreamSerialPort whose drain answer is the ESP32 UART's own
/// TX-done status instead of the software buffer level. The transport's
/// wire-time estimate still ANDs with this answer (Design v1.1 D13: the
/// estimate never outlives a real drain, so the conjunction costs
/// nothing and covers ports that are optimistic by ignorance).
///
/// The constructor takes a HardwareSerial and auto-detects the UART
/// number (&Serial1 → UART_NUM_1, etc.), so the sketch never passes
/// UART_NUM_1 or calls Serial1.begin(). The port's begin() configures
/// the UART with the baud, framing, and pin mapping stored at
/// construction.
class Esp32SerialPort : public StreamSerialPort {
 public:
  /// Construct the port. `stream` is the HardwareSerial (Serial1,
  /// Serial2, ...); the UART number is auto-detected from it.
  /// `txenPin`: RS-485 driver-enable pin. `baud`: line rate.
  /// `rxPin`/`txPin`: UART pin mapping (-1 = core default).
  /// `config`: UART framing (SERIAL_8N2 default). `bitsPerChar`:
  /// bit times per character for byteDurationMicros() (11 for 8N2,
  /// 10 for 8N1; must match `config`).
  Esp32SerialPort(HardwareSerial& stream, int txenPin, uint32_t baud,
                   int rxPin = -1, int txPin = -1,
                   uint32_t config = SERIAL_8N2,
                   uint8_t bitsPerChar = 11)
      : StreamSerialPort(stream, txenPin, baud, bitsPerChar),
        hwStream_(stream),
        rxPin_(rxPin),
        txPin_(txPin),
        uartBaud_(baud),
        uartConfig_(config) {
    if (&stream == &Serial1) {
      uartNum_ = UART_NUM_1;
#if defined(UART_NUM_2)
    } else if (&stream == &Serial2) {
      uartNum_ = UART_NUM_2;
#endif
    } else {
      uartNum_ = UART_NUM_0;
    }
  }

  /// Configure the UART, then delegate to the parent. The UART is
  /// initialized here (from setup() via the begin() chain), not in the
  /// constructor — so the sketch no longer calls Serial1.begin().
  void begin() override {
    hwStream_.begin(uartBaud_, uartConfig_,
                     static_cast<int8_t>(rxPin_),
                     static_cast<int8_t>(txPin_));
    StreamSerialPort::begin();
  }

  /// Hardware truth: true only when the TX FIFO and the shift register
  /// are both empty. Zero timeout keeps this non-blocking (Design v1.1
  /// D6: nothing in the library blocks). This is the permitted
  /// non-optimistic-by-design answer to the transmitDrained() seam
  /// contract.
  bool transmitDrained() const override {
    return uart_wait_tx_done(uartNum_, 0) == ESP_OK;
  }

 private:
  HardwareSerial& hwStream_;
  int rxPin_;
  int txPin_;
  uint32_t uartBaud_;
  uint32_t uartConfig_;
  uart_port_t uartNum_;
};

}  // namespace CMRInet

#endif  // ARDUINO && ARDUINO_ARCH_ESP32
