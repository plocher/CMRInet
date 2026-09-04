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
// Two nodes are referenced by this sketch: UA 30 and UA 31, 
// If you only have one node, things still work - offline nodes
// do not stall the host.
//
// We use the OLED (SSD1306 128x64 @ 0x3C) in this example to 
// show bus and node status info

#include <Arduino.h>
#include <string.h>
#include "CMRInet.h"                 // provides CMRIHost, RemoteNodeHandle
#include "transport/serial.h"        // SerialCMRITransport
#include "transport/serialESP32.h"   // ESP32 hardware transmit-drain port

// ---- OLED (optional) 
#define USE_OLED 1

#if USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"          // pure display-metrics helpers
#include "Ssd1306SegmentedFlush.h"      // non-blocking OLED push

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 120;   // redraw interval
constexpr uint32_t kErrorWindowMs     = 5000; // rolling error count window (long enough that a transient burst stays visible)

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
CMRInet::examples::Ssd1306SegmentedFlush oledFlush(display, kScreenAddr);
#endif

// ---- Services
#include "src/Services.h"

// ---- Sketch specific details
constexpr int     kCMRI_BAUD = 28800;

// Behavior:
//   -  Bitwalkers loop through all 8 bits of one byte, changing one bit
//      value each step, looping. If the LEDs light up in sequence, the 
//      node is seeing and reacting to TRANSMIT packets.
//   -  Feedback Loops read an input bit and write its value to an output.
//      When the output changes in step with the inputm this shows
//      that node is seeing and responding to a POLL packet with its
//      RESPONSE, and the Host is successfully sending the info in a
//      TRANSMIT packet to the destination.

//   You can expand these services to control your own test harness,
//   drive turnouts, signals, and panel lamps from the output bits, and
//   react to input from block detectors, pushbuttons, and turnout feedback.

// Outputs are active-low by default in the cpNode sketch, so a looped
// output reads back inverted (out 1 => in 0).

// Note: inputBit(n) returns false for bits beyond the node's input image
// with no error. A trigger bit past the last input byte silently never
// fires.

// ---- Per-node info: sketch-local layout (not a shared library roster) ----
// Each row is a full add artifact: UA + node type + that type's INIT fields.
// type is the NDP letter: 'C' CPNODE, 'M' SMINI, 'N' USIC, 'X' SUSIC.
// For 'C': inputBytes/outputBytes are NI/NO; opts1/opts2 are I-body opts.
// For 'M': geometry is fixed 3/6 — inputBytes/outputBytes ignored on add;
//          ns is searchlight pairs; ct[0..5] used when ns > 0.
// For 'N'/'X': ns is 4-card sets; inputBytes/outputBytes are image sizes;
//              ct[0..ns-1] are card-type bytes.
struct NodeInfo {
  uint8_t  UA;
  char     type;             // 'C' | 'M' | 'N' | 'X'
  uint16_t inputBytes;       // C, N, X image NI (ignored for M)
  uint16_t outputBytes;      // C, N, X image NO (ignored for M)
  uint8_t  opts1;            // C only
  uint8_t  opts2;            // C only
  uint8_t  ns;               // M / N / X
  uint8_t  ct[16];           // M: up to 6; N/X: ns entries
};

// Add or remove rows to match your layout — plain for-loop, no helper lib.
NodeInfo nodeTable[] = {
  // UA, type, in, out, opts1, opts2, ns, ct...
  {30, 'C', 2 + 5, 2 + 5, 0, 0, 0, {0}},  // CPNODE + IOX expanders
  {31, 'C', 2 + 1, 2 + 1, 0, 0, 0, {0}},  // CPNODE + 1 IOX
};
constexpr size_t kNodeCount = sizeof(nodeTable) / sizeof(nodeTable[0]);

/// Register one table row with the Host using the typed add API.
CMRInet::CMRIHost::ConfigStatus addNodeFromInfo(
    CMRInet::CMRIHost& host, const NodeInfo& row) {
  using CMRInet::NodeType;
  NodeType ndp;
  if (!CMRInet::nodeTypeFromNdp(row.type, ndp)) {
    return CMRInet::CMRIHost::ConfigStatus::kUnsupportedNodeType;
  }
  switch (ndp) {
    case NodeType::kCpnode: {
      CMRInet::CpnodeInit init;
      init.inputBytes = row.inputBytes;
      init.outputBytes = row.outputBytes;
      init.opts1 = row.opts1;
      init.opts2 = row.opts2;
      return host.addRemoteNode(row.UA, init);
    }
    case NodeType::kSmini: {
      CMRInet::SminiInit init;
      init.ns = row.ns;
      if (row.ns > 0) {
        memcpy(init.ct, row.ct, CMRInet::SminiInit::kCtCount);
      }
      return host.addRemoteNode(row.UA, init);
    }
    case NodeType::kUsic:
    case NodeType::kSusic: {
      CMRInet::UsicFamilyInit init;
      init.ns = row.ns;
      init.inputBytes = row.inputBytes;
      init.outputBytes = row.outputBytes;
      if (row.ns > 0 && row.ns <= CMRInet::UsicFamilyInit::kMaxNs) {
        memcpy(init.ct, row.ct, row.ns);
      }
      return host.addRemoteNode(row.UA, ndp, init);
    }
  }
  return CMRInet::CMRIHost::ConfigStatus::kUnsupportedNodeType;
}

#if USE_OLED
bool oledOk = false;
uint32_t lastDisplayMs = 0;

// Forward declaration: host is defined in the wiring block below, but
// drawHostStatus references it here.
extern CMRInet::CMRIHost host;

// Shared status-panel logic (HostStatusPanel) owns the rolling metric
// state and formats the header + per-node row strings; this sketch just
// feeds it the current counters and renders the strings to the OLED.
CMRInet::examples::HostStatusPanel panel;

// The OLED state tag comes from CMRInet::remoteNodeStateTag(), beside
// the enum in RemoteNodeHandle.h. 
//
/// Draw the host status panel. Layout:
///   HOST           <cadence>      (alternates c/s ↔ ms/cycle every 5 s)
///   P:nnnn R:nnnn m:nn            (host totals this redraw / 5s misses)
///   UA30  165ms  12m  0e          (online: no redundant ON tag)
///   UA31 OFF  ---ms   5m  0e      (non-online keeps compact tag)
/// Misses (noReplies / reply-gate timeouts) are first-class; 
/// Redrawn on a timer; no dirty tracking.
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
  // Segmented I2C push — never call display.display() (full ~25 ms stall).
  oledFlush.markDirty();
}
#endif

// ---- Host wiring (static/stack; the library never allocates) --------------
CMRInet::Esp32SerialPort port(Serial1, D3, 28800, RX /* D7 */, TX /* D6 */);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRIHost               host(transport);

/// Host event listener. The engine fires this when a reply is rejected,
/// when a poll times out, or when a node changes health state. This is
/// the how the host sketch reacts to node health state changes.
///
/// On a rejection, print what the remote node actually sent so the user
/// can diagnose a misconfiguration without reflashing the node.
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
  Serial.begin(115200);  // USB CDC: diagnostic output for rejections

  // The CMRI wire is configured by Esp32SerialPort::begin() via host.begin().
  transport.setInterByteTimeoutMs(50);  // tolerant while still quick fail detect

#if USE_OLED
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
  }
#endif

  // Register the listener 
  host.onEvent(onHostEvent);

  // Register each node from the table. Each add reports its own outcome
  // this sketch keeps the first failure so it can name
  // a reason on the display.
  CMRInet::CMRIHost::ConfigStatus configStatus =
      CMRInet::CMRIHost::ConfigStatus::kOk;
  for (size_t i = 0; i < kNodeCount; ++i) {
    const CMRInet::CMRIHost::ConfigStatus st =
        addNodeFromInfo(host, nodeTable[i]);
    if (st != CMRInet::CMRIHost::ConfigStatus::kOk &&
        configStatus == CMRInet::CMRIHost::ConfigStatus::kOk) {
      configStatus = st;
    }
  }
  if (configStatus != CMRInet::CMRIHost::ConfigStatus::kOk) {
    // ****** FAILURE ****** //
#if USE_OLED
    if (oledOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print(F("addRemoteNode\nrejected:\n"));
      display.print(CMRInet::configStatusString(configStatus));
      oledFlush.markDirty();
      oledFlush.serviceUntilIdle();  // halt path: finish the frame now
    }
#endif
    for (;;) {
      delay(1000);  // HALT: User needs to fix the configuration table and reflash
    }
  }
  host.begin();

  // Add services to orchestrator
  // Example: Configure bitwalker on node 30 byte 3 bits 0-7 delay 1S
  /*                              UA  Byte bit count Delay invert */
  BitWalkerConfig bitwalker1 = {  30,  3,   0,   8,   1000, true, };
  g_orchestrator.add(new BitWalkerService(bitwalker1));
  BitWalkerConfig bitwalker2 = {  30,  4,   3,   3,   500,  true, };
  g_orchestrator.add(new BitWalkerService(bitwalker2));
  BitWalkerConfig bitwalker3 = {  31,  2,   0,   8,   250,  false, };
  g_orchestrator.add(new BitWalkerService(bitwalker3));

  // Example: Configure input toggle
  InputToggleConfig inputToggle = {
    31,        // inputUA
    2,         // inByte
    0,         // inBit
    30,        // outputUA
    6,         // outByte
    1,         // outBit
    false,     // lastValue
  };
  g_orchestrator.add(new InputToggleService(inputToggle));
}

void loop() {
  const uint32_t now = millis();
  host.tick(now);  // advance the poll schedule; non-blocking

  // Run all services through the orchestrator
  g_orchestrator.tick(host, now);

#if USE_OLED
  // Paint on a timer; drain one I2C chunk every loop so the push never
  // owns the Host schedule the way display.display() did.
  if (now - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = now;
  }
  oledFlush.service();
#endif
}

