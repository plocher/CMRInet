// iox.h — MCP23017 I2C expander access for the XiaoNode example.
//
// Minimal register-level access via Wire.h — no external library
// dependency. The sketch owns the expander table (user config);
// ioxInit() reads it, initializes the ports, and reports the
// derived geometry (how many input/output bytes the table implies).

#pragma once

#include <Arduino.h>
#include <stdint.h>

enum Direction : uint8_t { UNUSED = 0, OUT = 1, IN = 2 };

struct IOX_Config {
  uint8_t   address;   // I2C address (0x20-0x27)
  Direction portA;     // GPIO 0-7  direction
  Direction portB;     // GPIO 8-15 direction
};

struct IOX_Geometry {
  uint8_t inputBytes;
  uint8_t outputBytes;
};

/// Initialize all ports in the table and return the derived geometry
/// (how many input/output bytes the table implies, excluding phantom
/// CPNODE bytes — the sketch adds those).
IOX_Geometry ioxInit(IOX_Config* table, uint8_t count);

/// Read one port (8 bits). isPortA selects port A (true) or B (false).
uint8_t ioxReadPort(uint8_t addr, bool isPortA);

/// Write one port (8 bits). isPortA selects port A (true) or B (false).
void ioxWritePort(uint8_t addr, bool isPortA, uint8_t val);
