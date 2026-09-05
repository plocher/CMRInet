// display.h — SSD1306 OLED status panel for TracerHost (#11).
//
// Wraps the shared HostStatusPanel (SimpleHostMetrics.h) the SimpleHost
// example also uses: polling cadence, per-node state/latency, and two
// operator-settable annotation lines (`display <1|2> <text>`, #64).
// Degrades to headless if the panel never initializes (ok() stays
// false; every other call becomes a no-op). Single-consumer today
// (this sketch only) -- kept as its own file for readability, not
// reuse: TracerHost.ino was pushing 900 lines before this split.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "CMRIHost.h"
#include "SimpleHostMetrics.h"      // shared HostStatusPanel
#include "Ssd1306SegmentedFlush.h"  // non-blocking OLED push

class TracerDisplay {
 public:
  static constexpr uint32_t kRefreshMs = 150;

  /// Wire.begin() + panel init. Returns ok().
  bool begin();
  bool ok() const { return ok_; }

  /// Set one operator annotation line (1 or 2); ignored otherwise. Takes
  /// effect on the next render() (within kRefreshMs; no human-perceptible
  /// reason to force an earlier redraw for a text annotation).
  void setAnnotation(int lineNumber, const char* text);

  /// Redraw at most once per kRefreshMs; call every loop().
  void render(CMRInet::CMRIHost& host, uint32_t nowMs);

  /// Drain one segmented I2C chunk; call every loop().
  void service();

 private:
  static constexpr int kScreenW = 128;
  static constexpr int kScreenH = 64;
  static constexpr int kScreenAddr = 0x3C;
  static constexpr size_t kMaxRows = 4;

  Adafruit_SSD1306 display_{kScreenW, kScreenH, &Wire, -1};
  CMRInet::examples::Ssd1306SegmentedFlush flush_{display_, kScreenAddr};
  CMRInet::examples::HostStatusPanel panel_;
  bool ok_ = false;
  uint32_t lastRenderMs_ = 0;
  char line1_[22] = {0};
  char line2_[22] = {0};
};
