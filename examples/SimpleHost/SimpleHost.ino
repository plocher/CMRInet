// SimpleHost.ino — a user-facing CMRInet Host example.
//
// This sketch is logically equivalent to JMRI (or Bruce's QBasic code)
// for the LOGICAL view of things.  The other side - Nodes - connect to
// the various PHYSICAL sensors and actuators (turnouts, occupancy,
// signals, etc)
//
// The other Host examples (TracerHost, XiaoSniffer) are bench test
// instruments built for library test harnesses; this example is a
// tutorial focused on the basics.
//
// What it does: polls remote nodes, shows each node's health, and runs
// a simplistic behavior model — obligatory blinking lights and an
// example sensor input.
//
// CMRInet is a layout I/O data service. This Host sketch deals in input
// and output data, freshness, and node health, while Node sketches on
// other devices connect to layout hardware and provide the input data
// and process output.
//
// The CMRInet protocol is a polled data transfer implementation that
// connects a CMRIHost to remote CMRINodes over a transport; you register
// the remote nodes that this host manages by way of RemoteNodeHandles and
// you call host.tick() every loop to make the gears turn.
//
// The host reads inputs / write outputs through a node handle:
//   handle = host.node(node's UA)
//   handle->inputBit(n) and
//   handle->setOutputBit(n, [0,1])
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex).
//   D7 - RX    CMRI RS485 receive
//   D6 - TX    CMRI RS485 transmit
//   D3 - TXEN  RS485 transmit enable
//   D4 - SDA   I2C (optional for the OLED status panel)
//   D5 - SCL   I2C (optional for the OLED status panel)
//
// RS485 bus Wiring:
//   T± on the Host routes to the Node's R± and
//   R± on the Host routes to the Node's T±
//
// Two nodes are referenced by this sketch: UA 30 and UA 31.
// If you only have one node, things still work — offline nodes
// do not stall the host.
//
// We use the OLED (SSD1306 128x64 @ 0x3C) in this example to
// show bus and node status info.

#include <Arduino.h>
#include "CMRInet.h"                 // CMRIHost, HostNodeSpec, typed INIT
#include "transport/serial.h"        // SerialCMRITransport
#include "transport/serialESP32.h"   // ESP32 hardware transmit-drain port

// ---- OLED (optional)
#define USE_OLED 1

#if USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"
#include "Ssd1306SegmentedFlush.h"

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 120;
constexpr uint32_t kErrorWindowMs     = 10000;

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
CMRInet::examples::Ssd1306SegmentedFlush oledFlush(display, kScreenAddr);
#endif

// ---- Services
#include "src/Services.h"

// ---- Sketch specific details
constexpr int kCMRI_BAUD = 28800;

// Behavior:
//   - Bitwalkers loop through bits of one output byte. If LEDs light in
//     sequence, the node is seeing TRANSMIT packets.
//   - Feedback loops copy an input bit to an output bit. That shows P→R
//     and T working end to end.
//
// Outputs are active-low by default in the cpNode sketch, so a looped
// output reads back inverted (out 1 => in 0).
//
// Note: inputBit(n) returns false for bits beyond the node's input image
// with no error. A trigger bit past the last input byte silently never
// fires.

// ---- Layout table (sketch-local; not a shared library roster) ------------
// Each row is a HostNodeSpec: UA + NodeType + that type's INIT payload.
//
//   hostNodeCpnode(ua, CpnodeInit(NI, NO [, opts1 [, opts2]]))
//   hostNodeSmini (ua, SminiInit([ns]))
//   hostNodeUsic  (ua, UsicFamilyInit(ns, NI, NO))
//   hostNodeSusic (ua, UsicFamilyInit(ns, NI, NO))
//
// Edit rows to match your layout. Offline UAs do not stall the host.
using CMRInet::CpnodeInit;
using CMRInet::HostNodeSpec;
using CMRInet::hostNodeCpnode;

HostNodeSpec nodeTable[] = {
  // UA 30: CPNODE — 2 onboard + 5 IOX bytes in and out
  hostNodeCpnode(30, CpnodeInit(2 + 5, 2 + 5 /* o1=0, o2=0 */)),
  // UA 31: CPNODE — 2 onboard + 1 IOX byte in and out
  hostNodeCpnode(31, CpnodeInit(2 + 1, 2 + 1 /* o1=0, o2=0 */)),
  // Examples of other types (uncomment / edit as needed):
  hostNodeSmini(5, CMRInet::SminiInit(/*ns=*/0)),
  // hostNodeSusic(10, CMRInet::UsicFamilyInit(/*ns=*/1, /*NI=*/4, /*NO=*/4)),
};
constexpr size_t kNodeCount = sizeof(nodeTable) / sizeof(nodeTable[0]);

#if USE_OLED
bool oledOk = false;
uint32_t lastDisplayMs = 0;

extern CMRInet::CMRIHost host;

CMRInet::examples::HostStatusPanel panel;

/// Draw the host status panel.
void drawHostStatus() {
  if (!oledOk) return;
  const uint32_t now = millis();

  const auto& hs = host.statistics();
  uint32_t nodeErrs[kNodeCount] = {};
  uint32_t nodeMisses[kNodeCount] = {};
  for (size_t i = 0; i < kNodeCount; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].UA);
    if (n != nullptr) {
      nodeErrs[i] = n->statistics().errors;
      nodeMisses[i] = n->statistics().noReplies;
    }
  }
  panel.sample(now, hs.pollsSent, hs.repliesAccepted,
               nodeErrs, nodeMisses, kNodeCount);

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

  char totals[24];
  panel.hostTotalsText(totals, sizeof(totals), now);
  display.setCursor(0, 18);
  display.print(totals);

  for (size_t i = 0; i < kNodeCount; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].UA);
    const bool online =
        (n != nullptr) && (n->state() == CMRInet::RemoteNodeState::kOnline);
    const char* tag =
        (n != nullptr) ? CMRInet::remoteNodeStateTag(n->state()) : "---";
    const uint32_t latMs = (n != nullptr)
        ? n->statistics().lastTurnaroundMs : 0;
    char row[28];
    panel.nodeRowText(row, sizeof(row), now, i,
                      nodeTable[i].UA, online, tag, latMs);
    display.setCursor(0, 30 + static_cast<int>(i) * 12);
    display.print(row);
  }
  oledFlush.markDirty();
}
#endif

// ---- Host wiring (static/stack; the library never allocates) --------------
CMRInet::Esp32SerialPort port(Serial1, D3, kCMRI_BAUD, RX /* D7 */, TX /* D6 */);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);

/// On rejection, print what the remote node sent for diagnosis.
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
    case CMRInet::ReplyRejectReason::kWireUAMismatch:
      Serial.print(F(" — polled UA "));
      Serial.print(event.node->wireUA());
      Serial.print(F(", got UA "));
      Serial.print(event.replyWireUA);
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

void setup() {
  Serial.begin(115200);

  transport.setInterByteTimeoutMs(50);

#if USE_OLED
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
  }
#endif

  host.onEvent(onHostEvent);

  CMRInet::CMRIHost::ConfigStatus configStatus =
      CMRInet::CMRIHost::ConfigStatus::kOk;
  for (size_t i = 0; i < kNodeCount; ++i) {
    const CMRInet::CMRIHost::ConfigStatus st =
        host.addRemoteNode(nodeTable[i]);
    if (st != CMRInet::CMRIHost::ConfigStatus::kOk &&
        configStatus == CMRInet::CMRIHost::ConfigStatus::kOk) {
      configStatus = st;
    }
  }
  if (configStatus != CMRInet::CMRIHost::ConfigStatus::kOk) {
#if USE_OLED
    if (oledOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print(F("addRemoteNode\nrejected:\n"));
      display.print(CMRInet::configStatusString(configStatus));
      oledFlush.markDirty();
      oledFlush.serviceUntilIdle();
    }
#endif
    for (;;) {
      delay(1000);
    }
  }
  host.begin();

  // Services
  BitWalkerConfig bitwalker1 = {  30,  3,   0,   8,   1000, true, };
  g_orchestrator.add(new BitWalkerService(bitwalker1));
  BitWalkerConfig bitwalker2 = {  30,  4,   3,   3,   500,  true, };
  g_orchestrator.add(new BitWalkerService(bitwalker2));
  BitWalkerConfig bitwalker3 = {  31,  2,   0,   8,   250,  false, };
  g_orchestrator.add(new BitWalkerService(bitwalker3));

  InputToggleConfig inputToggle = {31, 2,   0,  30,  6,  1, false, };
  g_orchestrator.add(new InputToggleService(inputToggle));
}

void loop() {
  const uint32_t now = millis();
  host.tick(now);
  g_orchestrator.tick(host, now);

#if USE_OLED
  if (now - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = now;
  }
  oledFlush.service();
#endif
}
