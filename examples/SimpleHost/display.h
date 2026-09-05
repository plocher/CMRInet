// display.h — SSD1306 OLED status panel for SimpleHost.
//
// Mirrors TracerHost's TracerDisplay (examples/TracerHost/display.h):
// one class owning its own OLED state (begin()/ok()/render()/service())
// instead of loose globals and a free function reading them. SimpleHost
// has no C&C annotation verbs, so unlike TracerDisplay this has no
// setAnnotation() -- just the health panel and a one-shot fatal-error
// screen for a rejected compile-time node table.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "CMRIHost.h"
#include "NodeInit.h"               // HostNodeSpec
#include "SimpleHostMetrics.h"      // shared HostStatusPanel
#include "Ssd1306SegmentedFlush.h"  // non-blocking OLED push

class SimpleDisplay {
 public:
  static constexpr uint32_t kRefreshMs = 120;

  /// Wire.begin() + panel init. Returns ok().
  bool begin();
  bool ok() const { return ok_; }

  /// Redraw at most once per kRefreshMs; call every loop(). `nodeTable`/
  /// `nodeCount` are the sketch's own layout table (SimpleHost's node set
  /// is fixed at compile time, unlike TracerHost's C&C-driven one, so it
  /// is simply passed in rather than discovered by scanning the host).
  void render(CMRInet::CMRIHost& host, const CMRInet::HostNodeSpec* nodeTable,
              size_t nodeCount, uint32_t nowMs);

  /// One-shot fatal-error screen (e.g. a rejected node table at setup()).
  /// Blocks until the OLED has fully flushed, since the caller parks
  /// forever right after calling this.
  void showFatalError(const char* title, const char* detail);

  /// Drain one segmented I2C chunk; call every loop().
  void service();

 private:
  static constexpr int kScreenW = 128;
  static constexpr int kScreenH = 64;
  static constexpr int kScreenAddr = 0x3C;

  Adafruit_SSD1306 display_{kScreenW, kScreenH, &Wire, -1};
  CMRInet::examples::Ssd1306SegmentedFlush flush_{display_, kScreenAddr};
  CMRInet::examples::HostStatusPanel panel_;
  bool ok_ = false;
  uint32_t lastRenderMs_ = 0;
};
