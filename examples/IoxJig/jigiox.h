// jigiox.h — MCP23017 I2C expander access for IoxJig (issue #32).
//
// Extended from the node examples' donor iox.h with what a validating
// jig needs and a node does not:
//   - every transaction RETURNS its I2C status instead of swallowing
//     errors (the donor's failed read silently returns 0, which for a
//     jig would masquerade as data — a NAK must be attributable as a
//     chip fault, not read as a stuck bus);
//   - OLAT is readable/writable, so the driver stage can be preloaded
//     before a direction flip and cross-checked against GPIO self-read;
//   - direction configuration goes through jigApplyPhaseRoles(), which
//     enforces the safety doctrine procedurally: every port of every
//     involved chip goes INPUT first (safe from ANY prior firmware
//     state), driver ports preload OLAT with the idle pattern before
//     IODIR flips to OUTPUT, and reader ports get pull-ups with raw
//     polarity (IPOL=0) so expected == driven.
//
// Register map: IOCON.BANK = 0; port B is base+1. Transaction shapes
// match the donor (stop-then-requestFrom reads — a repeated start
// failed to update on this bench hardware).

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "jigplan.h"

// MCP23017 register bases (IOCON.BANK = 0). Port B is base + 1.
enum : uint8_t {
  kRegIODIR = 0x00,  // direction: 1 = input
  kRegIPOL = 0x02,   // input polarity: 1 = invert
  kRegGPPU = 0x0C,   // input pull-up: 1 = enabled
  kRegGPIO = 0x12,   // port value: read = pins, write = OLAT
  kRegOLAT = 0x14,   // output latch
};

// Wire.endTransmission() status codes, named for verdict lines.
enum : uint8_t {
  kI2cOk = 0,
  kI2cDataTooLong = 1,
  kI2cNackAddr = 2,
  kI2cNackData = 3,
  kI2cOther = 4,
  kI2cTimeout = 5,
};

/// ACK-probe one address. kI2cOk means something answered — this is
/// the discovery primitive.
uint8_t jigProbe(uint8_t addr);

/// Scan [lo..hi] inclusive, calling `visitor` for every address that
/// ACKs. Pure enumeration; classification of what answered is the
/// caller's job.
typedef void (*JigScanVisitor)(void* ctx, uint8_t addr);
void jigScanBus(uint8_t lo, uint8_t hi, JigScanVisitor visitor, void* ctx);

/// Write one register. Returns the I2C status.
uint8_t jigWriteReg(uint8_t addr, uint8_t reg, uint8_t val);

/// Read one register (donor shape: register-pointer write with STOP,
/// then requestFrom). *out is only valid when the return is kI2cOk.
uint8_t jigReadReg(uint8_t addr, uint8_t reg, uint8_t* out);

/// Register address for a port: base for A, base+1 for B.
inline uint8_t jigRegFor(bool isPortA, uint8_t base) {
  return static_cast<uint8_t>(base + (isPortA ? 0 : 1));
}

/// Port direction: output=true drives (IODIR=0x00), false inputs
/// (IODIR=0xFF).
uint8_t jigSetDirection(uint8_t addr, bool isPortA, bool output);

/// Input pull-ups (GPPU mask).
uint8_t jigSetPullups(uint8_t addr, bool isPortA, uint8_t mask);

/// Input polarity (IPOL mask). The jig writes 0x00 — raw — so the
/// expected reader byte equals the driven pattern bit-for-bit.
uint8_t jigSetPolarity(uint8_t addr, bool isPortA, uint8_t mask);

/// Preload the output latch WITHOUT touching pin direction. Used
/// before any IODIR flip to OUTPUT so enabling the driver never
/// glitches an unexpected pattern onto jumpered pins.
uint8_t jigWriteOlat(uint8_t addr, bool isPortA, uint8_t val);

/// Read the output latch (health-floor readback).
uint8_t jigReadOlat(uint8_t addr, bool isPortA, uint8_t* out);

/// Drive an output port (writes GPIO, which updates OLAT).
uint8_t jigWriteGpio(uint8_t addr, bool isPortA, uint8_t val);

/// Read port pins (GPIO). On an OUTPUT port this is the driver-stage
/// self-read: the actual pin state, not the latch.
uint8_t jigReadGpio(uint8_t addr, bool isPortA, uint8_t* out);

/// Force both ports of one chip to INPUT — the known-safe base state,
/// valid to run from any prior firmware configuration.
uint8_t jigAllInputs(uint8_t addr);

/// Apply one phase's direction configuration safely:
///   1. every port of every listed chip to INPUT (safe from any state),
///   2. each kRoleDriver port: preload OLAT with `idlePattern`, then
///      IODIR to OUTPUT,
///   3. each kRoleReader port: GPPU on, IPOL raw.
/// Returns the first non-zero I2C status; kI2cOk when every write
/// landed. Call only with roles from jigplan::phaseRoles() over a map
/// that passed jigplan::validateMap().
uint8_t jigApplyPhaseRoles(const jigplan::ChipRoles* roles, uint8_t count,
                           uint8_t idlePattern);
