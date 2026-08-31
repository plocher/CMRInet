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
// - OLED per-bit I/O grid with change halos (donor Xiao_I2C panel).
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
//
// OLED live view (when USE_OLED):
//   Top:    NODE_NAME + r/t spinners (pack / unpack traffic)
//   Middle: one row per expander; Port B left, Port A right; bits 7..0
//           L→R; filled=1 hollow=0; recent changes briefly halo-boxed
//   Bottom: WiFi/OTA status line

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

#ifdef USE_OLED
#include "display.h"
#endif

#ifdef USE_OTA
#include "ota.h"
// #include "secrets.h"  // optional for credentials
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
#ifndef NODE_ID
  #define NODE_ID 30    // 0...127, same as in JMRI
#endif
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

// MCP23017 expanders. Each has two 8-bit ports (A, B).
// Edit this table to match your hardware. Count is sizeof(expanders);
// the OLED is told that count at begin() and clamps at its fixed max-8.
IOX_Config expanders[] =
#if NODE_ID == 30
{
    { 0x20, IN,     OUT    },
    { 0x21, IN,     OUT    },
    { 0x22, IN,     OUT    },
    { 0x23, OUT,    IN     },
    { 0x24, OUT,    IN     },
    { 0x25, UNUSED, UNUSED },
    { 0x26, UNUSED, UNUSED },
    { 0x27, UNUSED, UNUSED },
};
#else
{
    { 0x20, IN,     OUT    },
    { 0x21, UNUSED, UNUSED },
    { 0x22, UNUSED, UNUSED },
    { 0x23, UNUSED, UNUSED },
    { 0x24, UNUSED, UNUSED },
    { 0x25, UNUSED, UNUSED },
    { 0x26, UNUSED, UNUSED },
    { 0x27, UNUSED, UNUSED },
};
#endif

constexpr uint8_t kExpanderCount =
    static_cast<uint8_t>(sizeof(expanders) / sizeof(expanders[0]));

#ifdef USE_OLED
NodeDisplay oled;
constexpr uint32_t kDisplayRefreshMs = 100;  // ~10 fps; bits change slowly
uint32_t lastDisplayMs = 0;
// Cached bits from the last pack/unpack, one [portA, portB] per expander.
// Row count from the I/O table; columns match the panel's port pair.
uint8_t portState[kExpanderCount][NodeDisplay::kPortsPerExpander] = {};
#endif

#ifdef USE_OTA
OtaManager ota;
#endif

// Counters for the status panel (used by loop() and the callbacks).
// Naming matches the donor: tx = pack (reply to poll), rx = unpack (T).
unsigned long txCount = 0;  // pack / poll replies
unsigned long rxCount = 0;  // unpack / transmits received

// =============================================
// ====   Plumbing                          ====
// =============================================
CMRInet::Esp32SerialPort        port(Serial1, D3, 28800, RX, TX);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRINode               node(transport);

void packInputs(CMRInet::IOBuffer& ib);
void unpackOutputs(CMRInet::IOBuffer& ob);
#ifdef USE_OLED
void sampleInputPorts();
#endif

#if defined(USE_OLED) && defined(USE_OTA)
static NodeDisplay::NetState netStateFor(OtaManager::State s) {
  switch (s) {
    case OtaManager::READY:
    case OtaManager::UPDATING:
      return NodeDisplay::NET_READY;
    case OtaManager::CONNECTING:
      return NodeDisplay::NET_CONNECTING;
    case OtaManager::FAILED:
      return NodeDisplay::NET_FAILED;
    default:
      return NodeDisplay::NET_OFF;
  }
}
#endif

// =============================================
// ====   Setup                             ====
// =============================================
void setup() {
  Wire.begin();  // default pins for the board (D4/D5 on cpNode-Xiao)

#ifdef USE_OLED
  oled.begin(NODE_NAME);  // degrades to headless on failure
#endif

  // Initialize expanders and derive geometry.
  // CPNODE card type has 2 phantom onboard bytes; IOX bytes follow.
  IOX_Geometry geom = ioxInit(expanders, kExpanderCount);

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
#ifdef USE_OLED
  ota.onStart = []() { oled.otaStart(); };
  ota.onProgress = [](unsigned int received, unsigned int total) {
    oled.otaProgress(received, total);
  };
  ota.onEnd = []() { oled.otaSuccess(); };
  ota.onError = [](const char* name) { oled.otaError(name); };
#endif
  ota.begin(NODE_NAME, WIFI_SSID, WIFI_PASSWORD);
#endif
}

// =============================================
// ====   Loop                              ====
// =============================================
void loop() {
  node.tick(millis());

#ifdef USE_OTA
  ota.poll();  // non-blocking; blocks only during a transfer
#endif

#ifdef USE_OLED
  const uint32_t now = millis();
  if (now - lastDisplayMs >= kDisplayRefreshMs) {
    lastDisplayMs = now;
    // Sample inputs here so the grid tracks pin changes even when the
    // Host is not polling. Outputs still arrive via unpack().
    sampleInputPorts();
    oled.update(expanders, kExpanderCount, portState);
    oled.setTX(txCount);
    oled.setRX(rxCount);
#ifdef USE_OTA
    oled.setNet(netStateFor(ota.state()), ota.ip());
#endif
    oled.show();  // pushes a frame only when something changed
  }
  oled.serviceFlush();  // one I2C chunk per loop
#endif
}

// =============================================
// ====   Convenience routines              ====
// =============================================

// onPack: read all input ports into IB at P time.
// onUnpack: write all output ports from OB at T time.
// Both skip the 2 phantom CPNODE bytes at the start of the image.
// portState is the OLED snapshot: inputs are also sampled in loop so
// the grid moves without waiting for a Host poll; outputs update here.

#ifdef USE_OLED
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
#endif

void packInputs(CMRInet::IOBuffer& ib) {
  txCount++;
  ib.setByte(0, 0);  // phantom onboard byte
  ib.setByte(1, 0);  // phantom onboard byte
  uint8_t idx = 2;
  for (uint8_t e = 0; e < kExpanderCount && idx < ib.length(); e++) {
    if (expanders[e].portA == IN) {
      const uint8_t val = ioxReadPort(expanders[e].address, true);
#ifdef USE_OLED
      portState[e][0] = val;
#endif
      ib.setByte(idx++, val);
    }
    if (expanders[e].portB == IN && idx < ib.length()) {
      const uint8_t val = ioxReadPort(expanders[e].address, false);
#ifdef USE_OLED
      portState[e][1] = val;
#endif
      ib.setByte(idx++, val);
    }
  }
}

void unpackOutputs(CMRInet::IOBuffer& ob) {
  rxCount++;
  uint8_t idx = 2;  // skip phantom onboard bytes
  for (uint8_t e = 0; e < kExpanderCount && idx < ob.length(); e++) {
    if (expanders[e].portA == OUT) {
      const uint8_t val = ob.byte(idx++);
#ifdef USE_OLED
      portState[e][0] = val;
#endif
      ioxWritePort(expanders[e].address, true, val);
    }
    if (expanders[e].portB == OUT && idx < ob.length()) {
      const uint8_t val = ob.byte(idx++);
#ifdef USE_OLED
      portState[e][1] = val;
#endif
      ioxWritePort(expanders[e].address, false, val);
    }
  }
}
