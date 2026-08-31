// iox.cpp — MCP23017 I2C expander access. See iox.h.
//
// Register map and transaction shape match the donor cpNode IOX path
// (proven on this hardware): stop-then-requestFrom reads, GPIO for
// R/W, IODIR/GPPU/IPOL setup on inputs.

#include "iox.h"
#include <Wire.h>

namespace {

// MCP23017 register base addresses (IOCON.BANK = 0). Port B is base+1.
constexpr uint8_t MCP_IODIR   = 0x00;  // A=0x00, B=0x01
constexpr uint8_t MCP_IPOL    = 0x02;  // A=0x02, B=0x03  (active-low map)
constexpr uint8_t MCP_GPPU    = 0x0C;  // A=0x0C, B=0x0D
constexpr uint8_t MCP_GPIO    = 0x12;  // A=0x12, B=0x13

constexpr uint8_t PORT_INPUT     = 0xFF;
constexpr uint8_t PORT_OUTPUT    = 0x00;
constexpr uint8_t PORT_PULLUPS   = 0xFF;
constexpr uint8_t PORT_ACTIVELOW = 0xFF;

uint8_t portOffset(bool isPortA) { return isPortA ? 0 : 1; }

void initPort(uint8_t addr, Direction dir, bool isPortA) {
  const uint8_t port = portOffset(isPortA);
  if (dir == IN) {
    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(MCP_IODIR + port));
    Wire.write(PORT_INPUT);
    Wire.endTransmission();

    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(MCP_GPPU + port));
    Wire.write(PORT_PULLUPS);
    Wire.endTransmission();

    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(MCP_IPOL + port));
    Wire.write(PORT_ACTIVELOW);
    Wire.endTransmission();
  } else if (dir == OUT) {
    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(MCP_IODIR + port));
    Wire.write(PORT_OUTPUT);
    Wire.endTransmission();
  }
}

}  // namespace

IOX_Geometry ioxInit(IOX_Config* table, uint8_t count) {
  IOX_Geometry geom = {0, 0};
  for (uint8_t e = 0; e < count; e++) {
    if (table[e].portA != UNUSED) {
      initPort(table[e].address, table[e].portA, true);
      if (table[e].portA == IN)  geom.inputBytes++;
      if (table[e].portA == OUT) geom.outputBytes++;
    }
    if (table[e].portB != UNUSED) {
      initPort(table[e].address, table[e].portB, false);
      if (table[e].portB == IN)  geom.inputBytes++;
      if (table[e].portB == OUT) geom.outputBytes++;
    }
  }
  return geom;
}

uint8_t ioxReadPort(uint8_t addr, bool isPortA) {
  // Donor shape: stop after the register pointer write, then requestFrom.
  // A repeated-start (endTransmission(false)) failed to update on this
  // bench hardware even though the same register map is correct.
  Wire.beginTransmission(addr);
  Wire.write(static_cast<uint8_t>(MCP_GPIO + portOffset(isPortA)));
  Wire.endTransmission();

  Wire.requestFrom(addr, static_cast<uint8_t>(1));
  if (Wire.available()) {
    return static_cast<uint8_t>(Wire.read());
  }
  return 0;
}

void ioxWritePort(uint8_t addr, bool isPortA, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(static_cast<uint8_t>(MCP_GPIO + portOffset(isPortA)));
  Wire.write(val);
  Wire.endTransmission();
}
