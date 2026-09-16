// display.h — SSD1306 OLED status panel for the XiaoNode example.
//
// Ported from the donor cpNode Xiao_I2C NodeDisplay: a pure renderer with
// no CMRI, IOX, WiFi, or OTA semantics of its own. Two screen groups:
//   - live view: bit grid + activity spinners + net status
//   - OTA views: progress bar / success / error (full-screen takeover)
// A screen-ownership latch keeps loop()'s show() from painting over an
// OTA result; an error screen holds ~5 s, then live view resumes.
// begin() failure degrades to headless: every call becomes a no-op.
//
// Live view layout (128x64):
//   Top:    NODE_NAME plus r/t spinners (poll pack / transmit unpack)
//   Middle: up to kMaxExpanders port-pair rows (sketch says how many);
//           Port B left / Port A right, bits 7..0 L→R; filled=1 hollow=0;
//           recent changes halo-boxed.
//   Bottom: network status (WiFi spinner or IP + OTA)
//
// Memory is fixed at kMaxExpanders (=8 for this 128x64 layout). The
// sketch owns expander quantity (sizeof expanders[]) and hands the table
// plus bit cache to update(); count is clamped at the max. The sketch
// does not cast directions or walk ports for the panel.

#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <stdint.h>

#include "iox.h"

class NodeDisplay {
 public:
  enum NetState : uint8_t {
    NET_OFF = 0,
    NET_CONNECTING,
    NET_READY,
    NET_FAILED,  // initial join timed out / bad credentials
  };

  /// Fixed storage ceiling for this panel layout (not the sketch's table).
  static constexpr uint8_t kMaxExpanders = 8;
  static constexpr uint8_t kPortsPerExpander = 2;  // A, B

  /// Init; returns false and goes headless on failure (never hangs).
  bool begin(const char* name);

  // ---- live view state (dirty-flagged; cheap to call every loop) ----
  /// Snapshot the expander table and its port bit cache. `count` is the
  /// sketch's expander quantity (sizeof); clamped to kMaxExpanders.
  /// `portState[i][0]` is port A, `portState[i][1]` is port B.
  void update(const IOX_Config* expanders, uint8_t count,
              const uint8_t portState[][kPortsPerExpander]);
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

  /// Drain one segmented I2C chunk; call once per loop().
  void serviceFlush();

 private:
  // Same ordinals as iox::Direction; private so the sketch never casts.
  enum Dir : uint8_t { UNUSED = 0, OUT = 1, IN = 2 };
  enum Mode : uint8_t { MODE_LIVE, MODE_OTA, MODE_HOLD };

  void setPort(uint8_t index, Dir dirA, uint8_t dataA, Dir dirB, uint8_t dataB);
  void render();
  void drawHeader();
  void drawGrid();
  void drawStatus();
  void drawPortCells(uint8_t x, uint8_t y, Dir dir, uint8_t val, uint8_t halo);
  void drawCentered(const char* text, uint8_t y);

  bool alive_ = false;
  uint8_t visibleCount_ = 0;  // rows to draw; <= kMaxExpanders

  // Fixed max-8 storage; only the first visibleCount_ slots are live.
  Dir dirs_[kMaxExpanders][kPortsPerExpander] = {};
  uint8_t data_[kMaxExpanders][kPortsPerExpander] = {};
  uint8_t delta_[kMaxExpanders][kPortsPerExpander] = {};   // bits recently changed
  uint8_t haloAge_[kMaxExpanders][kPortsPerExpander] = {}; // highlight cycles left
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
  char spinner_[5] = "-\\|/";
};
