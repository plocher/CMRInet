// jigdisplay.cpp — SSD1306 OLED screens for IoxJig. See jigdisplay.h.
#include "jigdisplay.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint8_t kScreenWidth = 128;
constexpr uint8_t kScreenHeight = 64;
constexpr uint8_t kScreenAddress = 0x3C;  // shares the expander I2C bus

Adafruit_SSD1306 oled(kScreenWidth, kScreenHeight);

// 6x8 base font: 21 chars per line; rows pitched at 10 px.
constexpr uint8_t kLineH = 10;

void line(uint8_t row, const char* text) {
  oled.setCursor(0, static_cast<uint8_t>(row * kLineH));
  oled.print(text);
}

// Donor grid cell band (XiaoNode display.cpp metrics): 8 cells, 4 px
// square, 6 px pitch, bits 7..0 left-to-right. Fault-map semantics:
// filled + halo-boxed = FAILED bit, hollow = healthy.
void faultCells(uint8_t x, uint8_t y, uint8_t mask) {
  for (uint8_t pos = 0; pos < 8; ++pos) {
    const uint8_t b = static_cast<uint8_t>(7 - pos);
    const uint8_t cx = static_cast<uint8_t>(x + pos * 6);
    if ((mask >> b) & 1u) {
      oled.fillRect(cx, y, 4, 4, SSD1306_WHITE);
      oled.drawRect(cx - 1, y - 1, 6, 6, SSD1306_WHITE);  // halo: look here
    } else {
      oled.drawRect(cx, y, 4, 4, SSD1306_WHITE);
    }
  }
}

}  // namespace

bool JigDisplay::begin() {
  ok_ = oled.begin(SSD1306_SWITCHCAPVCC, kScreenAddress);
  if (!ok_) return false;
  oled.clearDisplay();
  oled.dim(true);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  oled.display();
  return true;
}

void JigDisplay::drawHeader_(const char* title) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  line(0, title);
  oled.drawFastHLine(0, 9, kScreenWidth, SSD1306_WHITE);
}

void JigDisplay::showBoot(const char* version, uint16_t autoRunSecs) {
  if (!ok_) return;
  drawHeader_("IOX JIG");
  char buf[22];
  snprintf(buf, sizeof(buf), "v%s", version);
  line(1, buf);
  line(2, "cpNode-IOX validator");
  snprintf(buf, sizeof(buf), "auto-run in %us", static_cast<unsigned>(autoRunSecs));
  line(4, buf);
  line(5, "serial line = rerun");
  oled.display();
}

void JigDisplay::showInventory(const uint8_t* addrs, uint8_t count,
                               uint8_t unknown) {
  if (!ok_) return;
  drawHeader_("DISCOVERY");
  char buf[22];
  snprintf(buf, sizeof(buf), "expanders: %u", static_cast<unsigned>(count));
  line(1, buf);
  // Up to four addresses per line, two lines (the 0x20-0x27 window
  // holds at most eight chips).
  char l1[22];
  char l2[22];
  l1[0] = '\0';
  l2[0] = '\0';
  for (uint8_t i = 0; i < count; ++i) {
    char* dst = (i < 4) ? l1 : l2;
    const size_t len = strlen(dst);
    snprintf(dst + len, sizeof(l1) - len, "%s%02X",
             (i % 4 == 0) ? "" : " ", static_cast<unsigned>(addrs[i]));
  }
  line(2, l1);
  line(3, l2);
  snprintf(buf, sizeof(buf), "unknown: %u", static_cast<unsigned>(unknown));
  line(4, buf);
  oled.display();
}

void JigDisplay::showBlock(uint8_t block, uint8_t blockCount, uint32_t i2cKhz,
                           uint32_t delayMs, uint16_t stepsDone,
                           uint16_t totalSteps, uint16_t faults,
                           const char* lastFault) {
  if (!ok_) return;
  drawHeader_("WALK");
  char buf[22];
  snprintf(buf, sizeof(buf), "blk %u/%u", static_cast<unsigned>(block),
           static_cast<unsigned>(blockCount));
  line(2, buf);
  snprintf(buf, sizeof(buf), "%ukHz d%ums", static_cast<unsigned>(i2cKhz),
           static_cast<unsigned>(delayMs));
  line(3, buf);
  snprintf(buf, sizeof(buf), "step %u/%u", static_cast<unsigned>(stepsDone),
           static_cast<unsigned>(totalSteps));
  line(4, buf);
  snprintf(buf, sizeof(buf), "faults %u", static_cast<unsigned>(faults));
  line(5, buf);
  if (lastFault != nullptr && lastFault[0] != '\0') {
    line(6, lastFault);  // setTextWrap(false) clips overlong summaries
  }
  oled.display();
}

void JigDisplay::showRefused(const char* reason) {
  if (!ok_) return;
  drawHeader_("MAP REFUSED");
  line(2, reason);
  line(4, "RED FAIL (setup)");
  oled.display();
}

void JigDisplay::showVerdict(bool green, uint16_t stepsRun, uint16_t faults,
                             const char* classNames, uint8_t chipCount,
                             const uint8_t (*faultMasks)[2],
                             const char* detail) {
  if (!ok_) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  if (green || chipCount == 0 || faultMasks == nullptr) {
    // Plain banner screen: GREEN, or RED without pair attribution
    // (setup faults — the serial tier names those).
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.print(green ? "GREEN OK" : "RED FAIL");
    oled.setTextSize(1);
    char buf[22];
    snprintf(buf, sizeof(buf), "steps %u", static_cast<unsigned>(stepsRun));
    line(3, buf);
    snprintf(buf, sizeof(buf), "faults %u", static_cast<unsigned>(faults));
    line(4, buf);
    if (!green) line(5, classNames);
    if (detail != nullptr && detail[0] != '\0') line(6, detail);
    oled.display();
    return;
  }
  // RED with fault map: donor node-grid idiom, static. One row per
  // chip (Port B left / Port A right, PCB order), failed bits filled +
  // halo-boxed. The row gap stays clear — chip addresses ride on the
  // bottom identification line instead.
  char buf[22];
  snprintf(buf, sizeof(buf), "RED FAIL f=%u", static_cast<unsigned>(faults));
  line(0, buf);
  const uint8_t rows = (chipCount > 8) ? 8 : chipCount;
  for (uint8_t i = 0; i < rows; ++i) {
    const uint8_t y = static_cast<uint8_t>(11 + i * 5);
    faultCells(0, y, faultMasks[i][1]);   // Port B left (PCB order)
    faultCells(64, y, faultMasks[i][0]);  // Port A right
  }
  // Bottom identification line: address(es) + worst pair LEFT (the
  // actionable info, always legible), class names right (clip
  // gracefully if combined).
  if (detail != nullptr && detail[0] != '\0') {
    oled.setCursor(0, 55);
    oled.print(detail);
  }
  oled.setCursor(72, 55);
  oled.print(classNames);
  oled.display();
}
