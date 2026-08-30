// XiaoNode.ino — full-featured CMRINode with OLED and WiFi OTA.
//
// The rich Node example: same pack/unpack seam as SimpleNode, but
// driving MCP23017 I2C expanders for real layout I/O, with an SSD1306
// status panel and non-blocking WiFi OTA firmware updates.
//
// What it demonstrates:
// - The onPack/onUnpack callback pattern with real hardware (I2C expanders).
// - onPack reads input ports at P time; onUnpack writes output ports
//   at T time. The engine handles the protocol.
// - Feature toggles (USE_OLED, USE_OTA) that compile out cleanly.
// - OLED status panel with OTA progress/success/error screens.
// - No dependency on the cpNode library — MCP23017 access is inline.
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

#include "CMRInet.h"               // CMRINode, CMRINodeConfig
#include "transport/serial.h"      // SerialCMRITransport
#include "transport/serialESP32.h" // ESP32 hardware transmit-drain port

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

#ifdef USE_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

// ---- Configuration --------------------------------------------------------
#define NODE_ID 30    // 0...127, same as in JMRI
#define NODE_NAME "XiaoC6-" "30"

// =============================================
// ====   I/O Expander Configuration        ====
// =============================================
// Up to 8 MCP23017 expanders at I2C addresses 0x20-0x27.
// Each expander has two 8-bit ports (A=GPIO 0-7, B=GPIO 8-15).
enum Direction : uint8_t { UNUSED = 0, OUT = 1, IN = 2 };

struct IOX_Config {
  uint8_t   address;   // I2C address (0x20-0x27)
  Direction portA;     // GPIO 0-7  direction
  Direction portB;     // GPIO 8-15 direction
};

constexpr uint8_t DISP_ROWS = 8;

IOX_Config expanders[DISP_ROWS] = {
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
// ====   MCP23017 I2C routines (inline)    ====
// =============================================
// Minimal MCP23017 access via Wire.h — no external library dependency.
// Registers: IODIRA/B=0x00/0x01, IPOLA/B=0x02/0x03, GPIOA/B=0x12/0x13,
// OLATA/B=0x14/0x15.

namespace {

constexpr uint8_t MCP_IODIRA = 0x00;
constexpr uint8_t MCP_IODIRB = 0x01;
constexpr uint8_t MCP_IPOLA  = 0x02;
constexpr uint8_t MCP_IPOLB  = 0x03;
constexpr uint8_t MCP_GPIOA  = 0x12;
constexpr uint8_t MCP_GPIOB  = 0x13;
constexpr uint8_t MCP_OLATA  = 0x14;
constexpr uint8_t MCP_OLATB  = 0x15;

void mcpWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t mcpReadReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, static_cast<uint8_t>(1));
  if (Wire.available()) return Wire.read();
  return 0;
}

void mcpInitPort(uint8_t addr, Direction dir, bool isPortA) {
  const uint8_t iodir = isPortA ? MCP_IODIRA : MCP_IODIRB;
  const uint8_t ipol  = isPortA ? MCP_IPOLA  : MCP_IPOLB;
  if (dir == IN) {
    mcpWriteReg(addr, iodir, 0xFF);       // all pins input
    mcpWriteReg(addr, ipol,  0xFF);       // invert (active-low convention)
  } else if (dir == OUT) {
    mcpWriteReg(addr, iodir, 0x00);       // all pins output
  }
}

uint8_t mcpReadPort(uint8_t addr, bool isPortA) {
  return mcpReadReg(addr, isPortA ? MCP_GPIOA : MCP_GPIOB);
}

void mcpWritePort(uint8_t addr, bool isPortA, uint8_t val) {
  mcpWriteReg(addr, isPortA ? MCP_OLATA : MCP_OLATB, val);
}

}  // namespace

// =============================================
// ====   Derived geometry                  ====
// =============================================
// CPNODE card type has 2 phantom onboard bytes; IOX bytes follow.
uint8_t inputBytes  = 2;
uint8_t outputBytes = 2;

void initExpanders() {
  for (uint8_t e = 0; e < DISP_ROWS; e++) {
    if (expanders[e].portA != UNUSED) {
      mcpInitPort(expanders[e].address, expanders[e].portA, true);
      if (expanders[e].portA == IN)  inputBytes++;
      if (expanders[e].portA == OUT) outputBytes++;
    }
    if (expanders[e].portB != UNUSED) {
      mcpInitPort(expanders[e].address, expanders[e].portB, false);
      if (expanders[e].portB == IN)  inputBytes++;
      if (expanders[e].portB == OUT) outputBytes++;
    }
  }
}

// ---- Wiring (static; the library never allocates) -------------------------
CMRInet::Esp32SerialPort port(Serial1, D3, 28800, RX, TX);
CMRInet::SerialCMRITransport    transport(port);
CMRInet::CMRINode               node(transport);

CMRInet::CMRINodeConfig makeConfig() {
  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = NODE_ID;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = inputBytes;
  cfg.outputBytes = outputBytes;
  return cfg;
}

#ifdef USE_OTA
OtaManager ota;
#endif

// ---- Pack / Unpack seam ---------------------------------------------------
// onPack is called at P time: read all input ports into IB.
// onUnpack is called at T time: write all output ports from OB.
// Both skip the 2 phantom CPNODE bytes at the start of the image.

uint32_t pollCount  = 0;
uint32_t txCount    = 0;

void packInputs(void*, uint8_t* ib, size_t len) {
  pollCount++;
  ib[0] = 0;  // phantom onboard byte
  ib[1] = 0;  // phantom onboard byte
  uint8_t idx = 2;
  for (uint8_t e = 0; e < DISP_ROWS && idx < len; e++) {
    if (expanders[e].portA == IN) {
      ib[idx++] = mcpReadPort(expanders[e].address, true);
    }
    if (expanders[e].portB == IN && idx < len) {
      ib[idx++] = mcpReadPort(expanders[e].address, false);
    }
  }
}

void unpackOutputs(void*, const uint8_t* ob, size_t len) {
  txCount++;
  uint8_t idx = 2;  // skip phantom onboard bytes
  for (uint8_t e = 0; e < DISP_ROWS && idx < len; e++) {
    if (expanders[e].portA == OUT) {
      mcpWritePort(expanders[e].address, true, ob[idx++]);
    }
    if (expanders[e].portB == OUT && idx < len) {
      mcpWritePort(expanders[e].address, false, ob[idx++]);
    }
  }
}

// =============================================
// ====   OLED display                      ====
// =============================================
#ifdef USE_OLED

constexpr int      kScreenW    = 128;
constexpr int      kScreenH    = 64;
constexpr int      kScreenAddr = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 100;

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;

// OTA screen ownership: prevents loop()'s status draw from painting
// over an OTA result. An error screen holds ~5 s, then live view resumes.
enum class ScreenMode : uint8_t { LIVE, OTA, HOLD };
ScreenMode screenMode = ScreenMode::LIVE;
uint32_t holdUntilMs = 0;
uint8_t lastOtaPct = 255;

void drawCentered(const char* text, uint8_t y) {
  int16_t x = (kScreenW - static_cast<int16_t>(strlen(text)) * 6) / 2;
  if (x < 0) x = 0;
  display.setCursor(static_cast<uint8_t>(x), y);
  display.print(text);
}

void drawStatus() {
  if (!oledOk || screenMode != ScreenMode::LIVE) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("NODE"));
  display.setTextSize(1);
  display.setCursor(60, 4);
  display.print(F("UA"));
  display.print(node.UA());

  // Activity counters
  display.setCursor(0, 20);
  display.print(F("P:"));
  display.print(static_cast<unsigned long>(pollCount));
  display.print(F(" T:"));
  display.print(static_cast<unsigned long>(txCount));

  // Output image hex
  display.setCursor(0, 36);
  display.print(F("out:"));
  for (size_t i = 0; i < node.outputLength(); ++i) {
    if (node.outputByte(i) < 0x10) display.print('0');
    display.print(node.outputByte(i), HEX);
    display.print(' ');
  }

#ifdef USE_OTA
  display.setCursor(0, 56);
  switch (ota.state()) {
    case OtaManager::OFF:
      break;
    case OtaManager::CONNECTING:
      display.print(F("WiFi ..."));
      break;
    case OtaManager::READY:
      display.print(ota.ip().toString());
      display.print(F(" OTA"));
      break;
    case OtaManager::UPDATING:
      display.print(F("UPDATING"));
      break;
  }
#endif

  display.display();
}

void otaStart() {
  if (!oledOk) return;
  screenMode = ScreenMode::OTA;
  lastOtaPct = 255;
}

void otaProgress(unsigned int received, unsigned int total) {
  if (!oledOk) return;
  screenMode = ScreenMode::OTA;
  uint8_t pct = (total > 0)
      ? static_cast<uint8_t>(static_cast<uint64_t>(received) * 100 / total)
      : 0;
  if (pct == lastOtaPct) return;
  lastOtaPct = pct;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  drawCentered("FIRMWARE UPDATE", 4);
  display.drawRect(13, 22, 102, 12, SSD1306_WHITE);
  if (pct > 0) display.fillRect(14, 23, pct, 10, SSD1306_WHITE);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u%%", pct);
  drawCentered(buf, 40);
  display.display();
}

void otaSuccess() {
  if (!oledOk) return;
  screenMode = ScreenMode::OTA;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.drawCircle(64, 20, 12, SSD1306_WHITE);
  display.drawLine(58, 20, 62, 25, SSD1306_WHITE);
  display.drawLine(62, 25, 70, 15, SSD1306_WHITE);
  drawCentered("UPDATE OK", 40);
  drawCentered("rebooting...", 52);
  display.display();
}

void otaError(const char* name) {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.drawCircle(64, 20, 12, SSD1306_WHITE);
  display.drawLine(58, 14, 70, 26, SSD1306_WHITE);
  display.drawLine(70, 14, 58, 26, SSD1306_WHITE);
  drawCentered("UPDATE FAILED", 38);
  drawCentered(name, 48);
  display.display();
  screenMode = ScreenMode::HOLD;
  holdUntilMs = millis() + 5000;
}

#endif  // USE_OLED

// =============================================
// ====   Setup                             ====
// =============================================
void setup() {
  Wire.begin(D4 /* SDA */, D5 /* SCL */);

#ifdef USE_OLED
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
    display.setTextWrap(false);
  }
#endif

  // Derive geometry from the expander table before configuring the node.
  initExpanders();

  // Configure the node with the derived geometry.
  node.config(makeConfig());
  node.onPack(packInputs);
  node.onUnpack(unpackOutputs);
  node.begin();  // → transport.begin() → port.begin() → Serial1.begin()

#ifdef USE_OTA
#ifdef USE_OLED
  ota.onStart    = otaStart;
  ota.onProgress = otaProgress;
  ota.onEnd      = otaSuccess;
  ota.onError    = otaError;
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

  // Release hold after OTA error
  if (screenMode == ScreenMode::HOLD && now >= holdUntilMs) {
    screenMode = ScreenMode::LIVE;
  }

  if (now - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawStatus();
    lastDisplayMs = now;
  }
#endif
}
