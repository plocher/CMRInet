// SimpleHost.ino — a user-facing CMRInet Host example.
//
// The front-door example for the Host side of this library.
// This sketch is logically equivalent to JMRI (or Bruce's QBasic code)
// for the LOGICAL view of things.  The other side - Nodes - connect to
// the various PHYSICAL sensors and actuators (turnouts, occupancy,
// signals, etc)
// The other Host examples (XiaoHostTracer, XiaoSniffer) are bench test
// instruments built for orchestration and test harnesses; this one is
// for a person controlling a layout.
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
// You read inputs / write outputs through the host's node handle using
// host.node(address)->inputBit(n) and 
// host.node(address)->setOutputBit(n, [0,1])

// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex).
//   D7 - RX    CMRI RS485 receive
//   D6 - TX    CMRI RS485 transmit
//   D3 - TXEN  RS485 transmit enable
//   D4 - SDA   I2C (optional for the OLED status panel)
//   D5 - SCL   I2C (optional for the OLED status panel)
//
// RS485 bus Wiring: 
//         T± on the Host routes to the Node's R± and
//         R± on the Host routes to the Node's T±
//
// Two nodes are referenced by this sketch: UA 30 and UA 31, with all the examples
// using node 30.  If you only have one node, things still work - offline nodes
// do not stall the host.
//
// We use the OLED (SSD1306 128x64 @ 0x3C) to show health and status info
// as an example, you can use the display for any purpose.

#include <Arduino.h>
#include "CMRInet.h"                 // CMRIHost, RemoteNodeHandle, SerialCMRITransport
#include "Esp32UartCMRISerialPort.h" // ESP32 hardware transmit-drain port

// ---- OLED (optional) 
#define USE_OLED 1

#if USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;   // redraw interval

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
#endif

// ---- helpers
// bitOf(byte, bit) is the flat bit index for byte `byte`, bit `bit`.
// Bit 0 is the least significant bit of the byte.
constexpr size_t bitOf(size_t byte, size_t bit) { return byte * 8 + bit; }

// ---- Sketch specific details
constexpr int     kCMRI_BAUD = 28800;

// Behavior:
//   1. A walking one steps through all 8 bits of one byte,
//      one step per second, looping. If the LEDs light up in sequence,
//      the full poll/T/R path works.
//   2. A fast bit walker, one step per 100ms
//   3. An output toggle on each rising edge of an input. This shows
//      the input side: read a pushbutton or block detector, act on it.
//
//   Expand these to control your own test harness — drive turnouts, signals,
//   panel lamps from the output bits, and react to input edges from
//   block detectors, pushbuttons, and turnout feedback.
//
// On the cpNode-Xiao, which has no onboard I/O, bytes 0 and 1 are currently
// 0 on input and ignored on output. IO expander I/O starts at byte 2.
//
// Outputs are active-low by default in the cpNode sketch, so a looped
// output reads back inverted (out 1 => in 0).
//
// Note: inputBit(n) returns false for bits beyond the node's input image
// with no error. A trigger bit past the last input byte silently never
// fires. Keep kTriggerInBit within node 30's input range (bits 0..55 for
// 7 input bytes).

constexpr uint32_t kBitwalkPeriodMs = 1000;  // one step per second
constexpr size_t   kBitwalkByte     = 5;      // the byte to slow walk
constexpr uint32_t kFastBitwalkPeriodMs = 250;  // faster
constexpr size_t   kFastBitwalkByte = 3;      // the byte to fast walk

// Input toggle demo: toggle this output on each rising edge of the trigger.
constexpr size_t kTriggerInBit = bitOf(6, 0);  // last IOX port B, 
constexpr size_t kToggleOutBit = bitOf(4, 0);  // output on a different IOX expander

// ---- Per-node info: everything the sketch knows about each node ------
// Add or remove rows to match your layout. 
struct NodeInfo {
  uint8_t  address;
  uint16_t inputBytes;
  uint16_t outputBytes;
};

NodeInfo nodeTable[] = {
  {30,  7,  7},   // Node 30: 2 onboard phantom + 1 IOX32 + 3 IOX boards, all configured as 8IN/8OUT per expander
  {31,  4,  4},   // Node 31: 2 onboard phantom + 1 IOX board
};
constexpr size_t kNodeCount = sizeof(nodeTable) / sizeof(nodeTable[0]);

// Bitwalk state: which bit (0-7) is currently lit, and when it last
// advanced.
uint8_t  bitwalkStep  = 0;
uint32_t lastBitwalkMs = 0;
uint8_t  fastBitwalkStep  = 0;
uint32_t lastFastBitwalkMs = 0;

// Input-toggle state. RemoteNodeHandle reports the current input value
// only; the sketch keeps the previous value to detect a rising edge.
bool lastTriggerIn = false;




#if USE_OLED
bool oledOk = false;
uint32_t lastDisplayMs = 0;

// Forward declaration: host is defined in the wiring block below, but
// drawHostStatus references it here.
extern CMRInet::CMRIHost host;

/// Two-letter state tag for the OLED line.
const char* stateTag(CMRInet::RemoteNodeState s) {
  switch (s) {
    case CMRInet::RemoteNodeState::kOnline:        return "ON ";
    case CMRInet::RemoteNodeState::kStale:         return "OLD";
    case CMRInet::RemoteNodeState::kOffline:       return "OFF";
    case CMRInet::RemoteNodeState::kUninitialized: return "---";
  }
  return "??";
}

/// Draw the host status panel: a header and one line per node with its
/// address and health. Redrawn on a timer; no dirty tracking.
void drawHostStatus() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("HOST"));
  display.setTextSize(1);
  for (size_t i = 0; i < kNodeCount; ++i) {
    display.setCursor(0, 20 + i * 10);
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].address);
    const char* tag = (n != nullptr) ? stateTag(n->state()) : "---";
    display.printf(PSTR("UA %u: %s"), nodeTable[i].address, tag);
  }
  display.display();
}
#endif

// ---- Host wiring (static/stack; the library never allocates) --------------
CMRInet::Esp32UartCMRISerialPort port(Serial1, UART_NUM_1, D3, 28800);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRIHost               host(transport);

/// Host event listener. The engine fires this when a reply is rejected,
/// when a poll times out, or when a node changes health state. This is
/// the seam a real layout host uses to react to health changes.
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

void setup() {
  Serial.begin(115200);  // USB CDC: diagnostic output for rejections

  // The CMRI wire: 28800 8N2 on the MAX3491 UART pins.
  Serial1.begin(kCMRI_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  transport.setInterByteTimeoutMs(50);  // tolerant

#if USE_OLED
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
  }
#endif

  // Register the listener 
  host.onEvent(onHostEvent);

  // Register each node from the table. 
  // begin() returns the configuration status. 
  // A rejected add records its failure reason in host.configStatus()
  for (size_t i = 0; i < kNodeCount; ++i) {
    host.addRemoteNode(nodeTable[i].address,
                       nodeTable[i].inputBytes,
                       nodeTable[i].outputBytes);
  }
  if (host.begin() != CMRInet::CMRIHost::ConfigStatus::kOk) {
    // ****** FAILURE ****** //
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
      delay(1000);  // HALT: User needs to fix the configuration table and reflash
    }
  }
}

void loop() {
  const uint32_t now = millis();
  host.tick(now);  // advance the poll schedule; non-blocking

  // Drive the behavior through the target node's handle.
  CMRInet::RemoteNodeHandle* node = host.node(30);

  if (node != nullptr &&
      node->state() == CMRInet::RemoteNodeState::kOnline) {
    // Bitwalk: every kBitwalkPeriodMs, set one bit of byte kBitwalkByte,
    // clearing the rest. The unlit bit advances through 0..7 and loops.
    if (now - lastBitwalkMs >= kBitwalkPeriodMs) {
      node->setOutputBit(bitOf(kBitwalkByte, bitwalkStep), true);
      if (bitwalkStep > 0) {
        node->setOutputBit(bitOf(kBitwalkByte, bitwalkStep - 1), false);
      } else if (lastBitwalkMs != 0) {
        // Wrap from step 0: clear bit 7 from the previous cycle.
        node->setOutputBit(bitOf(kBitwalkByte, 7), false);
      }
      bitwalkStep = (bitwalkStep + 1) % 8;
      lastBitwalkMs = now;
    }
    // Fast Bitwalk: every kFastBitwalkPeriodMs, clear one bit of byte kFastBitwalkByte,
    // setting the rest. The lit bit advances through 0..7 and loops.
    if (now - lastFastBitwalkMs >= kFastBitwalkPeriodMs) {
      node->setOutputBit(bitOf(kFastBitwalkByte, fastBitwalkStep), false);
      if (fastBitwalkStep > 0) {
        node->setOutputBit(bitOf(kFastBitwalkByte, fastBitwalkStep - 1), true);
      } else if (lastFastBitwalkMs != 0) {
        // Wrap from step 0: clear bit 7 from the previous cycle.
        node->setOutputBit(bitOf(kFastBitwalkByte, 7), true);
      }
      fastBitwalkStep = (fastBitwalkStep + 1) % 8;
      lastFastBitwalkMs = now;
    }
    // Toggle an output on each rising edge of an input.
    const bool in0 = node->inputBit(kTriggerInBit);
    if (in0 && !lastTriggerIn) {
      node->setOutputBit(kToggleOutBit,
                         !node->outputBit(kToggleOutBit));
    }
    lastTriggerIn = in0;
  }

#if USE_OLED
  // Redraw screen on a timer. 
  if (now - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = now;
  }
#endif
}

