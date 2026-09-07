# [CMRInet](README.md)
| **Installation** | [Hardware Details](README-hardware.md)  | [Tutorials](README-tutorials.md)  | [API Documentation](README-api.md) |  [CMRInet Protocol Details](README-protocol.md)  | [References](README-references.md) |

# Installing CMRInet

## You will need

- Arduino IDE 1.6.2 or later.
- A cpNode-Xiao board or equivalent. See
  [README-hardware.md](README-hardware.md).
- For Nodes: MCP23017-based 16-bit I2C expanders such as the
  cpNode-IOX, MRCS' IOX-16 and IOX-32, or generic breakout boards. A
  single Node supports up to 8 expander devices.
- For the OLED examples: the `Adafruit SSD1306` and `Adafruit GFX`
  libraries. To compile without the display, comment out `USE_OLED` at
  the top of the sketch.
- For WiFi-based over-the-air firmware updates: the `ArduinoOTA`
  library. To compile without WiFi OTA, comment out `USE_OTA` at the
  top of the sketch.

## Install

1. Install the Espressif board package.
   - File > Preferences: add
     `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
     to the list of additional board manager URLs.
   - Tools > Board > Boards Manager: install the `esp32` package.
   - Select the "Seeed Studio XIAO ESP32C6" board.
2. Install the display libraries if your sketch uses the OLED.
   - Library Manager: install `Adafruit SSD1306` and `Adafruit GFX`.
   - `ArduinoOTA` ships with the ESP32 core. You do not install it.
3. Get the CMRInet library.
```
Linux/MacOS
   git clone https://github.com/plocher/CMRInet ~/Documents/Arduino/libraries/CMRInet

Windows
   git clone https://github.com/plocher/CMRInet Documents\Arduino\libraries\CMRInet
```
4. Restart the Arduino IDE. Verify the install: the CMRInet examples
   appear under File > Examples > CMRInet, and "Seeed Studio XIAO
   ESP32C6" appears in the board list.

## Serial settings

The examples transmit at 28800 baud, 8 data bits, no parity, two stop
bits (8N2). If your Host is JMRI, match both ends before you expect
traffic. JMRI's CMRI default is 19200 baud, but can easily be changed.

Next: [README-tutorials.md](README-tutorials.md) — run your first Node.
