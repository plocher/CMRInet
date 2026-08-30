// SimpleNode.ino — minimal CMRINode example.
//
// a CMRInet node: 
//    - read a pushbutton, 
//    - drive an LED
//
// The pack/unpack callbacks are how a Node sketch moves data between
// the wire and the connected sensors and actuators.
// The CMRI engine calls pack() when it gets a POLL message
// so the routine can fill the input image.  In the same way,
// it calls unpack() when it gets a TRANSMIT message with
// the output image so the routine can drive output pins. 
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

#include <Arduino.h>

#include "CMRInet.h"               // CMRINode, CMRINodeConfig
#include "transport/serial.h"      // SerialCMRITransport
#include "transport/serialESP32.h" // ESP32 hardware transmit-drain port

// ---- Configuration ---------------------------------------------------------

#ifndef SIMPLE_NODE_UA
#define SIMPLE_NODE_UA 30          // node UA (0..127); wire UA = UA + 65
#endif

#ifndef SIMPLE_NODE_INPUT_BYTES
#define SIMPLE_NODE_INPUT_BYTES 1  // NI: one input byte (8 input bits)
#endif

#ifndef SIMPLE_NODE_OUTPUT_BYTES
#define SIMPLE_NODE_OUTPUT_BYTES 1 // NO: one output byte (8 output bits)
#endif

#ifndef SIMPLE_NODE_BAUD
#define SIMPLE_NODE_BAUD 28800
#endif

constexpr int kLedPin    = LED_BUILTIN;
constexpr int kButtonPin = D2;

// ---- Wiring (static; the library never allocates) -------------------------

constexpr int kTxenPin = D3;

// The port adapter wraps the stream but does NOT configure the UART.
// The sketch must call Serial1.begin() below to set baud, framing, and
// pin routing before the transport uses it.
CMRInet::Esp32SerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                       SIMPLE_NODE_BAUD);
CMRInet::SerialCMRITransport    transport(port);

CMRInet::CMRINodeConfig makeConfig() {
  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = SIMPLE_NODE_UA;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = SIMPLE_NODE_INPUT_BYTES;
  cfg.outputBytes = SIMPLE_NODE_OUTPUT_BYTES;
  return cfg;
}

CMRInet::CMRINode node(transport, makeConfig());

// ---- Pack / Unpack seam ----------------------------------------------------
//
// pack() is called at P time: fill the input image (ib) the engine
//   will send as R. The callback receives the buffer and its length.
// unpack() is called at T time: the engine has stored the output
//   image (ob) the Host sent; drive hardware from it.
//
// The engine guarantees len > 0 for both callbacks (it guards on
// inputBytes / outputBytes before calling). For a single byte with a
// single bit, the math is trivial. For multi-byte images with I2C
// expanders, see XiaoNode — the pattern is the same, the body grows.

void packInputs(void*, uint8_t* ib, size_t len) {
  // Read the pushbutton into input bit (0, 0).
  // Active-low: pressed = LOW = true.
  const bool buttonPressed = (digitalRead(kButtonPin) == LOW);
  ib[0] = buttonPressed ? 0x01 : 0x00;
}

void unpackOutputs(void*, const uint8_t* ob, size_t len) {
  // Drive the LED from output bit (0, 0).
  // The Host sets this bit via a T message.
  digitalWrite(kLedPin, (ob[0] & 0x01u) ? HIGH : LOW);
}

// ---- Setup -----------------------------------------------------------------

void setup() {
  pinMode(kLedPin, OUTPUT);
  digitalWrite(kLedPin, LOW);
  pinMode(kButtonPin, INPUT_PULLUP);

  Serial.begin(115200);  // USB CDC: diagnostic output

  // Configure the CMRI wire: 28800 8N2 on the MAX3491 UART pins.
  // The port adapter above wraps Serial1 but does not configure it;
  // this call sets the baud rate, framing, and RX/TX pin mapping.
  Serial1.begin(SIMPLE_NODE_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);

  // Register the pack/unpack seam before begin().
  node.pack(packInputs);
  node.unpack(unpackOutputs);
  node.begin();
}

// ---- Loop ------------------------------------------------------------------

void loop() {
  node.tick(millis());

  // The engine calls packInputs on the next poll and unpackOutputs
  // when a T arrives — no image work needed here in loop().
  //
  // If you have event-driven inputs that don't need to wait for poll
  // time, you can also set bits directly:
  //   node.setInputBit(0, 0, buttonPressed);  // writes the same ib
  // Both patterns touch the same buffer and can be mixed.

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
