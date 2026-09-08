// jigdisplay.h — SSD1306 OLED screens for IoxJig (issue #32).
//
// Tier 0 surface: the operator-side view of a run. Blocking draws are
// fine here — the jig has no protocol timing to disturb (the #11
// OLED-stall lesson is CMRI-loop-specific). Like the node examples'
// NodeDisplay, this degrades to headless when no OLED answers at
// begin(); every screen is then a silent no-op and the serial tier
// carries the whole story alone.
//
// Boundary doctrine: screens are drawn at run BOUNDARIES only — boot,
// inventory, rate-block starts, verdict — never inside a block, so
// OLED bus traffic never interleaves with expander transactions and an
// OLED failure (the SSD1306 is out of spec above 400 kHz) can never
// influence expander results. A draw that fails at a high-rate
// boundary just means a less frequent picture.

#pragma once

#include <Arduino.h>
#include <stdint.h>

class JigDisplay {
 public:
  /// Probe and initialize the panel. False when absent — the run
  /// continues headless.
  bool begin();

  /// True when the panel is live.
  bool ok() const { return ok_; }

  /// Boot screen: identity + auto-run countdown hint.
  void showBoot(const char* version, uint16_t autoRunSecs);

  /// Discovery result: expander addresses found + unknown responder count.
  void showInventory(const uint8_t* addrs, uint8_t count, uint8_t unknown);

  /// Rate-block boundary screen: which block, at what pace/clock, and
  /// the cumulative progress so far. `lastFault` is the most recent
  /// fault summary ("" for none) — faults are shown at boundaries,
  /// never live.
  void showBlock(uint8_t block, uint8_t blockCount, uint32_t i2cKhz,
                 uint32_t delayMs, uint16_t stepsDone, uint16_t totalSteps,
                 uint16_t faults, const char* lastFault);

  /// Refused map: setup verdict without a walk.
  void showRefused(const char* reason);

  /// Final verdict screen. GREEN (or RED with no fault map): big
  /// banner + counters. RED with a fault map (chipCount > 0): the
  /// donor node grid idiom — one row per chip, Port B left / Port A
  /// right, bits 7..0 left-to-right; FAILED bits filled and halo-boxed,
  /// healthy bits hollow. Bottom identification line: chip address(es)
  /// + worst pair as `detail` (left, e.g. "20 p6 A6-B6"), class names
  /// (right). Static — drawn once at the verdict boundary, no
  /// animation.
  void showVerdict(bool green, uint16_t stepsRun, uint16_t faults,
                   const char* classNames, uint8_t chipCount,
                   const uint8_t (*faultMasks)[2], const char* detail);

 private:
  void drawHeader_(const char* title);

  bool ok_ = false;
};
