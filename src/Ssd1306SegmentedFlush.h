// Ssd1306SegmentedFlush.h — non-blocking SSD1306 framebuffer push.
//
// Adafruit_SSD1306::display() always streams the full 1024-byte
// framebuffer over I2C (~20-25 ms), which straddles CMRI exchanges and
// corrupts Host reply-gate / turnaround measurements. This helper pushes
// the same buffer in small Wire chunks (default 32 bytes) across loop()
// iterations so no single tick blocks more than a couple of milliseconds.
//
// Pattern (every OLED sketch):
//   1. Paint into the Adafruit framebuffer (clearDisplay / print / ...).
//   2. flush.markDirty() instead of display.display().
//   3. Call flush.service() once per loop() — drains one chunk if dirty.
//
// Shared by SimpleHost, XiaoHostTracer, XiaoSniffer, and XiaoNode so the
// segmented path cannot drift sketch-to-sketch again.
//
// Arduino-only: desktop tests never include this header.

#pragma once

#if defined(ARDUINO)

#include <stddef.h>
#include <stdint.h>

#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace CMRInet {
namespace examples {

/// Incremental I2C push of an Adafruit_SSD1306 framebuffer.
class Ssd1306SegmentedFlush {
 public:
  static constexpr size_t kDefaultChunkBytes = 32;
  static constexpr size_t kFrameBytes128x64 = (128 * 64) / 8;  // 1024

  /// `display` and `i2cAddress` must outlive this object.
  /// `chunkBytes` is the payload per service() call (plus the 0x40 D/C byte).
  explicit Ssd1306SegmentedFlush(Adafruit_SSD1306& display,
                                 uint8_t i2cAddress = 0x3C,
                                 size_t chunkBytes = kDefaultChunkBytes,
                                 size_t frameBytes = kFrameBytes128x64)
      : display_(display),
        addr_(i2cAddress),
        chunkBytes_(chunkBytes == 0 ? kDefaultChunkBytes : chunkBytes),
        frameBytes_(frameBytes) {}

  /// Mark the framebuffer dirty. Next service() calls stream it out.
  /// Safe to call every redraw; restarts a push in progress.
  void markDirty() {
    dirty_ = true;
    offset_ = 0;
  }

  /// True while a push is incomplete.
  bool busy() const { return dirty_; }

  /// Send at most one chunk. Call once per loop() iteration.
  void service() {
    if (!dirty_) {
      return;
    }

    if (offset_ == 0) {
      // Column 0..127, page 0..end — full window, same as Adafruit display().
      Wire.beginTransmission(addr_);
      Wire.write(static_cast<uint8_t>(0x00));  // command
      Wire.write(static_cast<uint8_t>(0x21));
      Wire.write(static_cast<uint8_t>(0));
      Wire.write(static_cast<uint8_t>(127));
      Wire.endTransmission();

      Wire.beginTransmission(addr_);
      Wire.write(static_cast<uint8_t>(0x00));  // command
      Wire.write(static_cast<uint8_t>(0x22));
      Wire.write(static_cast<uint8_t>(0));
      Wire.write(static_cast<uint8_t>(0xFF));
      Wire.endTransmission();
    }

    const uint8_t* buf = display_.getBuffer();
    size_t end = offset_ + chunkBytes_;
    if (end > frameBytes_) {
      end = frameBytes_;
    }

    Wire.beginTransmission(addr_);
    Wire.write(static_cast<uint8_t>(0x40));  // data
    for (size_t i = offset_; i < end; ++i) {
      Wire.write(buf[i]);
    }
    Wire.endTransmission();

    offset_ = end;
    if (offset_ >= frameBytes_) {
      offset_ = 0;
      dirty_ = false;
    }
  }

  /// Optional: block until the current dirty frame is fully out.
  /// Prefer service() in loop(); this is for rare setup splash only.
  void serviceUntilIdle(uint32_t yieldMs = 0) {
    while (dirty_) {
      service();
      if (yieldMs != 0) {
        delay(yieldMs);
      }
    }
  }

 private:
  Adafruit_SSD1306& display_;
  uint8_t addr_;
  size_t chunkBytes_;
  size_t frameBytes_;
  bool dirty_ = false;
  size_t offset_ = 0;
};

}  // namespace examples
}  // namespace CMRInet

#endif  // ARDUINO
