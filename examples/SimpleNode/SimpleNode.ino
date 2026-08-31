// SimpleNode.ino — minimal CMRINode example.
// 
// Hardware requirements:
// A cpNode-Xiao connected to a 4-wire RS485 CMRI bus.
// JMRI configured as a CMRI Host with a "C type" node as UA 30.
//    This usually requires a USB/RS485 dongle on the PC, and a 
//    4-wire RS485 bus to the cpNode-Xiao.
//
// What it demonstrates:
// - A functional CMRINode: JMRI's sensor and turnout tables can show changes
//   on Sensor CMRI:cs0002 and Turnout CMRI:ct0001
// - Configuration in setup().
// - Registration and use of onPack/onUnpack callbacks.
//   - The onboard LED is driven by CT0001 on the cpNode-Xiao.
//   - The onboard pushbutton on D2is read by CS0002 on the cpNode-Xiao.
// - Driving the engine in loop() with node.tick(millis()).
//
// How it works:
// The onPack/onUnpack mechanism is how a Node sketch moves data between
// the CMRI bus and the hardware. When it receives a POLL request, the 
// engine calls the onPack callback to construct a RESPONSE.  When it
// receives a TRANSMIT packet, the engine invokes the onUnpack callback
// to set the associated output pins.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491) wired as follows:
//   D7 - RX    CMRI 4-wire RS485 receive
//   D6 - TX    CMRI 4-wire RS485 transmit
//   D5 - SCL   I2C (unused in this example)
//   D4 - SDA   I2C (unused in this example)
//   D3 - TXEN  RS422/485 transmit enable
//   D2 -       pushbutton input (active-low)
//
// RS485 bus wiring:
//         R± on the Node routes to the Host's T± and
//         T± on the Node routes to the Host's R±
//
// No sketch support for OLED, WiFi, OTA or I2C

#include <Arduino.h>

#include "CMRInet.h"               // CMRINode, CMRINodeConfig, IOBuffer
#include "transport/serial.h"      // SerialCMRITransport
#include "transport/serialESP32.h" // Esp32SerialPort

// ---- Plumbing --------------------------------------------------------------

CMRInet::Esp32SerialPort        port(Serial1, /* TXEN */ D3, /* Baud */ 28800, RX, TX);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRINode               node(transport);

// ---- Pack / Unpack data ----------------------------------------------------
//
// onPack callback on POLL receipt: read the switches and sensors
void packInputs(CMRInet::IOBuffer& ib) {
  ib.setBit(0, 2, (digitalRead(D2) == LOW));  // active-low button
  // RESPONSE is automatically sent
}

// onUnpack callback on TRANSMIT receipt:  write outputs.
void unpackOutputs(CMRInet::IOBuffer& ob) {
  digitalWrite(LED_BUILTIN, ob.getBit(0, 1) ? HIGH : LOW);
}

// ---- Setup -----------------------------------------------------------------

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(D2, INPUT_PULLUP);

  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = 30;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = 1;
  cfg.outputBytes = 1;
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();  // → transport.begin() → port.begin() → Serial1.begin()
}

// ---- Loop ------------------------------------------------------------------

void loop() {
  node.tick(millis());
}
