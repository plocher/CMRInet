// jigiox.cpp — MCP23017 access for IoxJig. See jigiox.h.
//
// Register map and transaction shapes match the donor cpNode IOX path
// (examples/XiaoNode/iox.cpp, proven on this hardware): stop-then-
// requestFrom reads, GPIO for pin R/W. Unlike the donor, every
// transaction returns its Wire status — a jig must attribute a NAK as
// a chip fault, never read it as data.

#include "jigiox.h"

#include <Wire.h>

uint8_t jigProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission();
}

void jigScanBus(uint8_t lo, uint8_t hi, JigScanVisitor visitor, void* ctx) {
  // uint16_t cursor so hi == 0xFF terminates cleanly.
  for (uint16_t addr = lo; addr <= hi; ++addr) {
    if (jigProbe(static_cast<uint8_t>(addr)) == kI2cOk) {
      visitor(ctx, static_cast<uint8_t>(addr));
    }
  }
}

uint8_t jigWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission();
}

uint8_t jigReadReg(uint8_t addr, uint8_t reg, uint8_t* out) {
  // Donor shape: register-pointer write with STOP, then requestFrom.
  // A repeated start (endTransmission(false)) failed to update on this
  // bench hardware even though the register map is correct.
  Wire.beginTransmission(addr);
  Wire.write(reg);
  const uint8_t status = Wire.endTransmission();
  if (status != kI2cOk) return status;
  if (Wire.requestFrom(static_cast<int>(addr), 1) != 1) return kI2cOther;
  *out = static_cast<uint8_t>(Wire.read());
  return kI2cOk;
}

uint8_t jigSetDirection(uint8_t addr, bool isPortA, bool output) {
  return jigWriteReg(addr, jigRegFor(isPortA, kRegIODIR),
                     output ? 0x00 : 0xFF);
}

uint8_t jigSetPullups(uint8_t addr, bool isPortA, uint8_t mask) {
  return jigWriteReg(addr, jigRegFor(isPortA, kRegGPPU), mask);
}

uint8_t jigSetPolarity(uint8_t addr, bool isPortA, uint8_t mask) {
  return jigWriteReg(addr, jigRegFor(isPortA, kRegIPOL), mask);
}

uint8_t jigWriteOlat(uint8_t addr, bool isPortA, uint8_t val) {
  return jigWriteReg(addr, jigRegFor(isPortA, kRegOLAT), val);
}

uint8_t jigReadOlat(uint8_t addr, bool isPortA, uint8_t* out) {
  return jigReadReg(addr, jigRegFor(isPortA, kRegOLAT), out);
}

uint8_t jigWriteGpio(uint8_t addr, bool isPortA, uint8_t val) {
  return jigWriteReg(addr, jigRegFor(isPortA, kRegGPIO), val);
}

uint8_t jigReadGpio(uint8_t addr, bool isPortA, uint8_t* out) {
  return jigReadReg(addr, jigRegFor(isPortA, kRegGPIO), out);
}

uint8_t jigAllInputs(uint8_t addr) {
  const uint8_t statusA = jigSetDirection(addr, true, false);
  if (statusA != kI2cOk) return statusA;
  return jigSetDirection(addr, false, false);
}

uint8_t jigApplyPhaseRoles(const jigplan::ChipRoles* roles, uint8_t count,
                           uint8_t idlePattern) {
  // 1. Base state: every port of every involved chip to INPUT. Safe
  //    from any prior firmware configuration, and this same pass is
  //    the phase transit — a jumpered pin pair is never driven from
  //    both ends at any instant.
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t status = jigAllInputs(roles[i].addr);
    if (status != kI2cOk) return status;
  }
  // 2. Driver ports: preload the output latch with the idle pattern
  //    BEFORE flipping IODIR, so enabling the driver never glitches an
  //    unexpected pattern onto jumpered pins.
  for (uint8_t i = 0; i < count; ++i) {
    for (uint8_t side = 0; side < 2; ++side) {
      const bool isPortA = (side == 0);
      const jigplan::PortRole role =
          isPortA ? roles[i].portA : roles[i].portB;
      if (role != jigplan::kRoleDriver) continue;
      const uint8_t preload =
          jigWriteOlat(roles[i].addr, isPortA, idlePattern);
      if (preload != kI2cOk) return preload;
      const uint8_t flip = jigSetDirection(roles[i].addr, isPortA, true);
      if (flip != kI2cOk) return flip;
    }
  }
  // 3. Reader ports: pull-ups on, polarity raw — the expected reader
  //    byte then equals the driven pattern bit-for-bit, and an
  //    unpaired/floating reader bit sits at 1.
  for (uint8_t i = 0; i < count; ++i) {
    for (uint8_t side = 0; side < 2; ++side) {
      const bool isPortA = (side == 0);
      const jigplan::PortRole role =
          isPortA ? roles[i].portA : roles[i].portB;
      if (role != jigplan::kRoleReader) continue;
      const uint8_t pullups = jigSetPullups(roles[i].addr, isPortA, 0xFF);
      if (pullups != kI2cOk) return pullups;
      const uint8_t polarity = jigSetPolarity(roles[i].addr, isPortA, 0x00);
      if (polarity != kI2cOk) return polarity;
    }
  }
  return kI2cOk;
}
