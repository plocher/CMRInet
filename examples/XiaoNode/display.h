// display.h — SSD1306 OLED status panel for the XiaoNode example.
//
// Ported from the donor cpNode Xiao_I2C NodeDisplay: a pure renderer with
// no CMRI, IOX, WiFi, or OTA semantics of its own. Two screen groups:
//   - live view: expander bit grid + activity spinners + net status
//   - OTA views: progress bar / success / error (full-screen takeover)
// A screen-ownership latch keeps loop()'s show() from painting over an
// OTA result; an error screen holds ~5 s, then live view resumes.
// begin() failure degrades to headless: every call becomes a no-op.
//
// Live view layout (128x64):
//   Top:    NODE_NAME plus r/t spinners (poll pack / transmit unpack)
//   Middle: one row per expander, Port B left / Port A right, bits 7..0
//           left-to-right; filled=1 hollow=0; recent changes halo-boxed
//   Bottom: network status (WiFi spinner or IP + OTA)

#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <stdint.h>

// Grid dimensions: max I2C expanders (0x20–0x27) × ports per chip (A, B)
constexpr uint8_t DISP_ROWS = 8;
constexpr uint8_t DISP_COLS = 2;

class NodeDisplay {
 public:
  enum Dir : uint8_t { UNUSED = 0, OUT = 1, IN = 2 };
  enum NetState : uint8_t { NET_OFF = 0, NET_CONNECTING, NET_READY };

  /// Init; returns false and goes headless on failure (never hangs).
  bool begin(const char* name);

  // ---- live view state (dirty-flagged; cheap to call every loop) ----
  void setPort(uint8_t index, Dir dirA, uint8_t dataA, Dir dirB, uint8_t dataB);
  void setTX(unsigned long count);
  void setRX(unsigned long count);
  void setNet(NetState state, IPAddress ip);

  /// Render if anything changed and the live view owns the screen.
  void show();

  // ---- OTA screens (call from OTA hooks; take over the screen) ----
  void otaStart();
  void otaProgress(unsigned int received, unsigned int total);
  void otaSuccess();
  void otaError(const char* name);

 private:
  enum Mode : uint8_t { MODE_LIVE, MODE_OTA, MODE_HOLD };

  void render();
  void drawHeader();
  void drawGrid();
  void drawStatus();
  void drawPortCells(uint8_t x, uint8_t y, Dir dir, uint8_t val, uint8_t halo);
  void drawCentered(const char* text, uint8_t y);

  bool alive_ = false;

  Dir dirs_[DISP_ROWS][DISP_COLS] = {};
  uint8_t data_[DISP_ROWS][DISP_COLS] = {};
  uint8_t delta_[DISP_ROWS][DISP_COLS] = {};   // bits recently changed
  uint8_t haloAge_[DISP_ROWS][DISP_COLS] = {}; // highlight cycles left
  unsigned long txCount_ = 0;
  unsigned long rxCount_ = 0;
  uint8_t txFrame_ = 0;
  uint8_t rxFrame_ = 0;  // quantized spinner phases
  NetState net_ = NET_OFF;
  IPAddress ip_;

  bool dirty_ = true;
  Mode mode_ = MODE_LIVE;
  unsigned long holdUntil_ = 0;
  uint8_t lastPct_ = 255;  // OTA progress redraw throttle
  uint8_t anim_ = 0;       // status-line spinner frame

  const char* name_ = "";
  char spinner_[5] = "-\|/";
};
