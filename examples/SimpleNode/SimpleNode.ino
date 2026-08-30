// SimpleNode.ino — minimal CMRINode example.
//
// The front-door Node example, mirroring SimpleHost's role for the
// Node side. This sketch is the simplest way to make a device act as
// a CMRInet node: read a pushbutton, drive an LED, and let the engine
// handle the protocol.
//
// What it demonstrates:
// - Construct one CMRINode with its transport.
// - Configure it in setup() with config().
// - Register onPack/onUnpack callbacks — the canonical Node seam.
// - Drive the engine with node.tick(millis()).
//
// The onPack/onUnpack seam is how a Node sketch moves data between
// the wire and the hardware. The engine calls onPack at P time (just
// before sending R) to fill the input image, and onUnpack at T time
// (after receiving the output image) to drive pins.
//
// Inside the callbacks, two peer APIs touch the same image buffer:
//   setBit(ib, byte, bit, v)   — write a bit in the raw buffer
//   node.setInputBit(byte, bit, v) — write a bit via the engine
// Both are equally first-class; use whichever reads cleaner. The
// direct accessors in loop() (setInputBit / outputBit) are a safety
// net for cases where you want to set or read bits outside callback
// time.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex).
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
//
// RS485 bus wiring:
//         R± on the Node routes to the Host's T± and
//         T± on the Node routes to the Host's R±
//
// No OLED, no WiFi, no OTA — just the engine and two pins.

#include <Arduino.h>

#include "CMRInet.h"               // CMRINode, CMRINodeConfig, setBit, getBit
#include "transport/serial.h"      // SerialCMRITransport
#include "transport/serialESP32.h" // Esp32SerialPort (auto-detects UART,
                                   //   calls Serial1.begin() from begin())

// ---- Wiring (static; the library never allocates) -------------------------

CMRInet::Esp32SerialPort port(Serial1, D3, 28800, RX, TX);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRINode               node(transport);

// ---- Pack / Unpack seam ----------------------------------------------------
//
// onPack is called at P time: fill the input image (ib) the engine
//   will send as R. The engine guarantees len > 0.
// onUnpack is called at T time: the engine has stored the output
//   image (ob) the Host sent; drive hardware from it.

void packInputs(uint8_t* ib, size_t len) {
  CMRInet::setBit(ib, 0, 0, (digitalRead(D2) == LOW));  // active-low button
}

void unpackOutputs(const uint8_t* ob, size_t len) {
  digitalWrite(LED_BUILTIN, CMRInet::getBit(ob, 0, 0) ? HIGH : LOW);
}

// ---- Setup -----------------------------------------------------------------

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(D2, INPUT_PULLUP);

  Serial.begin(115200);  // USB CDC: diagnostic output

  CMRInet::CMRINodeConfig cfg;
  cfg.ua = 30;
  cfg.nodeType = 'C';
  cfg.inputBytes = 1;
  cfg.outputBytes = 1;
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();  // → transport.begin() → port.begin() → Serial1.begin()
}

// ---- Loop ------------------------------------------------------------------

void loop() {
  node.tick(millis());

  // Periodic diagnostic: show input/output images every 5 s.
  static uint32_t lastPrintMs = 0;
  const uint32_t now = millis();
  if (now - lastPrintMs >= 5000u || lastPrintMs == 0) {
    Serial.print("SimpleNode UA=");
    Serial.print(node.UA());
    Serial.print(" in=");
    Serial.print(node.inputByte(0), HEX);
    Serial.print(" out=");
    Serial.print(node.outputByte(0), HEX);
    Serial.println();
    lastPrintMs = now;
  }
}
