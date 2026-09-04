// display.cpp — SSD1306 OLED status panel. See display.h.
#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <string.h>

#include "Ssd1306SegmentedFlush.h"

namespace {

constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 64;
constexpr uint8_t SCREEN_ADDRESS = 0x3C;

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT);
CMRInet::examples::Ssd1306SegmentedFlush oledFlush(oled, SCREEN_ADDRESS);

// Live view layout metrics (128x64, 6x8 base font).
constexpr uint8_t FONT_W = 6;
constexpr uint8_t HEADER_Y = 0;
constexpr uint8_t GRID_TOP = 10;
constexpr uint8_t ROW_PITCH = 5;
constexpr uint8_t CELL_SIZE = 4;
constexpr uint8_t CELL_PITCH = 6;
constexpr uint8_t PORT_LEFT_X = 0;   // left port group (PCB: Port B)
constexpr uint8_t PORT_RIGHT_X = 60; // right port group (PCB: Port A)
constexpr uint8_t STATUS_Y = 56;

constexpr unsigned long OTA_ERROR_HOLD_MS = 5000;
// How many refresh cycles a changed bit stays highlighted (~100 ms each).
constexpr uint8_t CHANGE_HALO_CYCLES = 3;

void glyphI(uint8_t x, uint8_t y) {
  oled.drawPixel(x + 1, y, SSD1306_WHITE);
  oled.drawFastVLine(x + 1, y + 2, 2, SSD1306_WHITE);
}

void glyphO(uint8_t x, uint8_t y) {
  oled.drawPixel(x + 1, y, SSD1306_WHITE);
  oled.drawPixel(x, y + 1, SSD1306_WHITE);
  oled.drawPixel(x + 2, y + 1, SSD1306_WHITE);
  oled.drawPixel(x, y + 2, SSD1306_WHITE);
  oled.drawPixel(x + 2, y + 2, SSD1306_WHITE);
  oled.drawPixel(x + 1, y + 3, SSD1306_WHITE);
}

}  // namespace

void NodeDisplay::drawHeader() {
  oled.setCursor(0, HEADER_Y);
  oled.print(name_);

  // Quantized spinners: at most one step per refresh (see setTX/setRX).
  oled.setCursor(96, HEADER_Y);
  oled.printf("r%c", spinner_[txFrame_ % (sizeof(spinner_) - 1)]);
  oled.setCursor(114, HEADER_Y);
  oled.printf("t%c", spinner_[rxFrame_ % (sizeof(spinner_) - 1)]);
}

void NodeDisplay::drawPortCells(uint8_t x, uint8_t y, Dir dir, uint8_t val,
                                uint8_t halo) {
  const uint8_t cellsX = x + 2 * CELL_SIZE;
  const uint8_t bandW = 8 * CELL_PITCH - (CELL_PITCH - CELL_SIZE);

  if (dir == UNUSED) {
    for (uint8_t dx = 0; dx < bandW; dx += 4) {
      oled.drawFastHLine(cellsX + dx, y + CELL_SIZE / 2, 2, SSD1306_WHITE);
    }
    return;
  }

  if (dir == IN) {
    glyphI(x, y);
  } else {
    glyphO(x, y);
  }

  // PCB order: bit 7 leftmost .. bit 0 rightmost.
  for (uint8_t pos = 0; pos < 8; pos++) {
    const uint8_t b = static_cast<uint8_t>(7 - pos);
    const uint8_t cx = static_cast<uint8_t>(cellsX + pos * CELL_PITCH);
    if ((val >> b) & 1u) {
      oled.fillRect(cx, y, CELL_SIZE, CELL_SIZE, SSD1306_WHITE);
    } else {
      oled.drawRect(cx, y, CELL_SIZE, CELL_SIZE, SSD1306_WHITE);
    }
    if ((halo >> b) & 1u) {
      oled.drawRect(cx - 1, y - 1, CELL_SIZE + 2, CELL_SIZE + 2, SSD1306_WHITE);
    }
  }
}

void NodeDisplay::drawGrid() {
  // PCB/wiring order: Port B left, Port A right. Only the sketch-supplied
  // visible count is drawn; storage remains fixed at kMaxExpanders.
  for (uint8_t e = 0; e < visibleCount_; e++) {
    const uint8_t y = static_cast<uint8_t>(GRID_TOP + e * ROW_PITCH);
    drawPortCells(PORT_LEFT_X, y, dirs_[e][1], data_[e][1], delta_[e][1]);
    drawPortCells(PORT_RIGHT_X, y, dirs_[e][0], data_[e][0], delta_[e][0]);
  }
}

void NodeDisplay::drawStatus() {
  oled.setCursor(0, STATUS_Y);
  switch (net_) {
    case NET_OFF:
      break;
    case NET_CONNECTING:
      oled.printf("WiFi %c", spinner_[anim_++ % (sizeof(spinner_) - 1)]);
      break;
    case NET_READY:
      oled.printf("%s OTA", ip_.toString().c_str());
      break;
    case NET_FAILED:
      // Keep the same "WiFi" lead-in as CONNECTING so the eye lands
      // on the same corner; message must fit a 21-char 6px line.
      oled.print(F("WiFi: can't connect"));
      break;
  }
}

void NodeDisplay::render() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  drawHeader();
  drawGrid();
  drawStatus();
  oledFlush.markDirty();
}

bool NodeDisplay::begin(const char* name) {
  name_ = name;

  alive_ = oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  if (!alive_) {
    return false;
  }

  oled.clearDisplay();
  oled.dim(true);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);
  drawHeader();
  oledFlush.markDirty();
  oledFlush.serviceUntilIdle();
  dirty_ = true;
  return true;
}

void NodeDisplay::update(const IOX_Config* expanders, uint8_t count,
                         const uint8_t portState[][kPortsPerExpander]) {
  if (!alive_ || expanders == nullptr || portState == nullptr) {
    return;
  }
  const uint8_t n = (count > kMaxExpanders) ? kMaxExpanders : count;
  if (n != visibleCount_) {
    visibleCount_ = n;
    dirty_ = true;
  }
  for (uint8_t e = 0; e < visibleCount_; e++) {
    // Direction and Dir share ordinals (UNUSED/OUT/IN); map once here so
    // the sketch never casts.
    setPort(e, static_cast<Dir>(expanders[e].portA), portState[e][0],
            static_cast<Dir>(expanders[e].portB), portState[e][1]);
  }
}

void NodeDisplay::setPort(uint8_t index, Dir dirA, uint8_t dataA, Dir dirB,
                          uint8_t dataB) {
  if (!alive_ || index >= visibleCount_) {
    return;
  }

  const uint8_t diffA = static_cast<uint8_t>(dataA ^ data_[index][0]);
  const uint8_t diffB = static_cast<uint8_t>(dataB ^ data_[index][1]);

  if (dirs_[index][0] != dirA || dirs_[index][1] != dirB || diffA || diffB) {
    dirs_[index][0] = dirA;
    data_[index][0] = dataA;
    dirs_[index][1] = dirB;
    data_[index][1] = dataB;

    if (diffA) {
      delta_[index][0] |= diffA;
      haloAge_[index][0] = CHANGE_HALO_CYCLES;
    }
    if (diffB) {
      delta_[index][1] |= diffB;
      haloAge_[index][1] = CHANGE_HALO_CYCLES;
    }
    dirty_ = true;
  }
}

void NodeDisplay::setTX(unsigned long count) {
  if (!alive_ || count == txCount_) {
    return;
  }
  txCount_ = count;
  txFrame_++;
  dirty_ = true;
}

void NodeDisplay::setRX(unsigned long count) {
  if (!alive_ || count == rxCount_) {
    return;
  }
  rxCount_ = count;
  rxFrame_++;
  dirty_ = true;
}

void NodeDisplay::setNet(NetState state, IPAddress ip) {
  if (!alive_) {
    return;
  }
  if (state != net_ || !(ip == ip_)) {
    net_ = state;
    ip_ = ip;
    dirty_ = true;
  }
  if (state == NET_CONNECTING) {
    dirty_ = true;  // keep the spinner animating
  }
}

void NodeDisplay::show() {
  if (!alive_) {
    return;
  }

  if (mode_ == MODE_OTA) {
    return;
  }
  if (mode_ == MODE_HOLD) {
    if (millis() < holdUntil_) {
      return;
    }
    mode_ = MODE_LIVE;
    dirty_ = true;
  }

  // Age change highlights; keep rendering while any are visible so
  // expired halos get erased.
  for (uint8_t e = 0; e < visibleCount_; e++) {
    for (uint8_t p = 0; p < kPortsPerExpander; p++) {
      if (haloAge_[e][p]) {
        if (--haloAge_[e][p] == 0) {
          delta_[e][p] = 0;
        }
        dirty_ = true;
      }
    }
  }

  if (!dirty_) {
    return;
  }
  render();
  dirty_ = false;
}

void NodeDisplay::drawCentered(const char* text, uint8_t y) {
  int16_t x =
      static_cast<int16_t>((SCREEN_WIDTH - static_cast<int16_t>(strlen(text)) * FONT_W) / 2);
  if (x < 0) {
    x = 0;
  }
  oled.setCursor(static_cast<uint8_t>(x), y);
  oled.print(text);
}

void NodeDisplay::otaStart() {
  if (!alive_) {
    return;
  }
  mode_ = MODE_OTA;
  lastPct_ = 255;
  otaProgress(0, 1);
}

void NodeDisplay::otaProgress(unsigned int received, unsigned int total) {
  if (!alive_) {
    return;
  }
  mode_ = MODE_OTA;

  const uint8_t pct = (total > 0)
      ? static_cast<uint8_t>(static_cast<uint64_t>(received) * 100 / total)
      : 0;
  if (pct == lastPct_) {
    return;
  }
  lastPct_ = pct;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  drawCentered("FIRMWARE UPDATE", 4);

  constexpr uint8_t barX = 13;
  constexpr uint8_t barY = 22;
  constexpr uint8_t barW = 102;
  constexpr uint8_t barH = 12;
  oled.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  if (pct > 0) {
    oled.fillRect(barX + 1, barY + 1, pct, barH - 2, SSD1306_WHITE);
  }

  char buf[24];
  snprintf(buf, sizeof(buf), "%u%%", pct);
  drawCentered(buf, 40);
  snprintf(buf, sizeof(buf), "%u / %u KB", received / 1024, total / 1024);
  drawCentered(buf, 52);
  oledFlush.markDirty();
  oledFlush.serviceUntilIdle();
}

void NodeDisplay::otaSuccess() {
  if (!alive_) {
    return;
  }
  mode_ = MODE_OTA;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.drawCircle(64, 20, 12, SSD1306_WHITE);
  oled.drawLine(58, 20, 62, 25, SSD1306_WHITE);
  oled.drawLine(62, 25, 70, 15, SSD1306_WHITE);
  drawCentered("UPDATE OK", 40);
  drawCentered("rebooting...", 52);
  oledFlush.markDirty();
  oledFlush.serviceUntilIdle();
}

void NodeDisplay::otaError(const char* name) {
  if (!alive_) {
    return;
  }

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.drawCircle(64, 20, 12, SSD1306_WHITE);
  oled.drawLine(58, 14, 70, 26, SSD1306_WHITE);
  oled.drawLine(70, 14, 58, 26, SSD1306_WHITE);
  drawCentered("UPDATE FAILED", 38);
  drawCentered(name, 48);
  oledFlush.markDirty();
  oledFlush.serviceUntilIdle();

  mode_ = MODE_HOLD;
  holdUntil_ = millis() + OTA_ERROR_HOLD_MS;
}

void NodeDisplay::serviceFlush() {
  if (!alive_) {
    return;
  }
  oledFlush.service();
}
