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

// ---- OLED (optional). SimpleDisplay mirrors TracerHost's TracerDisplay
// shape (examples/TracerHost/display.h): one class owning its own OLED
// state instead of loose globals.
#define USE_OLED 1

#if USE_OLED
#include "display.h"
SimpleDisplay hostDisplay;
#endif

// ---- Services
#include "Services.h"

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

// ---- Host wiring (static/stack; the library never allocates) --------------
CMRInet::Esp32SerialPort port(Serial1, D3, kCMRI_BAUD, RX /* D7 */, TX /* D6 */);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);

/// On rejection, print what the remote node sent for diagnosis.
void onHostEvent(void* /*context*/, const CMRInet::CMRIHostEvent& event) {
  if (event.type != CMRInet::CMRIHostEventType::kReplyRejected) return;
  const char* reason = CMRInet::replyRejectReasonString(event.rejectReason);
  switch (event.rejectReason) {
    case CMRInet::ReplyRejectReason::kGeometryMismatch:
      Serial.printf("REJECT: %s -- expected %u input bytes, got %u\n", reason,
                    static_cast<unsigned>(event.node->inputLength()),
                    static_cast<unsigned>(event.replyLength));
      break;
    case CMRInet::ReplyRejectReason::kWireUAMismatch:
      Serial.printf("REJECT: %s -- polled UA %u, got UA %u\n", reason,
                    static_cast<unsigned>(event.node->wireUA()),
                    static_cast<unsigned>(event.replyWireUA));
      break;
    case CMRInet::ReplyRejectReason::kMtMismatch:
      Serial.printf("REJECT: %s -- expected MT 'R', got MT 0x%02X\n", reason,
                    static_cast<unsigned>(event.replyMt));
      break;
    default:
      Serial.printf("REJECT: %s\n", reason);
      break;
  }
}

void setup() {
  Serial.begin(115200);

  transport.setInterByteTimeoutMs(50);

#if USE_OLED
  hostDisplay.begin();  // degrades to headless on failure
#endif

  host.onEvent(onHostEvent);

  bool anyRejected = false;
#if USE_OLED
  char errorMsg[8*32];   errorMsg[0] = '\0';
  char nodeErrorMsg[32]; nodeErrorMsg[0] = '\0';
#endif

  for (size_t i = 0; i < kNodeCount; ++i) {
    const CMRInet::CMRIHost::ConfigStatus st = host.addRemoteNode(nodeTable[i]);
    if (st != CMRInet::CMRIHost::ConfigStatus::kOk) {
      anyRejected = true;
      Serial.printf("ERROR: node[%u] UA=%u: %s\n", static_cast<unsigned>(i),
                    static_cast<unsigned>(nodeTable[i].UA),
                    CMRInet::configStatusString(st));
#if USE_OLED
      snprintf(nodeErrorMsg, sizeof(nodeErrorMsg), "\nUA %u: %s",
               static_cast<unsigned>(nodeTable[i].UA),
               CMRInet::configStatusString(st));
      strncat(errorMsg, nodeErrorMsg, sizeof(errorMsg) - strlen(errorMsg) - 1);
#endif
    }
  }
  if (anyRejected) {
    Serial.println(F("FATAL: one or more addRemoteNode calls had errors at setup(); halting."));
#if USE_OLED
    hostDisplay.showFatalError("Setup FAILED", errorMsg);
#endif
    for (;;) {
      delay(1000);
    }
  }
  host.begin();

  g_orchestrator.add(new BitWalkerService({.nodeUA = 30, .byte = 3, .startBit = 0, .bitsCount = 8, .periodMs = 1000, .inverted = true }));
  g_orchestrator.add(new BitWalkerService({.nodeUA = 30, .byte = 4, .startBit = 3, .bitsCount = 3, .periodMs = 500,  .inverted = true }));
  g_orchestrator.add(new BitWalkerService({.nodeUA = 31, .byte = 2, .startBit = 0, .bitsCount = 8, .periodMs = 250,  .inverted = false}));
  g_orchestrator.add(new InputToggleService({.inNodeUA  = 31, .inByte  = 2, .inBit  = 0,
                                              .outNodeUA = 30, .outByte = 6, .outBit = 1,
                                              .mode = InputToggleMode::kLevelFollow}));
}

void loop() {
  const uint32_t now = millis();
  host.tick(now);
  g_orchestrator.tick(host, now);

#if USE_OLED
  hostDisplay.render(host, nodeTable, kNodeCount, now);
  hostDisplay.service();
#endif
}
