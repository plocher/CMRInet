// XiaoBenchCal.ino — minimal 2-wire bench calibration sketch.
//
// Purpose: prove the wiring and tooling work end to end. The Host
// TXs a known I -> T -> P sequence to one node and reports everything
// it sees on RX through the shared TracerShell's onTrace listener.
//
// The OLED identifies this sketch as the operational Host on the bench
// (splash: "CAL", status: "HOST" + cadence + node row), distinct
// from the tracer ("TRC") and the sniffer ("SNIFFER").
//
// CAVEAT: this sketch has NOT been validated on a 2-wire
// (single-pair, full-duplex) bench. The bench currently runs
// 4-wire (two-pair) only; the wiring, bench.json roles, and
// calibrate.sh runner all assume the 4-wire topology. The
// sketch compiles and the engine runs, but the 2-wire echo path
// is presumed broken until the bench is reconfigured for one pair.
//
// On 2-wire (full-duplex), the Host would see its own TX echoed
// back on the RX pair. The trace lines would show the echo
// arriving alongside the outbound frames. Both sniffers on the
// same pair should see the same frames.
//
// On 4-wire, the Host does NOT see its own TX (separate pairs), so
// the trace shows outbound only — which itself tells you the wiring
// is 4-wire, not 2-wire.
//
// No generators, no ring buffer, no C&C verbs beyond what the shell
// provides by default. Just the engine, one node, OLED identity, and
// JSON trace output the bench capture scripts can read.
//
// STATUS: UNTESTED on 2-wire. Compiles and the engine runs, but
// the 2-wire echo path is presumed broken until the bench is
// reconfigured for single-pair full-duplex.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
//   D4 - SDA I2C Data
//   D5 - SCL I2C Clock
//
// Wiring per docs/testbed-physical-notes.md. On 2-wire, tie
// R+ to T+ and R- to T- at the terminal block.

#include <Arduino.h>

// Forward declarations: Arduino auto-generates prototypes at the
// top of every .ino file. Declaring them ourselves first makes
// the dependency explicit and keeps the auto-generated ones
// from surprising the build.
void drawSplash();
void drawStatus();

#include "CMRIHost.h"
#include "SerialCMRITransport.h"
#include "testbed/TracerShell.h"
#include "testbed/CdcLineWriter.h"  // #99: shared CDC line writer

#include "Esp32UartCMRISerialPort.h"

// OLED: identifies the sketch as the operational Host on the bench.
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"          // shared HostStatusPanel

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;
CMRInet::examples::HostStatusPanel panel;

// One calibration node. Override with:
//   --build-property "build.defines=-DCALIB_UA=31"
#ifndef CALIB_UA
#define CALIB_UA 30     // node UA; wire UA = UA + 65
#endif
#ifndef CALIB_INPUT_BYTES
#define CALIB_INPUT_BYTES 2  // small, so the I body is easy to read
#endif
#ifndef CALIB_OUTPUT_BYTES
#define CALIB_OUTPUT_BYTES 2
#endif
#ifndef CALIB_BAUD
#define CALIB_BAUD 28800
#endif
#ifndef CALIB_INTER_BYTE_TIMEOUT_MS
#define CALIB_INTER_BYTE_TIMEOUT_MS 50  // 0 disables (interop 2.2.6)
#endif

constexpr const char* kImage = "xiao_bench_cal";
constexpr const char* kVersion = "0.1.0";
constexpr int kTxenPin = D3;  // specific to the cpNode-Xiao board

CMRInet::Esp32UartCMRISerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                     CALIB_BAUD);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::testbed::TracerShell shell;

// The CDC console seam for writeCdcLine (#99): binds the shared
// src/testbed/CdcLineWriter.h logic to this sketch's Serial, millis,
// and delay. Identical to the tracer and sniffer sketches.
class XiaoCdcConsole : public CMRInet::testbed::CdcConsole {
 public:
  bool open() const override { return static_cast<bool>(Serial); }
  size_t availableForWrite() override { return Serial.availableForWrite(); }
  size_t write(const uint8_t* data, size_t n) override {
    return Serial.write(data, n);
  }
  uint32_t nowMs() override { return millis(); }
  void yieldMs() override { delay(1); }
};

XiaoCdcConsole cdc;

void drawSplash() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("CAL"));
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print(kVersion);
  display.setCursor(0, 40);
  display.print(F("polling..."));
  display.display();
}

void drawStatus() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("HOST"));
  display.setTextSize(1);
  char header[16];
  panel.headerText(header, sizeof(header), millis());
  display.setCursor(60, 4);
  display.print(header);
  const CMRInet::RemoteNodeHandle* node = host.node(CALIB_UA);
  const char* tag =
      (node != nullptr) ? CMRInet::remoteNodeStateTag(node->state()) : "---";
  const uint32_t latMs = (node != nullptr)
      ? node->statistics().lastTurnaroundMs : 0;
  char row[24];
  panel.nodeRowText(row, sizeof(row), millis(), 0,
                    CALIB_UA, tag, latMs);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print(row);
  display.display();
}

void setup() {
  Serial.begin(115200);  // USB CDC: the calibration report stream
  // R&D image: wait for the CDC stream so the epoch line is captured.
  // Bound at 3 s so a headless board still boots and polls without
  // hanging setup().
  const uint32_t kSerialWaitMs = 3000;
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < kSerialWaitMs) {
    delay(10);
  }
  Serial.setTxTimeoutMs(0);
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(1024);
#endif

  // OLED init: identifies this sketch on the bench display as the
  // operational Host (not the tracer or sniffer).
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  if (display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr)) {
    oledOk = true;
    display.dim(true);
    display.setTextWrap(false);
    drawSplash();
  } else {
    oledOk = false;
  }

  // The CMRI wire: 28800 8N2 on the MAX3491 UART pins.
  Serial1.begin(CALIB_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  transport.setInterByteTimeoutMs(CALIB_INTER_BYTE_TIMEOUT_MS);

  // One node. The engine sends I -> T -> P to it; on 2-wire the
  // Host sees all three echoed back on RX.
  CMRInet::RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = CALIB_INPUT_BYTES;
  nodeConfig.outputBytes = CALIB_OUTPUT_BYTES;
  host.addRemoteNode(CALIB_UA, nodeConfig);

  // Start the engine. Without this, tick() early-returns and the
  // engine never runs — the panel shows ---ms (no polls sent).
  host.begin();

  // Bind the shell: it registers the onTrace listener that emits
  // a JSON trace line for every TX and RX packet. No status
  // extender, no generators — just the trace stream.
  shell.bind(host, transport, kImage, kVersion,
             &CMRInet::testbed::writeCdcLineCb, &cdc);
  char bootMs[16];
  snprintf(bootMs, sizeof(bootMs), "%lu",
           static_cast<unsigned long>(millis()));
  shell.emitEpoch("bootMs", bootMs);
}

void loop() {
  // Tick the host. The engine runs I -> settle -> T -> gap -> P ->
  // reply-gate cycle. On 2-wire, each TX echoes back on RX and
  // the trace listener reports it. On 4-wire, only the TX
  // lines appear (no echo).
  shell.setNow(millis());
  host.tick(millis());
  if (oledOk && (millis() - lastDisplayMs) >= kDisplayRefreshMs) {
    drawStatus();
    lastDisplayMs = millis();
  }
}
