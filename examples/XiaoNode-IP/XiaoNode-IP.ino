// XiaoNode-IP.ino — CMRINode over TCP (JMRI CMRI-over-IP) with OLED and WiFi OTA.
//
// Demonstrates C/MRI over TCP using TcpCMRITransport and ClientTcpPort:
// - Communicates with JMRI's "Network Interface" C/MRI connection.
// - Listens on TCP port 2000 (default) as a WiFiServer.
// - Automatically accepts incoming client connections from JMRI.
// - Drives 4 MCP23017 I2C expanders:
//     * 0x20: Port A IN,  Port B IN  (16 input bits)
//     * 0x21: Port A OUT, Port B OUT (16 output bits)
//     * 0x22: Port A OUT, Port B OUT (16 output bits)
//     * 0x23: Port A OUT, Port B OUT (16 output bits)
// - SSD1306 live view showing bit grid, link spinners, and WiFi IP address.
// - Non-blocking WiFi OTA updates via ArduinoOTA.
//
// JMRI Connection Setup:
//   1. Preferences -> Connections -> Add Connection
//   2. System manufacturer: C/MRI
//   3. System connection:   Network Interface
//   4. IP address:          <Node IP from OLED bottom line or Serial monitor>
//   5. Port:                2000
//   6. Configure Node:      Node address matching NODE_ID (default 30),
//                           Type: CPNODE (or SUSIC with 4 bytes in, 8 bytes out).
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6):
//   D4 - SDA  I2C (expanders + OLED)
//   D5 - SCL  I2C (expanders + OLED)
//   (RS-485 transceiver pins D3/D6/D7 are unused on this TCP node)

// =============================================
// ====   Feature toggles                   ====
// =============================================

#define USE_OLED   // Comment out to run headless
#define USE_OTA    // Comment out to disable WiFi OTA

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

#include "CMRInet.h"
#include "transport/tcp.h"
#include "transport/tcpClient.h"
#include "iox.h"
#include "display.h"

#ifdef USE_OTA
#include "ota.h"
#if __has_include("secrets.h")  
#include "secrets.h"  // optional for credentials
#endif
#ifndef WIFI_SSID
  // Default placeholders so the sketch compiles out of the box.
  // Create secrets.h with real credentials, or inject via build defines:
  //   --build-property "compiler.cpp.extra_flags=-DWIFI_SSID=... -DWIFI_PASSWORD=... -DNODE_ID=30"
  #define WIFI_SSID     "your-network"
  #define WIFI_PASSWORD "your-password"
#endif
#endif

// =============================================
// ====   User configuration                 ====
// =============================================

#ifndef NODE_ID
  #define NODE_ID 51    // 0...127, matching JMRI node address
#endif

#ifndef CMRI_PORT
  #define CMRI_PORT 2000  // TCP listening port for JMRI
#endif

#define CMRI_NODE_DESCRIPTION "XiaoIP"
#define STRINGIFY_(x)  #x
#define STRINGIFY(x)   STRINGIFY_(x)
#ifndef NODE_NAME
#define NODE_NAME      CMRI_NODE_DESCRIPTION "-" STRINGIFY(NODE_ID)
#endif

// MCP23017 expanders:
// 1 expander with both ports IN (0x20) + 3 expanders all OUT (0x21, 0x22, 0x23)
IOX_Config expanders[] = {
    { 0x20, IN,  IN  },  // Expander 0: 16 inputs  (bytes 0-1)
    { 0x21, OUT, OUT },  // Expander 1: 16 outputs (bytes 0-1)
    { 0x22, OUT, OUT },  // Expander 2: 16 outputs (bytes 2-3)
    { 0x23, OUT, OUT },  // Expander 3: 16 outputs (bytes 4-5)
};

constexpr uint8_t kExpanderCount =
    static_cast<uint8_t>(sizeof(expanders) / sizeof(expanders[0]));

NodeDisplay oled;
constexpr uint32_t kDisplayRefreshMs = 100;  // ~10 fps
uint32_t lastDisplayMs = 0;

// Cached bits from the last pack/unpack, one [portA, portB] per expander.
uint8_t portState[kExpanderCount][NodeDisplay::kPortsPerExpander] = {};

#ifdef USE_OTA
OtaManager ota;
#endif

// Activity counters for display.
unsigned long txCount = 0;  // pack / poll replies
unsigned long rxCount = 0;  // unpack / transmits received

// =============================================
// ====   Plumbing                          ====
// =============================================

WiFiServer                    server(CMRI_PORT);
WiFiClient                    client;
CMRInet::ClientTcpPort        port(client);
CMRInet::TcpCMRITransport     transport(port);
CMRInet::CMRINode             node(transport);
bool                          serverStarted = false;

void packInputs(CMRInet::IOBuffer& ib);
void unpackOutputs(CMRInet::IOBuffer& ob);
void sampleInputPorts(IOX_Config expanders[], uint8_t expanderCount,
                      uint8_t portState[][NodeDisplay::kPortsPerExpander]);

#if defined(USE_OTA)
static NodeDisplay::NetState netStateFor(OtaManager::State s) {
  switch (s) {
    case OtaManager::READY:
    case OtaManager::UPDATING:   return NodeDisplay::NET_READY;
    case OtaManager::CONNECTING: return NodeDisplay::NET_CONNECTING;
    case OtaManager::FAILED:     return NodeDisplay::NET_FAILED;
    default:                     return NodeDisplay::NET_OFF;
  }
}
#endif

// =============================================
// ====   Setup                             ====
// =============================================

void setup() {
  Wire.begin();  // default pins for the board (D4/D5 on cpNode-Xiao)
  oled.begin(NODE_NAME);  // degrades to headless on failure

  // Initialize expanders and derive geometry.
  // CPNODE card type has 2 phantom onboard bytes; IOX bytes follow.
  IOX_Geometry geom = ioxInit(expanders, kExpanderCount);

  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = NODE_ID;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = 2 + geom.inputBytes;   // 2 phantom + 2 IOX = 4 bytes (32 bits)
  cfg.outputBytes = 2 + geom.outputBytes;  // 2 phantom + 6 IOX = 8 bytes (64 bits)
  node.config(cfg);
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();

#ifdef USE_OTA
  ota.onStart = []() { oled.otaStart(); };
  ota.onProgress = [](unsigned int received, unsigned int total) {
    oled.otaProgress(received, total);
  };
  ota.onEnd = []() { oled.otaSuccess(); };
  ota.onError = [](const char* name) { oled.otaError(name); };
  ota.begin(NODE_NAME, WIFI_SSID, WIFI_PASSWORD);
#endif
}

// =============================================
// ====   Loop                              ====
// =============================================

void loop() {
#ifdef USE_OTA
  ota.poll();  // non-blocking WiFi + OTA lifecycle
#endif

  // Manage TCP listener once WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
    }
    // Accept incoming connection from JMRI if not already connected
    if (!client.connected()) {
      WiFiClient newClient = server.available();
      if (newClient) {
        client = newClient;
      }
    }
  }

  // Pump CMRI engine (reads TCP bytes, feeds decoder, handles packets)
  node.tick(millis());

  const uint32_t now = millis();
  if (now - lastDisplayMs >= kDisplayRefreshMs) {
    lastDisplayMs = now;
    // Sample inputs for local OLED display
    sampleInputPorts(expanders, kExpanderCount, portState);
    oled.update(expanders, kExpanderCount, portState);
    oled.setTX(txCount);
    oled.setRX(rxCount);
#ifdef USE_OTA
    oled.setNet(netStateFor(ota.state()), ota.ip());
#endif
    oled.show();
  }
  oled.serviceFlush();
}

// =============================================
// ====   Convenience routines              ====
// =============================================

void sampleInputPorts(IOX_Config expanders[], uint8_t expanderCount,
                      uint8_t portState[][NodeDisplay::kPortsPerExpander]) {
  for (uint8_t e = 0; e < expanderCount; e++) {
    if (expanders[e].portA == IN) {
      portState[e][0] = ioxReadPort(expanders[e].address, true);
    }
    if (expanders[e].portB == IN) {
      portState[e][1] = ioxReadPort(expanders[e].address, false);
    }
  }
}

void packInputs(CMRInet::IOBuffer& ib) {
  txCount++;
  sampleInputPorts(expanders, kExpanderCount, portState);

  ib.setByte(0, 0);  // phantom onboard byte
  ib.setBit(0, 2, (digitalRead(D2) == LOW));  // active-low button if present
  ib.setByte(1, 0);  // phantom onboard byte
  uint8_t idx = 2;
  for (uint8_t e = 0; e < kExpanderCount && idx < ib.length(); e++) {
    if (expanders[e].portA == IN) {
      ib.setByte(idx++, portState[e][0]);
    }
    if (expanders[e].portB == IN && idx < ib.length()) {
      ib.setByte(idx++, portState[e][1]);
    }
  }
}

void unpackOutputs(CMRInet::IOBuffer& ob) {
  rxCount++;
  digitalWrite(LED_BUILTIN, ob.getBit(0, 1) ? HIGH : LOW);

  uint8_t idx = 2;  // skip phantom onboard bytes
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
