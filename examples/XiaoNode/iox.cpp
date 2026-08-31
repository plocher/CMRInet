// iox.cpp — MCP23017 I2C expander access. See iox.h.

#include "iox.h"
#include <Wire.h>

namespace {

// MCP23017 register addresses.
constexpr uint8_t MCP_IODIRA = 0x00;
constexpr uint8_t MCP_IODIRB = 0x01;
constexpr uint8_t MCP_IPOLA  = 0x02;
constexpr uint8_t MCP_IPOLB  = 0x03;
constexpr uint8_t MCP_GPIOA  = 0x12;
constexpr uint8_t MCP_GPIOB  = 0x13;
constexpr uint8_t MCP_OLATA  = 0x14;
constexpr uint8_t MCP_OLATB  = 0x15;

void writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t readReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(addr, static_cast<uint8_t>(1));
  if (Wire.available()) return Wire.read();
  return 0;
}

void initPort(uint8_t addr, Direction dir, bool isPortA) {
  const uint8_t iodir = isPortA ? MCP_IODIRA : MCP_IODIRB;
  const uint8_t ipol  = isPortA ? MCP_IPOLA  : MCP_IPOLB;
  if (dir == IN) {
    writeReg(addr, iodir, 0xFF);       // all pins input
    writeReg(addr, ipol,  0xFF);       // invert (active-low convention)
  } else if (dir == OUT) {
    writeReg(addr, iodir, 0x00);       // all pins output
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
  return readReg(addr, isPortA ? MCP_GPIOA : MCP_GPIOB);
}

void ioxWritePort(uint8_t addr, bool isPortA, uint8_t val) {
  writeReg(addr, isPortA ? MCP_OLATA : MCP_OLATB, val);
}
