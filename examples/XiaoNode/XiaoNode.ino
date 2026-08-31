// XiaoNode.ino — full-featured CMRINode with OLED and WiFi OTA.
//
// The rich Node example: same onPack/onUnpack seam as SimpleNode, but
// driving MCP23017 I2C expanders for real layout I/O, with an SSD1306
// status panel and non-blocking WiFi OTA firmware updates.
//
// What it demonstrates:
// - The onPack/onUnpack callback pattern with real hardware (I2C expanders).
// - onPack reads input ports at P time; onUnpack writes output ports
//   at T time. The engine handles the protocol.
// - Feature toggles (USE_OLED, USE_OTA) that compile out cleanly.
// - OLED status panel with OTA progress/success/error screens.
// - No dependency on the cpNode library — MCP23017 access is in iox.h/cpp.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
//   D4 - SDA  I2C (expanders + OLED)
//   D5 - SCL  I2C (expanders + OLED)
//
// I/O lives on MCP23017 expanders at I2C addresses 0x20-0x27.
// Each expander has two 8-bit ports (A, B); each port is all-input or
// all-output. Edit the expanders[] table below to match your hardware.
//
// CPNODE card type: the first 2 bytes in each direction are phantom
// onboard bytes (the #36 phantom-byte trap). IOX bytes follow them.
// The sketch owns this layout knowledge — the engine deals in raw
// images and does not know about card types or phantom offsets.

// =============================================
// ====   Feature toggles                   ====
// =============================================
#define USE_OLED   // Comment out to run headless
#define USE_OTA    // Comment out to disable WiFi + OTA

#include <Arduino.h>
#include <Wire.h>

#include "CMRInet.h"
#include "transport/serial.h"
#include "transport/serialESP32.h"
#include "iox.h"
#include "display.h"

#ifdef USE_OTA
#include "ota.h"
#ifndef WIFI_SSID
  // Default placeholders so the sketch compiles out of the box.
  // Create secrets.h with real credentials, or inject via build defines:
  //   --build-property "compiler.cpp.extra_flags=-DWIFI_SSID=... -DWIFI_PASSWORD=..."
  #define WIFI_SSID     "your-network"
  #define WIFI_PASSWORD "your-password"
#endif
#endif

// =============================================
// ====   User configuration                 ====
// =============================================
#define NODE_ID 30    // 0...127, same as in JMRI
#define CMRI_NODE_DESCRIPTION "XiaoC6"
// The node's name: shown on the OLED header and used as the mDNS
// hostname for OTA discovery. Defaults to description-nodeID (e.g.
// "XiaoC6-30"); override with a name of your own meaning here or
// from a CLI build (-DNODE_NAME='"yard-throat"').
#define STRINGIFY_(x)  #x
#define STRINGIFY(x)   STRINGIFY_(x)
#ifndef NODE_NAME
#define NODE_NAME      CMRI_NODE_DESCRIPTION "-" STRINGIFY(NODE_ID)
#endif
// Up to 8 MCP23017 expanders. Each has two 8-bit ports.
// Edit this table to match your hardware.
IOX_Config expanders[] = {
  { 0x20, IN,     OUT    },
  { 0x21, IN,     OUT    },
  { 0x22, UNUSED, UNUSED },
  { 0x23, UNUSED, UNUSED },
  { 0x24, UNUSED, UNUSED },
  { 0x25, UNUSED, UNUSED },
  { 0x26, UNUSED, UNUSED },
  { 0x27, UNUSED, UNUSED },
};

// =============================================
// ====   Wiring                              ====
// =============================================
CMRInet::Esp32SerialPort port(Serial1, D3, 28800, RX, TX);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRINode               node(transport);

#ifdef USE_OTA
OtaManager ota;
#endif

// Counters for the status panel (used by loop() and the callbacks).
uint32_t pollCount = 0;
uint32_t txCount   = 0;

// =============================================
// ====   Forward declarations                ====
// =============================================
void packInputs(CMRInet::IOBuffer& ib);
void unpackOutputs(CMRInet::IOBuffer& ob);
void displayInit();
bool displayRefresh();
void drawNodeStatus(uint8_t ua, uint32_t polls, uint32_t txs,
                     const uint8_t* outputs, size_t outLen);
void displayOtaStart();
void displayOtaProgress(unsigned int received, unsigned int total);
void displayOtaSuccess();
void displayOtaError(const char* name);

// =============================================
// ====   Setup                               ====
// =============================================
void setup() {
  Wire.begin(D4 /* SDA */, D5 /* SCL */);

  displayInit();

  // Initialize expanders and derive geometry.
  // CPNODE card type has 2 phantom onboard bytes; IOX bytes follow.
  IOX_Geometry geom = ioxInit(expanders, sizeof(expanders) / sizeof(expanders[0]));

  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = NODE_ID;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = 2 + geom.inputBytes;
  cfg.outputBytes = 2 + geom.outputBytes;
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();  // → transport.begin() → port.begin() → Serial1.begin()

#ifdef USE_OTA
  ota.onStart    = displayOtaStart;
  ota.onProgress = displayOtaProgress;
  ota.onEnd      = displayOtaSuccess;
  ota.onError    = displayOtaError;
  ota.begin(NODE_NAME, WIFI_SSID, WIFI_PASSWORD);
#endif
}

// =============================================
// ====   Loop                                ====
// =============================================
void loop() {
  node.tick(millis());

#ifdef USE_OTA
  ota.poll();  // non-blocking; blocks only during a transfer
#endif

  if (displayRefresh()) {
    drawNodeStatus(node.UA(), pollCount, txCount,
                   node.outputs(), node.outputLength());
  }
}

// =============================================
// ====   Convenience routines                ====
// =============================================

// onPack: read all input ports into IB at P time.
// onUnpack: write all output ports from OB at T time.
// Both skip the 2 phantom CPNODE bytes at the start of the image.

void packInputs(CMRInet::IOBuffer& ib) {
  pollCount++;
  ib.setByte(0, 0);  // phantom onboard byte
  ib.setByte(1, 0);  // phantom onboard byte
  uint8_t idx = 2;
  for (uint8_t e = 0; e < sizeof(expanders) / sizeof(expanders[0]) && idx < ib.length(); e++) {
    if (expanders[e].portA == IN) {
      ib.setByte(idx++, ioxReadPort(expanders[e].address, true));
    }
    if (expanders[e].portB == IN && idx < ib.length()) {
      ib.setByte(idx++, ioxReadPort(expanders[e].address, false));
    }
  }
}

void unpackOutputs(CMRInet::IOBuffer& ob) {
  txCount++;
  uint8_t idx = 2;  // skip phantom onboard bytes
  for (uint8_t e = 0; e < sizeof(expanders) / sizeof(expanders[0]) && idx < ob.length(); e++) {
    if (expanders[e].portA == OUT) {
      ioxWritePort(expanders[e].address, true, ob.byte(idx++));
    }
    if (expanders[e].portB == OUT && idx < ob.length()) {
      ioxWritePort(expanders[e].address, false, ob.byte(idx++));
    }
  }
}
