// display.cpp — SSD1306 OLED status panel. See display.h.

#include "display.h"

#ifdef USE_OTA
#include "ota.h"
#endif

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace {

Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledOk = false;
uint32_t lastRefreshMs = 0;

// OTA screen ownership: prevents drawNodeStatus from painting over
// an OTA result. An error screen holds ~5 s, then live view resumes.
enum class ScreenMode : uint8_t { LIVE, OTA, HOLD };
ScreenMode screenMode = ScreenMode::LIVE;
uint32_t holdUntilMs = 0;
uint8_t lastOtaPct = 255;

void drawCentered(const char* text, uint8_t y) {
  int16_t x = (128 - static_cast<int16_t>(strlen(text)) * 6) / 2;
  if (x < 0) x = 0;
  display.setCursor(static_cast<uint8_t>(x), y);
  display.print(text);
}

}  // namespace

void displayInit() {
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOk) {
    display.dim(true);
    display.setTextWrap(false);
  }
}

bool displayRefresh() {
  if (!oledOk || screenMode != ScreenMode::LIVE) return false;
  const uint32_t now = millis();
  if (now - lastRefreshMs < 100 && lastRefreshMs != 0) return false;
  lastRefreshMs = now;
  return true;  // caller should drawNodeStatus now
}

void drawNodeStatus(uint8_t ua, uint32_t polls, uint32_t txs,
                     const uint8_t* outputs, size_t outLen) {
  if (!oledOk || screenMode != ScreenMode::LIVE) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("NODE"));
  display.setTextSize(1);
  display.setCursor(60, 4);
  display.print(F("UA"));
  display.print(ua);

  display.setCursor(0, 20);
  display.print(F("P:"));
  display.print(static_cast<unsigned long>(polls));
  display.print(F(" T:"));
  display.print(static_cast<unsigned long>(txs));

  display.setCursor(0, 36);
  display.print(F("out:"));
  for (size_t i = 0; i < outLen; ++i) {
    if (outputs[i] < 0x10) display.print('0');
    display.print(outputs[i], HEX);
    display.print(' ');
  }

#ifdef USE_OTA
  display.setCursor(0, 56);
  extern OtaManager ota;
  switch (ota.state()) {
    case OtaManager::OFF:        break;
    case OtaManager::CONNECTING: display.print(F("WiFi ..."));     break;
    case OtaManager::READY:      display.print(ota.ip().toString());
                                 display.print(F(" OTA"));           break;
    case OtaManager::UPDATING:   display.print(F("UPDATING"));     break;
  }
#endif

  display.display();
}

void displayOtaStart() {
  if (!oledOk) return;
  screenMode = ScreenMode::OTA;
  lastOtaPct = 255;
}

void displayOtaProgress(unsigned int received, unsigned int total) {
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

void displayOtaSuccess() {
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

void displayOtaError(const char* name) {
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
