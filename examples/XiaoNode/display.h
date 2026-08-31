// display.h — SSD1306 OLED status panel for the XiaoNode example.
//
// Shows the node's UA, poll/transmit counters, output image hex,
// and WiFi/OTA status. During a firmware update the screen switches
// to a progress bar, then success or failure. An error screen holds
// ~5 s, then live view resumes.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

/// Initialize the OLED. Call from setup() after Wire.begin().
void displayInit();

/// Draw the live node status panel: UA, counters, output hex,
/// and WiFi/OTA state (if USE_OTA). Throttled by displayRefresh().
void drawNodeStatus(uint8_t ua, uint32_t polls, uint32_t txs,
                     const uint8_t* outputs, size_t outLen);

/// Refresh the display if the refresh interval has elapsed.
/// Call from loop(). Returns true if a redraw happened.
bool displayRefresh();

// ---- OTA screen callbacks (wire to OtaManager hooks) ----

void displayOtaStart();
void displayOtaProgress(unsigned int received, unsigned int total);
void displayOtaSuccess();
void displayOtaError(const char* name);
