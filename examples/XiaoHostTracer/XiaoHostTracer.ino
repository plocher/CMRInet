// XiaoHostTracer.ino — the stage-2 Xiao Host R&D image (map issue
// #21): CMRIHost on the cpNode-Xiao RS-485 block, command-and-control
// over USB CDC. Same engine, same listeners as the desktop tracer
// (extras/desktop/cmri_tracer.cpp) via testbed/TracerShell.h;
// only this main() differs.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
// Host-seat wiring per docs/testbed-physical-notes.md: T± on the poll
// pair, R± on the reply pair. The Host/Node inversion is entirely in
// the crossover cable — this pinout is identical to a node's.
//
// No OLED, no OTA, no WiFi in this image (display is #11): stage 2
// validates TXEN and real-wire timing on a chip whose radio shares
// the die, so anomalies must be attributable to the wire.
//
// Inter-byte timeout: 50 ms tolerant override (stage-2 bench finding,
// wire-tap verified). The decoder measures gaps at tick granularity,
// and the ESP32-C6 runtime stalls up to ~32 ms every ~2 s — arrival
// gaps are not wire gaps, the same artifact class as stage 1's USB
// chunking. The wire itself measured 100% gapless. The 250 ms reply
// gate remains the genuine truncation guard. Be strict in what you
// send, forgiving in what you accept.
//
// C&C: verbs on the CDC stream (quiesce | resume | status |
// setbit <n> <0|1> | writeoutputs <hex> | forcetx | quit),
// JSON lines back. After quit the image emits "final" and parks;
// reset the board to run again.

#include <Arduino.h>

#include "CMRIHost.h"
#include "SerialCMRITransport.h"
#include "testbed/TracerShell.h"

#include "Esp32UartCMRISerialPort.h"

// Build-time knobs, overridable from a CLI build — e.g. the wrong-UA
// negative test:
//   --build-property "build.defines=-DTRACER_ADDRESS=31"
// (build.defines, NOT build.extra_flags: the esp32 core composes
// build.extra_flags itself, and overriding it clobbers the board's
// -DARDUINO_USB_CDC_ON_BOOT=1 — which would silently move Serial off
// the USB CDC console this image depends on.)
#ifndef TRACER_ADDRESS
#define TRACER_ADDRESS 30     // node address; wire UA = address + 65
#endif
#ifndef TRACER_INPUT_BYTES
#define TRACER_INPUT_BYTES 7  // bench node: 2 phantom CPNODE + 5 IOX IN
#endif
#ifndef TRACER_OUTPUT_BYTES
#define TRACER_OUTPUT_BYTES 7  // bench node: 2 phantom CPNODE + 5 IOX OUT
#endif
#ifndef TRACER_BAUD
#define TRACER_BAUD 28800
#endif
#ifndef TRACER_INTER_BYTE_TIMEOUT_MS
#define TRACER_INTER_BYTE_TIMEOUT_MS 50  // 0 disables (interop 2.2.6 exception)
#endif

namespace {

constexpr const char* kImage = "xiao_host_tracer";
// 0.1.1: hardware transmit-drain truth (Esp32UartCMRISerialPort) — the
// ~2 s C6 runtime stall made the estimate-based drain drop TXEN mid-ETX.
// 0.1.2: 50 ms inter-byte tolerance — the same stall splits intact
// replies at the tick level; the rate-derived timeout misread the gap.
// 0.1.3 (#27): Esp32UartCMRISerialPort promoted from this sketch into
// the library (src/); the library's inter-byte abort doctrine now
// ships a tolerant default (Design D13). This image keeps its explicit
// 50 ms override, so runtime behavior is unchanged from 0.1.2.
// 0.2.0: I/T bench slice (map issue #30) — output image via
// TRACER_OUTPUT_BYTES, onTrace packet telemetry, and output verbs
// (setbit/writeoutputs/forcetx) so T is exercisable from the bench.
constexpr const char* kVersion = "0.2.0";
constexpr int kTxenPin = D3;  // specific to the cpNode-Xiao board

CMRInet::Esp32UartCMRISerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                     TRACER_BAUD);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::RemoteNodeHandle* node = nullptr;
CMRInet::testbed::TracerShell engine;

bool finished = false;  // quit latched: "final" emitted, polling parked

void writeCdcLine(void* /*context*/, const char* line) {
  Serial.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  Serial.write('\n');
}

/// One newline-terminated verb from non-blocking CDC input. CRs are
/// dropped so a terminal sending CRLF works. Returns false when no
/// complete line is waiting.
bool readVerb(char* out, size_t len) {
  static char buffer[128];
  static size_t used = 0;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      buffer[used] = '\0';
      snprintf(out, len, "%s", buffer);
      used = 0;
      return true;
    }
    if (used < sizeof(buffer) - 1) {
      buffer[used++] = c;
    }
  }
  return false;
}

}  // namespace

void setup() {
  Serial.begin(115200);  // USB CDC: the command-and-control stream
  // R&D image: wait for the C&C stream so the epoch line — and every
  // line after it — is captured. Bench order: open the port first,
  // then (re)power the board.
  while (!Serial) {
    delay(10);
  }

  // The CMRI wire: 28800 8N2 on the MAX3491 UART pins.
  Serial1.begin(TRACER_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  // Tick-gap tolerance; see the header comment. Survives begin().
  transport.setInterByteTimeoutMs(TRACER_INTER_BYTE_TIMEOUT_MS);

  CMRInet::RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = TRACER_INPUT_BYTES;
  nodeConfig.outputBytes = TRACER_OUTPUT_BYTES;
  host.addRemoteNode(TRACER_ADDRESS, nodeConfig);

  if (host.configStatus() == CMRInet::CMRIHost::ConfigStatus::kOk) {
    engine.bind(host, transport, *host.node(TRACER_ADDRESS), kImage, kVersion,
                writeCdcLine, nullptr);
  }
  if (host.begin() != CMRInet::CMRIHost::ConfigStatus::kOk) {
    // Configuration rejected: report forever rather than run silent.
    for (;;) {
      Serial.print(F("{\"event\":\"fatal\",\"error\":\"addRemoteNode "
                     "rejected the configuration: "));
      Serial.print(CMRInet::configStatusString(host.configStatus()));
      Serial.println(F("\"}"));
      delay(1000);
    }
  }

  engine.setNow(millis());
  char bootMs[16];
  snprintf(bootMs, sizeof(bootMs), "%lu",
           static_cast<unsigned long>(millis()));
  engine.emitEpoch("bootMs", bootMs);
}

void loop() {
  const uint32_t nowMs = millis();
  engine.setNow(nowMs);
  if (!finished) {
    host.tick(nowMs);
  }

  char verb[128];
  if (readVerb(verb, sizeof(verb))) {
    using VerbResult = CMRInet::testbed::TracerShell::VerbResult;
    if (engine.handleVerb(verb) == VerbResult::kQuit && !finished) {
      engine.emitLine("final");
      finished = true;  // reset the board to run again
    }
  }
}
