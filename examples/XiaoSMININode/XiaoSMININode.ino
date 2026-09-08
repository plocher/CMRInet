// XiaoSMININode.ino
// A functional SMINI work-alike node for the
// cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491).
//
// CMRInet type 'M' (SMINI): 24 inputs, 48 outputs

//
// ---- Board wiring ----
// 5x cpNode-IOX-16 I2C expander boards.
//   D7 - RX    CMRI RS485 receive
//   D6 - TX    CMRI RS485 transmit
//   D5 - SCL   I2C (expanders + OLED)
//   D4 - SDA   I2C (expanders + OLED)
//   D3 - TXEN  RS422/485 transmit enable (hardware TXEN)
//   OLED live view (USE_OLED, on by default): expander bit grid
//   with r/t spinners. No OTA in this sketch.
//
// ---- RS485 bus wiring ----
//   R± on the Node routes to the Host's T±, and T± on the Node
//   routes to the Host's R±. Wire all Nodes the same way, TX to
//   TX and RX to RX, "+" in a pair to "+" and "-" to "-".
//
// ---- SMINI byte map ----
// All I/O lives on expanders.
//   Inputs (3 bytes):
//     bytes 0-1: 0x20 ports A/B
//     byte 2:    0x21 port A (port B unused)
//   Outputs (6 bytes):
//     bytes 0-1: 0x22 ports A/B
//     bytes 2-3: 0x23 ports A/B
//     bytes 4-5: 0x24 ports A/B
//

#if !defined(ARDUINO_ARCH_ESP32)
  #if defined(ARDUINO_ARCH_AVR)
    #error "For the AtMega328p ProMini target, use the ProMiniSMININode example."
  #endif
  #error "This sketch only works on an ESP32 (arduino:esp32)."
#endif

// =============================================
// ====   Feature toggles                   ====
// =============================================

// OLED live view. Comment out to run headless.
#define USE_OLED

#include <Arduino.h>
#include <Wire.h>

#include "CMRInet.h"                // CMRINode, CMRINodeConfig, IOBuffer
#include "transport/serial.h"       // SerialCMRITransport
#include "transport/serialESP32.h"  // Esp32SerialPort
#include "iox.h"                    // MCP23017 expander access
#ifdef USE_OLED
#include "display.h"  // SSD1306 live view
#endif

// =============================================
// ====   User configuration                 ====
// =============================================

#ifndef NODE_ID
#define NODE_ID 50  // 0...127; must match the Host's polled address
#endif

// CMRI bus line rate, 8N2 framing, same as the Host. Single source:
// the port constructor consumes it below — its begin() configures
// the UART with it, and the wire-time math runs from it.
#ifndef CMRI_BAUD
#define CMRI_BAUD 28800
#endif

#define CMRI_NODE_DESCRIPTION "xSMINI"
// The node's name: shown on the OLED header. Defaults to
// description-nodeID (e.g. "xSMINI-50"); override with a name of
// your own meaning here or from a CLI build
// (-DNODE_NAME='"yard-throat"').
#define STRINGIFY_(x)  #x
#define STRINGIFY(x)   STRINGIFY_(x)
#ifndef NODE_NAME
#define NODE_NAME      CMRI_NODE_DESCRIPTION "-" STRINGIFY(NODE_ID)
#endif


// MCP23017 expanders: {address, portA, portB}. IN ports pack in
// table order (port A before port B); OUT ports unpack the same
// way. A correct table yields the SMINI geometry: 3 input bytes
// and 6 output bytes.
IOX_Config expanders[] = {
    { 0x20, IN,  IN     },  // inputs:  bytes 0-1
    { 0x21, IN,  UNUSED },  // inputs:  byte 2 (port A only)
    { 0x22, OUT, OUT    },  // outputs: bytes 0-1
    { 0x23, OUT, OUT    },  // outputs: bytes 2-3
    { 0x24, OUT, OUT    },  // outputs: bytes 4-5
};

constexpr uint8_t kExpanderCount =
    static_cast<uint8_t>(sizeof(expanders) / sizeof(expanders[0]));
constexpr uint8_t kPortsPerExpander = 2;  // A, B

// =============================================
// ====   Plumbing                          ====
// =============================================

// Hardware TXEN on D3 (the proven XiaoNode line). The port's begin()
// configures Serial1 itself (baud, 8N2, pin map).
CMRInet::Esp32SerialPort 
    port(Serial1, /* TXEN */ D3, CMRI_BAUD, RX, TX);
CMRInet::SerialCMRITransport 
    transport(port);
CMRInet::CMRINode 
    node(transport);

// Cached bits, one [portA, portB] per expander: pack staging and the
// OLED grid snapshot. Inputs refresh at P time and between polls;
// outputs refresh at T time.
uint8_t portState[kExpanderCount][kPortsPerExpander] = {};

#ifdef USE_OLED
// Spinner counters (donor naming: tx = pack, rx = unpack).
unsigned long txCount = 0;
unsigned long rxCount = 0;

NodeDisplay oled;
constexpr uint32_t kDisplayRefreshMs = 100;  // ~10 fps; bits change slowly
uint32_t lastDisplayMs = 0;
#endif

void sampleInputPorts();
void packInputs(CMRInet::IOBuffer& ib);
void unpackOutputs(CMRInet::IOBuffer& ob);

// =============================================
// ====   Setup                             ====
// =============================================

void setup() {
  Wire.begin();  // expanders + OLED

#ifdef USE_OLED
  oled.begin(NODE_NAME);  // degrades to headless on failure
#endif

  // Initialize expanders and derive the SMINI geometry: all I/O
  // lives on expanders.
  const IOX_Geometry geom = ioxInit(expanders, kExpanderCount);

  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = NODE_ID;
  cfg.nodeType    = 'M';  // SMINI: 24 bits in, 48 bits out
  cfg.inputBytes  = geom.inputBytes;   // 3
  cfg.outputBytes = geom.outputBytes;  // 6
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();  // → transport.begin() → port.begin() → Serial1.begin(...)
}

// =============================================
// ====   Loop                              ====
// =============================================

void loop() {
  const uint32_t now = millis();
  node.tick(now);

#ifdef USE_OLED
  if (now - lastDisplayMs >= kDisplayRefreshMs) {
    lastDisplayMs = now;
    // Sample inputs so the grid tracks pin changes between polls.
    // Outputs still arrive via unpackOutputs().
    sampleInputPorts();
    oled.update(expanders, kExpanderCount, portState);
    oled.setTX(txCount);
    oled.setRX(rxCount);
    oled.show();  // pushes a frame only when something changed
  }
  oled.serviceFlush();  // one I2C chunk per loop
#endif
}

// Read every IN port into portState.
void sampleInputPorts() {
  for (uint8_t e = 0; e < kExpanderCount; e++) {
    if (expanders[e].portA == IN) {
      portState[e][0] = ioxReadPort(expanders[e].address, true);
    }
    if (expanders[e].portB == IN) {
      portState[e][1] = ioxReadPort(expanders[e].address, false);
    }
  }
}

// onPack (P time): read all inputs into the image. SMINI has no
// phantom bytes — the image starts with real data. IN ports pack in
// table order, port A before port B. RESPONSE is sent automatically.
void packInputs(CMRInet::IOBuffer& ib) {
#ifdef USE_OLED
  txCount++;  // spinner fuel; the counters live in the USE_OLED block
#endif
  sampleInputPorts();
  uint8_t idx = 0;
  for (uint8_t e = 0; e < kExpanderCount && idx < ib.length(); e++) {
    if (expanders[e].portA == IN) {
      ib.setByte(idx++, portState[e][0]);
    }
    if (expanders[e].portB == IN && idx < ib.length()) {
      ib.setByte(idx++, portState[e][1]);
    }
  }
}

// onUnpack (T time): write all outputs from the image, byte 0 first,
// port A before port B. Bit 1 = pin HIGH, written direct.
void unpackOutputs(CMRInet::IOBuffer& ob) {
#ifdef USE_OLED
  rxCount++;  // spinner fuel; the counters live in the USE_OLED block
#endif
  uint8_t idx = 0;
  for (uint8_t e = 0; e < kExpanderCount && idx < ob.length(); e++) {
    if (expanders[e].portA == OUT) {
      const uint8_t val = ob.byte(idx++);
      portState[e][0] = val;
      ioxWritePort(expanders[e].address, true, val);
    }
    if (expanders[e].portB == OUT && idx < ob.length()) {
      const uint8_t val = ob.byte(idx++);
      portState[e][1] = val;
      ioxWritePort(expanders[e].address, false, val);
    }
  }
}