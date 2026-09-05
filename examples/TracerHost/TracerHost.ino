// TracerHost.ino — Xiao Host R&D image 
// CMRIHost on the cpNode-Xiao RS-485 board, command-and-control
// over USB CDC, onboard OLED display. 
// This uses the same engine and test harness listeners as the desktop
// tracer (extras/desktop/cmri_tracer.cpp) via testbed/TracerShell.h;
// only this main()/loop() differs.
//
// This file contains the sketch's core engine objects, with implementation
// in the other files:
//   tracerconsole.h/.cpp the C&C surface: CDC byte I/O, verb framing,
//                       ring-buffer capture mode, and the
//                       local verb dispatch table. Built on
//                       capture.h's TracerCapture as a private detail.
//   display.h/.cpp     the OLED status panel -- behavior
//   generators.h/.cpp  the walker/toggle/stall stimulus -- behavior
// Behavior modules are callable with no knowledge that C&C exists;
// the console is callable with no knowledge of what a verb does,
// only who to call.

// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
//   D4 - SDA  I2C (expanders + OLED)
//   D5 - SCL  I2C (expanders + OLED)
// 
// Host RS422 (4-wire) wiring: 
//   T± on the poll pair routes to the Node chain's R± pair, 
//   R± on the reply pair coming from the Node chain's T± pair. 
// The Host/Node connection is a crossover cable.
// The Nodes are all daisy-chained T± to T±, R± to R±.
//
// Host RS485 (4-wire) wiring:
//   Exactly the same as RS422, but in ONE LOCATION the T± 
//   and R± pairs are connected (T+ to R+, T- to R-) together.
//
// Host RS485 (2-wire) wiring:
//   A+/B- is daisy chained to all Nodes' A+/B- pairs. 
// The Host/Node connection is a straight-through cable.
//
// In all cases, the "+" in a pair must connect to "+" and "-" to "-". 
// Reversing them will cause CMRI bus communication failures.

#include <Arduino.h>

#include "CMRIHost.h"
#include "transport/serial.h"
#include "transport/serialESP32.h"
#include "testbed/TracerShell.h"
#include "testbed/CdcLineWriter.h"  // #99: shared, testable CDC line writer

#include "tracerconsole.h"
#include "display.h"
#include "generators.h"

// Build-time knobs for the CMRI bus transport.
// The USB/CDC link tis fixed at 115200 in setup()
// CMRI_BAUD is the RS422/485 wire speed to the Nodes.
// Override with build.defines, NOT build.extra_flags 
// (esp32 core owns build.extra_flags; overriding it 
// will drop -DARDUINO_USB_CDC_ON_BOOT=1)
#ifndef CMRI_BAUD
#define CMRI_BAUD 28800
#endif

// Inter-byte timeout: 50 ms tolerant override. 
// The decoder measures gaps at tick granularity,
// and the ESP32-C6 runtime stalls up to ~32 ms every ~2 s when
// WiFi/OTA is enabled.  These arrival gaps are not wire gaps, 
// as they are caused by the single core being preempted for radio work.
// The protocol's 250 ms Poll/Reply gate remains the genuine truncation 
// guard. 

#ifndef CMRI_INTER_BYTE_TIMEOUT_MS
#define CMRI_INTER_BYTE_TIMEOUT_MS 50  // 0 disables (interop 2.2.6 exception)
#endif

namespace {

constexpr const char* kImage = "tracer_host";
constexpr const char* kVersion = "0.12.0"; 

CMRInet::Esp32SerialPort port(Serial1, D3, CMRI_BAUD, RX, TX);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::testbed::TracerShell engine;
TracerDisplay hostDisplay;
TracerConsole tracerConsole;

}  // namespace

void setup() {
  Serial.begin(115200);  
  // USB CDC: the command-and-control stream
  // R&D image: wait for the C&C stream so the epoch line — and every
  // line after it — is captured. 
  // Bench order: open the port first, then (re)power the board. 
  // But bound the wait at 3S so a headless board still boots and 
  // drives the OLED / polls the bus without hanging. 
  // On the Xiao ESP32-C6, `Serial` is native USB CDC and only reads
  // true once a host asserts DTR/RTS; without a bound the wait parks
  // here forever and loop() never runs.
  const uint32_t kSerialWaitMs = 3000;
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < kSerialWaitMs) {
    delay(10);
  }
  // Non-blocking CDC writes: with the default TX timeout
  // The problem: 
  //    Serial.write() stalls when a cable is plugged in 
  //    but nothing has yet opened the port (DTR not asserted):
  //    the ring buffer fills and nothing drains it. 
  // This starves loop() and the OLED freezes. 
  // The fix: use Espressif's HWCDC workaround
  //   - setTxTimeoutMs(0) makes writes discard-and-return when no
  //     host is reading instead of blocking. 
  //   - Use a larger TX buffer to reduce the drop rate when a
  //     host IS reading but slowly.
  Serial.setTxTimeoutMs(0);
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(1024);
#endif

  // The CMRI wire is configured by Esp32SerialPort::begin() (baud,
  // 8N2, RX/TX pins) when host.begin()/lazyBegin() runs. Do not call
  // Serial1.begin() here — a second begin with different args races the
  // port and can leave the UART unusable.
  // Tick-gap tolerance; see the header comment. Survives begin().
  transport.setInterByteTimeoutMs(CMRI_INTER_BYTE_TIMEOUT_MS);

  // OLED diagnostic display (#11); degrades to headless on failure.
  hostDisplay.begin();

  // Node membership is C&C-driven (probes send `node add`).
  // a probe owns the bench view and can delete/re-add.
  tracer_generators::begin();

  // Bind unconditionally: there are no added nodes yetto invalidate
  tracerConsole.bind(host, engine, hostDisplay);
  engine.bind(host, transport, kImage, kVersion,
              &CMRInet::testbed::writeCdcLineCb, &tracerConsole);
  engine.setStatusExtender(tracer_generators::writeStatusItem,
                           tracer_generators::serviceCount(), nullptr);
  const uint32_t nowMs = millis();
  char bootMs[16];
  engine.setNow(nowMs);
  snprintf(bootMs, sizeof(bootMs), "%lu", static_cast<unsigned long>(nowMs));
  engine.emitEpoch("bootMs", bootMs);
}

void loop() {
  const uint32_t nowMs = millis();
  host.tick(nowMs);
  tracerConsole.tick(nowMs);
  hostDisplay.render(host, nowMs);
  hostDisplay.service();
}
