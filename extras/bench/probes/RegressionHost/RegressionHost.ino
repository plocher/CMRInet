// RegressionHost.ino — a probe scaffold for reproducing library-side
// bugs under controlled loop stress.
//
// Behaves identically to examples/SimpleHost/SimpleHost.ino when
// compiled with no probe defines (the regression baseline). This
// means the sketch is also a passive regression-safety check for the
// library: if a change breaks the baseline build here, it will break
// SimpleHost too.
//
// Individual regressions are activated by low-level DIAG_* CLI
// defines. Do not drive them by hand — the mapping from issue number
// to defines lives in the harness:
//
//   extras/bench/probes/regressions/run.sh <issue-number>
//   extras/bench/probes/regressions/REGISTRY.md
//
// The harness compiles this sketch with the right defines, uploads,
// captures the CDC stream, and runs the per-issue analyzer if one
// exists.
//
// Regression guards in this file are named
// REGRESSION_<issue-number>_<short-slug> so the source cross-references
// the tracker. Adding a new regression means:
//   1. add a `#if defined(...)` guard below (the DIAG_* signal)
//   2. add a case clause in run.sh (the issue → defines mapping)
//   3. add a section in REGISTRY.md (the human-readable spec)
//   4. (optional) drop an analyzer under regressions/analyzers/
//
// Currently mapped:
//
//   REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF
//     Signal: DIAG_FAKE_STALL_MS + DIAG_FAKE_STALL_PERIOD_MS both set
//     Effect: suppresses the OLED draw and injects delay(N) every P ms
//     Purpose: reproduce backoff-under-loop-stall (issue #47)
//
// Orthogonal observation channel (usable with any regression):
//
//   DIAG_TRACE
//     Registers an onTrace listener that logs every TX/RX packet with
//     a timestamp to USB CDC, and makes CDC writes non-blocking so the
//     trace stream cannot itself stall the host loop.

#include <Arduino.h>
#include "CMRInet.h"
#include "Esp32UartCMRISerialPort.h"

// ---- Regression guards (derived from DIAG_* CLI defines) ------------------

#if defined(DIAG_FAKE_STALL_MS) && defined(DIAG_FAKE_STALL_PERIOD_MS)
  // Issue #47: recurring blocking work in loop() prevents CMRIHost's
  // exponential poll-retry backoff from accumulating. Guard fires when
  // both magnitude and period are set on the CLI; suppresses the OLED
  // draw (so the draw path is not a confounder) and injects a plain
  // delay() at the specified cadence.
  #define REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF 1
#endif

// ---- OLED (identical to SimpleHost)
#define USE_OLED 1

#if USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;
constexpr uint32_t kErrorWindowMs     = 5000;

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
#endif

// ---- helpers
constexpr size_t bitOf(size_t byte, size_t bit) { return byte * 8 + bit; }

constexpr int kCMRI_BAUD = 28800;

constexpr uint32_t kBitwalkPeriodMs = 1000;
constexpr size_t   kBitwalkByte     = 5;
constexpr uint32_t kFastBitwalkPeriodMs = 250;
constexpr size_t   kFastBitwalkByte = 3;

constexpr size_t kTriggerInBit = bitOf(6, 0);
constexpr size_t kToggleOutBit = bitOf(4, 0);

struct NodeInfo {
  uint8_t  address;
  uint16_t inputBytes;
  uint16_t outputBytes;
};

NodeInfo nodeTable[] = {
  {30,  7,  7},   // Node 30: 2 onboard phantom + 1 IOX32 + 3 IOX boards
  {31,  4,  4},   // Node 31: 2 onboard phantom + 1 IOX board
};
constexpr size_t kNodeCount = sizeof(nodeTable) / sizeof(nodeTable[0]);

uint8_t  bitwalkStep  = 0;
uint32_t lastBitwalkMs = 0;
uint8_t  fastBitwalkStep  = 0;
uint32_t lastFastBitwalkMs = 0;
bool lastTriggerIn = false;

#if USE_OLED
bool oledOk = false;
uint32_t lastDisplayMs = 0;

extern CMRInet::CMRIHost host;

CMRInet::examples::HostStatusPanel panel;

const char* stateTag(CMRInet::RemoteNodeState s) {
  switch (s) {
    case CMRInet::RemoteNodeState::kOnline:        return "ON ";
    case CMRInet::RemoteNodeState::kStale:         return "OLD";
    case CMRInet::RemoteNodeState::kOffline:       return "OFF";
    case CMRInet::RemoteNodeState::kUninitialized: return "---";
  }
  return "??";
}

void drawHostStatus() {
  if (!oledOk) return;
#if REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF
  // #47 probe: skip the OLED draw entirely. The fake-stall injection
  // in loop() provides the recurring blocking work that the probe is
  // measuring; the draw path is suppressed so it is not a confounder.
  return;
#else
  const uint32_t now = millis();

  const uint32_t pollsSent = host.statistics().pollsSent;
  uint32_t nodeErrs[kNodeCount] = {};
  for (size_t i = 0; i < kNodeCount; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].address);
    nodeErrs[i] = (n != nullptr) ? n->statistics().errors : 0;
  }
  panel.sample(now, pollsSent, nodeErrs, kNodeCount);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("HOST"));
  display.setTextSize(1);
  char header[16];
  panel.headerText(header, sizeof(header), now);
  display.setCursor(60, 4);
  display.print(header);

  for (size_t i = 0; i < kNodeCount; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].address);
    const char* tag = (n != nullptr) ? stateTag(n->state()) : "---";
    const uint32_t latMs = (n != nullptr)
        ? n->statistics().lastTurnaroundMs : 0;
    char row[24];
    panel.nodeRowText(row, sizeof(row), now, i,
                      nodeTable[i].address, tag, latMs);
    display.setTextSize(1);
    display.setCursor(0, 20 + i * 10);
    display.print(row);
  }
  display.display();
#endif  // REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF
}
#endif  // USE_OLED

CMRInet::Esp32UartCMRISerialPort port(Serial1, UART_NUM_1, D3, 28800);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRIHost               host(transport);

void onHostEvent(void* /*context*/, const CMRInet::CMRIHostEvent& event) {
  if (event.type != CMRInet::CMRIHostEventType::kReplyRejected) return;
  Serial.print(F("REJECT: "));
  Serial.print(CMRInet::replyRejectReasonString(event.rejectReason));
  switch (event.rejectReason) {
    case CMRInet::ReplyRejectReason::kGeometryMismatch:
      Serial.print(F(" — expected "));
      Serial.print(event.node->inputLength());
      Serial.print(F(" input bytes, got "));
      Serial.print(event.replyLength);
      break;
    case CMRInet::ReplyRejectReason::kUaMismatch:
      Serial.print(F(" — polled UA "));
      Serial.print(event.node->ua());
      Serial.print(F(", got UA "));
      Serial.print(event.replyUa);
      break;
    case CMRInet::ReplyRejectReason::kMtMismatch:
      Serial.print(F(" — expected MT 'R', got MT 0x"));
      Serial.print(event.replyMt, HEX);
      break;
    default:
      break;
  }
  Serial.println();
}

#ifdef DIAG_TRACE
// Per-packet trace listener. Timestamp + TX/RX + UA + MT + length,
// one line per packet, to USB CDC. Enables offline analysis of the
// exact poll/reply timeline (used by the grid-sweep analyzer for #47).
// CDC is set non-blocking in setup() so this cannot itself stall
// host.tick().
void onDiagTrace(void* /*ctx*/, bool tx, const CMRInet::CMRIPacket& p) {
  Serial.print(F("PKT t="));
  Serial.print(millis());
  Serial.print(tx ? F(" TX") : F(" RX"));
  Serial.print(F(" ua="));
  Serial.print(p.ua);
  Serial.print(F(" mt="));
  Serial.print(static_cast<char>(p.mt));
  Serial.print(F(" len="));
  Serial.print(p.length);
  Serial.println();
}
#endif

void setup() {
  Serial.begin(115200);
#ifdef DIAG_TRACE
  Serial.setTxTimeoutMs(0);  // non-blocking CDC (see #46)
#endif

  Serial1.begin(kCMRI_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  transport.setInterByteTimeoutMs(50);

#ifdef DIAG_TRACE
  Serial.print(F("BOOT DIAG_TRACE=1"));
#if defined(DIAG_FAKE_STALL_MS) && defined(DIAG_FAKE_STALL_PERIOD_MS)
  Serial.print(F(" DIAG_FAKE_STALL_MS="));
  Serial.print(DIAG_FAKE_STALL_MS);
  Serial.print(F(" DIAG_FAKE_STALL_PERIOD_MS="));
  Serial.print(DIAG_FAKE_STALL_PERIOD_MS);
#endif
  Serial.println();
#endif

#if USE_OLED
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
#if REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF
    // Under a #47 probe run the runtime OLED draw is suppressed, so
    // write a splash that identifies the probe point (matches the CSV
    // row for this run) and leave it up for the duration.
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.print(F("#47"));
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print(F("stall="));
    display.print(DIAG_FAKE_STALL_MS);
    display.print(F("ms"));
    display.setCursor(0, 32);
    display.print(F("period="));
    display.print(DIAG_FAKE_STALL_PERIOD_MS);
    display.print(F("ms"));
    display.setCursor(0, 44);
    display.print(F("OLED draw off"));
    display.display();
#endif
  }
#endif

  host.onEvent(onHostEvent);
#ifdef DIAG_TRACE
  host.onTrace(onDiagTrace);
#endif

  for (size_t i = 0; i < kNodeCount; ++i) {
    host.addRemoteNode(nodeTable[i].address,
                       nodeTable[i].inputBytes,
                       nodeTable[i].outputBytes);
  }
  if (host.begin() != CMRInet::CMRIHost::ConfigStatus::kOk) {
#if USE_OLED
    if (oledOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print(F("addRemoteNode\nrejected:\n"));
      display.print(CMRInet::configStatusString(host.configStatus()));
      display.display();
    }
#endif
    for (;;) {
      delay(1000);
    }
  }
}

void loop() {
  const uint32_t now = millis();
  host.tick(now);

#if REGRESSION_47_BLOCKING_INTERFERES_WITH_BACKOFF
  // #47 probe: recurring blocking stall injected on its own timer,
  // decoupled from the display refresh cadence so the (magnitude,
  // period) grid can be swept via CLI -D defines without touching
  // this file.
  static uint32_t lastFakeStallMs = 0;
  if (now - lastFakeStallMs >= DIAG_FAKE_STALL_PERIOD_MS ||
      lastFakeStallMs == 0) {
    delay(DIAG_FAKE_STALL_MS);
    lastFakeStallMs = now;
  }
#endif

  CMRInet::RemoteNodeHandle* node = host.node(30);

  if (node != nullptr &&
      node->state() == CMRInet::RemoteNodeState::kOnline) {
    if (now - lastBitwalkMs >= kBitwalkPeriodMs) {
      node->setOutputBit(bitOf(kBitwalkByte, bitwalkStep), true);
      if (bitwalkStep > 0) {
        node->setOutputBit(bitOf(kBitwalkByte, bitwalkStep - 1), false);
      } else if (lastBitwalkMs != 0) {
        node->setOutputBit(bitOf(kBitwalkByte, 7), false);
      }
      bitwalkStep = (bitwalkStep + 1) % 8;
      lastBitwalkMs = now;
    }
    if (now - lastFastBitwalkMs >= kFastBitwalkPeriodMs) {
      node->setOutputBit(bitOf(kFastBitwalkByte, fastBitwalkStep), false);
      if (fastBitwalkStep > 0) {
        node->setOutputBit(bitOf(kFastBitwalkByte, fastBitwalkStep - 1), true);
      } else if (lastFastBitwalkMs != 0) {
        node->setOutputBit(bitOf(kFastBitwalkByte, 7), true);
      }
      fastBitwalkStep = (fastBitwalkStep + 1) % 8;
      lastFastBitwalkMs = now;
    }
    const bool in0 = node->inputBit(kTriggerInBit);
    if (in0 && !lastTriggerIn) {
      node->setOutputBit(kToggleOutBit,
                         !node->outputBit(kToggleOutBit));
    }
    lastTriggerIn = in0;
  }

#if USE_OLED
  if (now - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = now;
  }
#endif
}
