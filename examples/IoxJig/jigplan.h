// jigplan.h — IoxJig planning and validation core (issue #32).
//
// The Arduino-free half of the IoxJig sketch: jumper-map validation,
// pattern-matrix step planning, expected-value math, fault
// classification, and verdict aggregation. Header-only and stdint-only
// so tests/test_iox_jig.cpp compiles it with the host compiler (the
// test_xiao_generators.cpp precedent for desktop-testing sketch logic).
//
// Safety doctrine (docs/testbed-physical-notes.md, hardware-protecting
// and non-negotiable): a pin pair joined by a jumper must never have
// both ends configured as outputs. This header makes double-drive
// structurally unrepresentable rather than merely checked at runtime:
//
//   - every pair endpoint lives on a distinct (chip, port, bit) —
//     one wire per pin, so no two driver pins are ever joined through
//     a shared endpoint in either phase;
//   - every (chip, port) has ONE role per phase (driver or reader) —
//     a port that drove one pair and read another would be OUTPUT and
//     INPUT at once, and its partner pair would see both jumper ends
//     driven;
//   - a pair never joins two pins of the same port — direction is
//     per-port, so such a pair could never be walked.
//
// A map that passes validateMap() therefore cannot produce a
// both-OUTPUT condition in either walk phase, whatever the physical
// jumper reality is.
//
// Step model: one step writes ONE pattern byte to one driver port and
// judges every paired reader bit of that coupling at once — byte
// granularity, not per-pair. The pattern matrix (walkers, bit rolls,
// checkerboard, all-on/all-off flash) exercises single-bit paths,
// adjacent-bit coupling, and simultaneous switching. Which PAIR missed
// is recovered afterwards from the mismatch bytes via
// pairForReaderBit/pairForDriverBit — that mapping feeds per-pair soak
// health.

#pragma once

#include <stdint.h>

namespace jigplan {

/// Expander port selector. MCP23017 ports: A = GPIO 0-7, B = GPIO 8-15.
enum Port : uint8_t { kPortA = 0, kPortB = 1 };

/// One jumpered loopback path between two expander pins. The endpoints
/// are physical facts; which end drives and which reads is decided by
/// the walk phase (phase 1 drives endpoint 1, phase 2 drives endpoint 2).
struct JumperPair {
  uint8_t addr1;  ///< I2C address of endpoint 1's chip (0x20-0x27)
  Port port1;     ///< endpoint 1's port
  uint8_t bit1;   ///< endpoint 1's bit (0..7)
  uint8_t addr2;  ///< I2C address of endpoint 2's chip (0x20-0x27)
  Port port2;     ///< endpoint 2's port
  uint8_t bit2;   ///< endpoint 2's bit (0..7)
};

/// The declared per-assembly jumper map: the table of loopback paths
/// the walk is allowed to exercise. Discovery finds the chips; this
/// map says which pins are wired to which.
struct JigMap {
  const JumperPair* pairs;
  uint8_t pairCount;
};

/// Why a map was refused. kMapOk means the walk is safe to run.
enum MapFault : uint8_t {
  kMapOk = 0,
  kMapBadBit,         ///< a bit index outside 0..7
  kMapBadAddr,        ///< an endpoint outside the 0x20-0x27 expander range
  kMapSamePortPair,   ///< pair joins two pins of one (chip, port)
  kMapDuplicatePair,  ///< one pin claimed by two pairs
  kMapRoleConflict,   ///< one (chip, port) driven by one pair, read by another
};

/// MCP23017 address window (A0-A2 hardware straps): 0x20-0x27. The OLED
/// (0x3C) shares the bus but is never a legal jumper endpoint.
inline bool isExpanderAddr(uint8_t addr) {
  return addr >= 0x20 && addr <= 0x27;
}

/// True when two endpoints sit on the same whole port (direction is
/// per-port, so such endpoints can never take opposite roles).
inline bool sameChipPort(uint8_t aAddr, Port aPort, uint8_t bAddr, Port bPort) {
  return aAddr == bAddr && aPort == bPort;
}

/// True when two endpoints are the exact same pin.
inline bool samePin(uint8_t aAddr, Port aPort, uint8_t aBit, uint8_t bAddr,
                    Port bPort, uint8_t bBit) {
  return sameChipPort(aAddr, aPort, bAddr, bPort) && aBit == bBit;
}

/// Validate a jumper map against the safety doctrine above. Returns
/// kMapOk when the walk may run; any other value names the refusal
/// reason for the verdict line. Checks run well-formedness first, then
/// per-pin uniqueness, then per-port role consistency.
inline MapFault validateMap(const JigMap& map) {
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    const JumperPair& p = map.pairs[i];
    if (p.bit1 > 7 || p.bit2 > 7) return kMapBadBit;
    if (!isExpanderAddr(p.addr1) || !isExpanderAddr(p.addr2)) {
      return kMapBadAddr;
    }
    if (sameChipPort(p.addr1, p.port1, p.addr2, p.port2)) {
      return kMapSamePortPair;
    }
  }
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    const JumperPair& a = map.pairs[i];
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < map.pairCount; ++j) {
      const JumperPair& b = map.pairs[j];
      // One wire per pin: any shared endpoint means two pairs' driver
      // pins are joined through it in one of the two phases — two
      // outputs fighting through a jumper.
      if (samePin(a.addr1, a.port1, a.bit1, b.addr1, b.port1, b.bit1) ||
          samePin(a.addr1, a.port1, a.bit1, b.addr2, b.port2, b.bit2) ||
          samePin(a.addr2, a.port2, a.bit2, b.addr1, b.port1, b.bit1) ||
          samePin(a.addr2, a.port2, a.bit2, b.addr2, b.port2, b.bit2)) {
        return kMapDuplicatePair;
      }
      // One role per (chip, port) per phase: endpoint 1 sides drive in
      // phase 1, endpoint 2 sides read. A port appearing on both sides
      // (across pairs) would be OUTPUT and INPUT at once.
      if (sameChipPort(a.addr1, a.port1, b.addr2, b.port2) ||
          sameChipPort(a.addr2, a.port2, b.addr1, b.port1)) {
        return kMapRoleConflict;
      }
    }
  }
  return kMapOk;
}

/// Walk polarity, kept for the walker pattern families: a set bit in a
/// cleared field, and a cleared bit in a set field. Both polarities
/// mean a stuck-at cannot hide in either direction, and a missing
/// jumper is unmistakable: the unpaired/floating reader bit sits at its
/// pull-up (1), so the driven-0 polarity always mismatches.
enum Polarity : uint8_t {
  kPolaritySetInCleared = 0,
  kPolarityClearedInSet = 1,
};

/// The driver-port byte for `bit` under `polarity`.
inline uint8_t drivePatternFor(uint8_t bit, Polarity polarity) {
  const uint8_t mask = static_cast<uint8_t>(1u << bit);
  return polarity == kPolaritySetInCleared ? mask
                                           : static_cast<uint8_t>(~mask);
}

// -----------------------------------------------------------------------
// Pattern matrix
// -----------------------------------------------------------------------

/// Pattern families. Each family stresses a different failure mode:
/// walkers isolate single-bit paths in both polarities; rolls and the
/// checkerboard switch GROUPS of bits so adjacent-bit bridges and
/// crosstalk show up; the flasher switches every bit at once (di/dt,
/// pull-up headroom, drive strength).
enum PatternFamily : uint8_t {
  kWalkerSet = 0,  ///< one-hot sweep: 0x01 .. 0x80
  kWalkerCleared,  ///< inverted one-hot sweep: 0xFE .. 0x7F
  kRoll2,          ///< adjacent-bit pairs: 0x03, 0x0C, 0x30, 0xC0
  kRoll4,          ///< nibbles: 0x0F, 0xF0
  kChecker,        ///< maximum alternation: 0x55, 0xAA
  kFlash,          ///< all bits together: 0x00, 0xFF
};

/// One entry of the pattern table.
struct Pattern {
  PatternFamily family;
  uint8_t value;  ///< byte to write to the driver port
};

/// Pattern table size: 8 + 8 + 4 + 2 + 2 + 2 = 26.
inline constexpr uint8_t patternCount() { return 26; }

/// Materialize the pattern at `index` in [0, patternCount()). Order:
/// walker_set, walker_cleared, roll2, roll4, checker, flash — walkers
/// first so the visually classic walk leads the run.
inline Pattern patternAt(uint8_t index) {
  Pattern p;
  if (index < 8) {
    p.family = kWalkerSet;
    p.value = static_cast<uint8_t>(1u << index);
  } else if (index < 16) {
    p.family = kWalkerCleared;
    p.value = static_cast<uint8_t>(~(1u << (index - 8)));
  } else if (index < 20) {
    p.family = kRoll2;
    p.value = static_cast<uint8_t>(0x03u << (2u * (index - 16)));
  } else if (index < 22) {
    p.family = kRoll4;
    p.value = (index == 20) ? 0x0F : 0xF0;
  } else if (index < 24) {
    p.family = kChecker;
    p.value = (index == 22) ? 0x55 : 0xAA;
  } else if (index < 26) {
    p.family = kFlash;
    p.value = (index == 24) ? 0x00 : 0xFF;
  } else {
    p.family = kWalkerSet;  // defensive; callers stay in range
    p.value = 0x00;
  }
  return p;
}

/// Wire-name of a family, for JSON fault lines.
inline const char* familyName(PatternFamily family) {
  switch (family) {
    case kWalkerSet: return "walker_set";
    case kWalkerCleared: return "walker_cleared";
    case kRoll2: return "roll2";
    case kRoll4: return "roll4";
    case kChecker: return "checker";
    case kFlash: return "flash";
  }
  return "unknown";
}

// -----------------------------------------------------------------------
// Couplings and step enumeration
// -----------------------------------------------------------------------

/// The endpoint that DRIVES in `phase` (1 or 2).
inline void driverEndpoint(const JigMap& map, uint8_t phase, uint8_t pairIndex,
                           uint8_t& addr, Port& port, uint8_t& bit) {
  const JumperPair& p = map.pairs[pairIndex];
  if (phase == 1) {
    addr = p.addr1;
    port = p.port1;
    bit = p.bit1;
  } else {
    addr = p.addr2;
    port = p.port2;
    bit = p.bit2;
  }
}

/// The endpoint that READS in `phase` (1 or 2) — the other side.
inline void readerEndpoint(const JigMap& map, uint8_t phase, uint8_t pairIndex,
                           uint8_t& addr, Port& port, uint8_t& bit) {
  const JumperPair& p = map.pairs[pairIndex];
  if (phase == 1) {
    addr = p.addr2;
    port = p.port2;
    bit = p.bit2;
  } else {
    addr = p.addr1;
    port = p.port1;
    bit = p.bit1;
  }
}

/// A coupling: one driver port paired with one reader port through the
/// map's jumpers. A step drives the driver port's whole byte and judges
/// the reader port's paired bits.
struct PortPair {
  uint8_t driverAddr;
  Port driverPort;
  uint8_t readerAddr;
  Port readerPort;
};

/// Distinct couplings of `phase`, in first-appearance order. Only call
/// on a map that passed validateMap().
inline uint8_t couplingCount(const JigMap& map, uint8_t phase) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    uint8_t dAddr, dBit, rAddr, rBit;
    Port dPort, rPort;
    driverEndpoint(map, phase, i, dAddr, dPort, dBit);
    readerEndpoint(map, phase, i, rAddr, rPort, rBit);
    // Linear re-scan for an existing identical coupling: cheap at map
    // sizes, and keeps this helper allocation-free.
    bool seen = false;
    for (uint8_t j = 0; j < i && !seen; ++j) {
      uint8_t jdAddr, jdBit, jrAddr, jrBit;
      Port jdPort, jrPort;
      driverEndpoint(map, phase, j, jdAddr, jdPort, jdBit);
      readerEndpoint(map, phase, j, jrAddr, jrPort, jrBit);
      if (sameChipPort(jdAddr, jdPort, dAddr, dPort) &&
          sameChipPort(jrAddr, jrPort, rAddr, rPort)) {
        seen = true;
      }
    }
    if (!seen) count++;
  }
  return count;
}

/// The `index`-th coupling of `phase` (first-appearance order).
inline PortPair couplingAt(const JigMap& map, uint8_t phase, uint8_t index) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    uint8_t dAddr, dBit, rAddr, rBit;
    Port dPort, rPort;
    driverEndpoint(map, phase, i, dAddr, dPort, dBit);
    readerEndpoint(map, phase, i, rAddr, rPort, rBit);
    bool seen = false;
    for (uint8_t j = 0; j < i && !seen; ++j) {
      uint8_t jdAddr, jdBit, jrAddr, jrBit;
      Port jdPort, jrPort;
      driverEndpoint(map, phase, j, jdAddr, jdPort, jdBit);
      readerEndpoint(map, phase, j, jrAddr, jrPort, jrBit);
      if (sameChipPort(jdAddr, jdPort, dAddr, dPort) &&
          sameChipPort(jrAddr, jrPort, rAddr, rPort)) {
        seen = true;
      }
    }
    if (seen) continue;
    if (count == index) {
      PortPair c;
      c.driverAddr = dAddr;
      c.driverPort = dPort;
      c.readerAddr = rAddr;
      c.readerPort = rPort;
      return c;
    }
    count++;
  }
  PortPair none = {0, kPortA, 0, kPortA};  // defensive; callers stay in range
  return none;
}

/// One drive step: write ONE pattern byte to the coupling's driver
/// port, then judge the reader port's paired bits.
struct DriveStep {
  uint8_t phase;         ///< 1 = endpoint 1 drives, 2 = endpoint 2 drives
  uint8_t coupling;      ///< coupling index within the phase
  uint8_t patternIndex;  ///< index into the pattern table
  PatternFamily family;
  uint8_t drivePattern;    ///< byte to write to the driver port
  uint8_t driverAddr;
  Port driverPort;
  uint8_t readerAddr;
  Port readerPort;
  uint8_t expectedReader;  ///< byte the reader port must show
  uint8_t pairedMask;      ///< reader bits this step judges
};

/// Steps in one full matrix pass (one repetition of the rate blocks):
/// every coupling of both phases under every pattern.
inline uint16_t stepCount(const JigMap& map) {
  const uint16_t couplings = static_cast<uint16_t>(couplingCount(map, 1)) +
                             static_cast<uint16_t>(couplingCount(map, 2));
  return static_cast<uint16_t>(couplings * patternCount());
}

/// Materialize the step at `index` in [0, stepCount(map)). Order is
/// phase-major, then coupling, then pattern — a caller finishes every
/// pattern of one phase's couplings before reconfiguring directions.
inline DriveStep stepAt(const JigMap& map, uint16_t index) {
  const uint16_t perPhase =
      static_cast<uint16_t>(couplingCount(map, 1)) * patternCount();
  DriveStep s;
  s.phase = index < perPhase ? 1 : 2;
  const uint16_t within =
      index < perPhase ? index : static_cast<uint16_t>(index - perPhase);
  s.coupling = static_cast<uint8_t>(within / patternCount());
  s.patternIndex = static_cast<uint8_t>(within % patternCount());
  const Pattern p = patternAt(s.patternIndex);
  s.family = p.family;
  s.drivePattern = p.value;
  const PortPair c = couplingAt(map, s.phase, s.coupling);
  s.driverAddr = c.driverAddr;
  s.driverPort = c.driverPort;
  s.readerAddr = c.readerAddr;
  s.readerPort = c.readerPort;
  // Reader bits paired through THIS driver port take the driven value;
  // every other reader bit floats at its pull-up (1) — reader ports are
  // configured GPPU-on and IPOL-off, and any other driver port idles
  // all-high.
  s.expectedReader = 0xFF;
  s.pairedMask = 0;
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    uint8_t dAddr, dBit, rAddr, rBit;
    Port dPort, rPort;
    driverEndpoint(map, s.phase, i, dAddr, dPort, dBit);
    readerEndpoint(map, s.phase, i, rAddr, rPort, rBit);
    if (!sameChipPort(dAddr, dPort, s.driverAddr, s.driverPort)) continue;
    if (!sameChipPort(rAddr, rPort, s.readerAddr, s.readerPort)) continue;
    const uint8_t driven = static_cast<uint8_t>((s.drivePattern >> dBit) & 1u);
    s.expectedReader = static_cast<uint8_t>(
        (s.expectedReader & ~(1u << rBit)) | (driven << rBit));
    s.pairedMask = static_cast<uint8_t>(s.pairedMask | (1u << rBit));
  }
  return s;
}

// -----------------------------------------------------------------------
// Bit -> pair attribution
// -----------------------------------------------------------------------

/// The pair whose READER endpoint in `phase` is this exact pin, or
/// 0xFF when the bit is unpaired (floating — never judged).
inline uint8_t pairForReaderBit(const JigMap& map, uint8_t phase,
                                uint8_t readerAddr, Port readerPort,
                                uint8_t bit) {
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    uint8_t addr, b;
    Port port;
    readerEndpoint(map, phase, i, addr, port, b);
    if (addr == readerAddr && port == readerPort && b == bit) return i;
  }
  return 0xFF;
}

/// The pair whose DRIVER endpoint in `phase` is this exact pin, or
/// 0xFF when the bit is unpaired. A self-read miss on an unpaired
/// driver bit is still a chip fault, but there is no pair to name.
inline uint8_t pairForDriverBit(const JigMap& map, uint8_t phase,
                                uint8_t driverAddr, Port driverPort,
                                uint8_t bit) {
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    uint8_t addr, b;
    Port port;
    driverEndpoint(map, phase, i, addr, port, b);
    if (addr == driverAddr && port == driverPort && b == bit) return i;
  }
  return 0xFF;
}

// -----------------------------------------------------------------------
// Phase roles
// -----------------------------------------------------------------------

/// A port's role in one phase. kRoleUnused ports stay INPUT — safe
/// whatever the physical wiring does.
enum PortRole : uint8_t {
  kRoleUnused = 0,
  kRoleDriver = 1,
  kRoleReader = 2,
};

/// Per-chip direction configuration for one phase.
struct ChipRoles {
  uint8_t addr;
  PortRole portA;
  PortRole portB;
};

/// Fill `out` with each chip the map involves and its per-port roles
/// for `phase`, in first-appearance order (each pair lists its
/// driver-side chip first). Only call on a map that passed
/// validateMap() — the role assignment assumes one role per
/// (chip, port). Returns the chip count written; never writes beyond
/// maxChips entries.
inline uint8_t phaseRoles(const JigMap& map, uint8_t phase, ChipRoles* out,
                          uint8_t maxChips) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < map.pairCount; ++i) {
    for (uint8_t side = 0; side < 2; ++side) {
      uint8_t addr, bit;
      Port port;
      if (side == 0) {
        driverEndpoint(map, phase, i, addr, port, bit);
      } else {
        readerEndpoint(map, phase, i, addr, port, bit);
      }
      const PortRole role = (side == 0) ? kRoleDriver : kRoleReader;
      ChipRoles* chip = nullptr;
      for (uint8_t c = 0; c < count; ++c) {
        if (out[c].addr == addr) {
          chip = &out[c];
          break;
        }
      }
      if (chip == nullptr) {
        if (count >= maxChips) continue;  // defensive clamp
        chip = &out[count++];
        chip->addr = addr;
        chip->portA = kRoleUnused;
        chip->portB = kRoleUnused;
      }
      if (port == kPortA) {
        chip->portA = role;
      } else {
        chip->portB = role;
      }
    }
  }
  return count;
}

// -----------------------------------------------------------------------
// Fault classification and verdict
// -----------------------------------------------------------------------

/// Per-step fault classes. The walk observes two reads per step: the
/// driver port's own GPIO (self-read — on MCP23017 a GPIO read of an
/// output port returns the actual pin state) and the reader port's
/// GPIO. Self-read first separates the driver stage from everything
/// downstream of it.
enum StepFault : uint8_t {
  kStepOk = 0,
  kStepDriverFault,    ///< self-read disagrees with the commanded pattern
  kStepLoopbackFault,  ///< self-read OK; a paired reader bit missed
};

/// Judge one step's reads. `pairedMask` restricts the loopback
/// comparison to the reader bits this step actually judges — unpaired
/// bits float at their pull-ups and are never asserted against.
///
/// Jumper-path vs input-reader attribution: through a loopback alone
/// the two are not separable per bit (a missing conductor and a dead
/// reader pin read identically), so a per-bit miss reports
/// kStepLoopbackFault and the sketch escalates to a reader-port fault
/// only when every paired bit of one reader port is stuck (allStuck).
inline StepFault classifyStep(uint8_t drivePattern, uint8_t selfRead,
                              uint8_t expectedReader, uint8_t observedReader,
                              uint8_t pairedMask) {
  if (selfRead != drivePattern) return kStepDriverFault;
  const uint8_t mismatch = static_cast<uint8_t>(
      (expectedReader ^ observedReader) & pairedMask);
  return mismatch != 0 ? kStepLoopbackFault : kStepOk;
}

/// Verdict-level fault classes, accumulated as a bitmask on the final
/// verdict line. Setup covers the pre-walk failures: chip absent,
/// register health, direction configuration, and refused maps.
enum VerdictClass : uint8_t {
  kClassNone = 0,
  kClassSetup = 1 << 0,
  kClassDriver = 1 << 1,
  kClassLoopback = 1 << 2,
};

/// Map a step fault to its verdict class.
inline uint8_t classForStepFault(StepFault fault) {
  switch (fault) {
    case kStepOk:
      return kClassNone;
    case kStepDriverFault:
      return kClassDriver;
    case kStepLoopbackFault:
      return kClassLoopback;
  }
  return kClassNone;
}

/// Soak health of one walked bit, from cumulative read-attempt
/// counters (retries INCLUDED — a miss that a retry recovered still
/// counts as a failed attempt; nothing masks flakiness).
enum BitHealth : uint8_t {
  kBitHealthy = 0,
  kBitStuckAt,   ///< failed every attempt — hard fault territory
  kBitMarginal,  ///< failed some attempts — flaky contact or speed margin
};

/// Classify one bit's soak counters. Zero attempts is healthy here;
/// bits the walk never reached are reported by coverage, not soak.
/// Hard-vs-speed-sensitive is a separate datum: the sketch reports the
/// per-pair fault count in the SLOWEST rate block alongside this.
inline BitHealth bitHealth(uint16_t attempts, uint16_t failures) {
  if (failures == 0) return kBitHealthy;
  if (attempts > 0 && failures >= attempts) return kBitStuckAt;
  return kBitMarginal;
}

/// True when every entry is stuck-at — the whole-port escalation
/// evidence for a reader-port fault. Empty is false: an escalation
/// needs evidence, not the absence of it.
inline bool allStuck(const BitHealth* healths, uint8_t count) {
  if (count == 0) return false;
  for (uint8_t i = 0; i < count; ++i) {
    if (healths[i] != kBitStuckAt) return false;
  }
  return true;
}

/// The final verdict, rendered as the JSON verdict line and the human
/// VERDICT banner.
struct Verdict {
  bool green;
  uint16_t stepsRun;
  uint16_t faults;
  uint8_t classes;  ///< VerdictClass bitmask
};

/// Aggregate counters into the verdict. GREEN requires zero faults AND
/// no fault classes present — a refused map (setup class, zero steps)
/// is RED even though nothing walked. Strict doctrine: ANY miss at ANY
/// rate block faults the run; retries classify, they never mask.
inline Verdict makeVerdict(uint16_t stepsRun, uint16_t faults,
                           uint8_t classes) {
  Verdict v;
  v.green = (faults == 0 && classes == kClassNone);
  v.stepsRun = stepsRun;
  v.faults = faults;
  v.classes = classes;
  return v;
}

}  // namespace jigplan
