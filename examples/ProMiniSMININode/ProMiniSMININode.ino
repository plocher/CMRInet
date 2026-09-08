// ProMiniSMININode.ino
// A functional SMINI work-alike node for the 
// cpNode-ProMini  (MRCS ATmega328P 16 MHz).
//
// CMRInet type 'M' (SMINI): 24 inputs, 48 outputs
//
// ---- Board wiring ----
// 4x cpNode-IOX-16 I2C expander boards.
//   D0 - RX    CMRI RS485 receive
//   D1 - TX    CMRI RS485 transmit
//   A4 - SCL   I2C (expanders)
//   A5 - SDA   I2C (expanders)
//   TXEN: MRCS AutoRTS (555 timer) circuit. The circuit watches
//   TX and holds driver enable by itself, past the last stop bit.
//   The library runs with no TXEN pin (AvrSerialPort::kNoTxenPin);
//   its drain estimate gates engine sequencing only, never wire
//   integrity — the 555 owns that.
//   The 16 onboard pins are inputs 0..15 (INPUT_PULLUP).
//   No LED use: LED_BUILTIN (D13) is input byte 1, bit 3.
//
// ---- RS485 bus wiring ----
//   R± on the Node routes to the Host's T±, and T± on the Node
//   routes to the Host's R±. Wire all Nodes the same way, TX to
//   TX and RX to RX, "+" in a pair to "+" and "-" to "-".
//
// ---- SMINI byte map ----
// Onboard bytes first, IOX bytes follow (donor cpNode convention).
//   Inputs (3 bytes):
//     byte 0, bits 0-7:  D2..D9
//     byte 1, bits 0-7:  D10..D13, A0..A3
//     byte 2, bits 0-7:  expander 0x20 port A (port B unused)
//   Outputs (6 bytes):
//     bytes 0-1: 0x21 ports A/B
//     bytes 2-3: 0x22 ports A/B
//     bytes 4-5: 0x23 ports A/B
//


#if !defined(ARDUINO_ARCH_AVR)
  #if defined(ARDUINO_ARCH_ESP32)
    #error "For the XIAO ESP32-C6 target, use the XiaoSMININode example."
  #endif
  #error "This sketch only works on an ATmega328P (arduino:avr)."
#endif

#include <Arduino.h>
#include <Wire.h>

#include "CMRInet.h"              // CMRINode, CMRINodeConfig, IOBuffer
#include "transport/serial.h"     // SerialCMRITransport
#include "transport/serialAvr.h"  // AvrSerialPort
#include "iox.h"                  // MCP23017 expander access

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

// MCP23017 expanders: {address, portA, portB}. IN ports pack in
// table order (port A before port B); OUT ports unpack the same
// way. A correct table yields the SMINI geometry: 3 input bytes
// and 6 output bytes.
IOX_Config expanders[] = {
    { 0x20, IN,  UNUSED },  // inputs:  byte 2 (port A only)
    { 0x21, OUT, OUT    },  // outputs: bytes 0-1
    { 0x22, OUT, OUT    },  // outputs: bytes 2-3
    { 0x23, OUT, OUT    },  // outputs: bytes 4-5
};
// Onboard contribution: D2..D9 (byte 0), D10..D13 + A0..A3 (byte 1).
constexpr uint8_t kOnboardInputBytes = 2;
// Onboard input pins in byte/bit order: byte 0 bits 0-7, then byte 1
// bits 0-7. INPUT_PULLUP; bit 1 = pin grounded. No LED use: pin 13
// (LED_BUILTIN) is input byte 1, bit 3.
const uint8_t kOnboardInputPins[16] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, A0, A1, A2, A3};

constexpr uint8_t kExpanderCount =
    static_cast<uint8_t>(sizeof(expanders) / sizeof(expanders[0]));

// =============================================
// ====   Plumbing                          ====
// =============================================

// No TXEN pin: the MRCS AutoRTS (555) circuit owns driver enable.
// The port's begin() configures Serial itself (CMRI_BAUD, 8N2).
CMRInet::AvrSerialPort 
    port(Serial, CMRInet::AvrSerialPort::kNoTxenPin, CMRI_BAUD);
CMRInet::SerialCMRITransport 
    transport(port);
CMRInet::CMRINode 
    node(transport);

void packInputs(CMRInet::IOBuffer& ib);
void unpackOutputs(CMRInet::IOBuffer& ob);

// =============================================
// ====   Setup                             ====
// =============================================

void setup() {
  Wire.begin();  // expanders

  for (uint8_t i = 0; i < sizeof(kOnboardInputPins); i++) {
    pinMode(kOnboardInputPins[i], INPUT_PULLUP);
  }

  // Initialize expanders and derive the SMINI geometry. Onboard
  // input bytes come first in the image; IOX bytes follow.
  const IOX_Geometry geom = ioxInit(expanders, kExpanderCount);

  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = NODE_ID;
  cfg.nodeType    = 'M';  // SMINI: 24 bits in, 48 bits out
  cfg.inputBytes  = kOnboardInputBytes + geom.inputBytes;  // 3
  cfg.outputBytes = geom.outputBytes;                      // 6
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();  // → transport.begin() → port.begin() → Serial.begin(...)
}

// =============================================
// ====   Loop                              ====
// =============================================

void loop() {
  const uint32_t now = millis();
  node.tick(now);
}


// onPack (POLL received, RESPONSE time): 
void packInputs(CMRInet::IOBuffer& ib) {
  // Onboard bits first: byte 0 = D2..D9, byte 1 = D10..D13 + A0..A3. 
  // A grounded pin (LOW) with INPUT_PULLUP is reported as 1.
  uint8_t byteIdx = 0;
  for (byteIdx = 0; byteIdx < kOnboardInputBytes; byteIdx++) {
    uint8_t val = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (digitalRead(kOnboardInputPins[byteIdx * 8 + bit]) == LOW) {
        val |= static_cast<uint8_t>(1u << bit);
      }
    }
    ib.setByte(byteIdx, val);
  }

  // Then expander IN ports in table order, port A before port B.
  byteIdx = kOnboardInputBytes;
  for (uint8_t e = 0; e < kExpanderCount && byteIdx < ib.length(); e++) {
    if (expanders[e].portA == IN) {
      ib.setByte(byteIdx++, ioxReadPort(expanders[e].address, true));
    }
    if (expanders[e].portB == IN && byteIdx < ib.length()) {
      ib.setByte(byteIdx++, ioxReadPort(expanders[e].address, false));
    }
  }
  // RESPONSE is sent automatically.
}

// onUnpack (TRANSMIT received): write OUT ports from the image, byte 0 first,
// port A before port B. 
//Bit 1 = pin HIGH, written direct.
void unpackOutputs(CMRInet::IOBuffer& ob) {
  uint8_t idx = 0;
  for (uint8_t e = 0; e < kExpanderCount && idx < ob.length(); e++) {
    if (expanders[e].portA == OUT) {
      ioxWritePort(expanders[e].address, true, ob.byte(idx++));
    }
    if (expanders[e].portB == OUT && idx < ob.length()) {
      ioxWritePort(expanders[e].address, false, ob.byte(idx++));
    }
  }
}
