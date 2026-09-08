// IoxJig.ino — standalone cpNode-IOX manufacturing-validation jig
// (issue #32).
//
// The DUT is the cpNode-IOX card on this board's local I2C bus. This
// sketch talks to the MCP23017 expanders directly over Wire: it scans
// the bus, validates each found chip at register level, then drives a
// PATTERN MATRIX through every declared loopback coupling in BOTH port
// directions — walkers (both polarities), 2/4-bit rolls, checkerboard,
// and an all-on/all-off flasher — under a schedule of RATE BLOCKS that
// sweeps step pacing and I2C clock fast-first and back again. Every
// step self-reads the driver stage so a fault names its stage. It
// finishes with a GREEN OK / RED FAIL verdict on the OLED, the onboard
// LED, and the serial stream. No CMRI, no RS-485, no host — the CMRI
// engines are absent by design (the reframe comment on #32 records
// why).
//
// Detection doctrine (bench feedback, 2026-09): fast dominates slow —
// anything that passes at line speed passes at 1 s settle — so the
// matrix runs fast-first and the slow blocks exist to CHARACTERIZE
// discovered faults (fails even at the slowest pace = hard; fails only
// at speed = marginal), not to re-detect. Verdict is strict: ANY miss
// at ANY rate block faults the run; retries classify (stuck vs
// marginal via per-pair read counters), they never mask.
//
// OLED doctrine: display updates happen at BOUNDARIES only (boot,
// inventory, rate-block starts, verdict) — never inside a block — so
// OLED traffic never interleaves with expander transactions and an
// OLED failure (e.g. the SSD1306 is out of spec above 400 kHz) can
// never influence expander results. A missed update at a high-rate
// boundary just means a less frequent picture; serial carries the
// data.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6):
//   D4 - SDA  I2C (expanders 0x20-0x27 + OLED 0x3C)
//   D5 - SCL  I2C
//   D2 - optional start button (needs JIG_USE_BUTTON hardware wiring)
//   LED_BUILTIN lights when the verdict lands.
//
// Serial: JSON lines (the bench-house machine-readable style) plus one
// human VERDICT banner. A run auto-starts at boot; after the verdict
// the board halts and any serial line re-triggers a fresh run — no
// reflash, no reset.
//
// The jumper map below declares the assembly's loopback wiring: which
// pins connect to which. Discovery finds the chips; the map says which
// pins are walkable. The map is validated before ANY direction write —
// a pair with both ends driven is refused (docs/testbed-physical-
// notes.md hardware-protecting rule). Edit it to match your assembly;
// the default is the bench testbed: one MCP23017 at 0x20 with an
// 8-conductor A<->B cable set.

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>

#include "jigplan.h"
#include "jigiox.h"
#include "jigdisplay.h"

using namespace jigplan;

// =============================================
// ====   Jig version and identity          ====
// =============================================

#ifndef JIG_VERSION
#define JIG_VERSION "1.1.1"
#endif

// =============================================
// ====   Jumper map (edit to match your    ====
// ====   assembly)                          ====
// =============================================

namespace {

// One MCP23017 at 0x20, port A bit i <-> port B bit i for all 8 bits.
const JumperPair kJumperPairs[] = {
    {0x20, kPortA, 0, 0x20, kPortB, 0},
    {0x20, kPortA, 1, 0x20, kPortB, 1},
    {0x20, kPortA, 2, 0x20, kPortB, 2},
    {0x20, kPortA, 3, 0x20, kPortB, 3},
    {0x20, kPortA, 4, 0x20, kPortB, 4},
    {0x20, kPortA, 5, 0x20, kPortB, 5},
    {0x20, kPortA, 6, 0x20, kPortB, 6},
    {0x20, kPortA, 7, 0x20, kPortB, 7},
};
const JigMap kMap = {kJumperPairs,
                     sizeof(kJumperPairs) / sizeof(kJumperPairs[0])};

}  // namespace

// =============================================
// ====   Knobs                             ====
// =============================================

#ifndef JIG_USE_OLED
#define JIG_USE_OLED 1  // 0 to run headless
#endif

#ifndef JIG_USE_BUTTON
// v1: no D2 hardware. Runs auto-start at boot; serial line re-triggers.
#define JIG_USE_BUTTON 0
#endif

#ifndef JIG_MATRIX_REPEATS
#define JIG_MATRIX_REPEATS 2  // repetitions of the whole rate-block sweep
#endif

#ifndef JIG_RETRIES
// Statistical re-reads after a faulted step. They NEVER mask the fault
// (the step stays faulted); they classify it: a pair whose reads all
// fail is stuck_at, one that sometimes passes is marginal.
#define JIG_RETRIES 3
#endif

#ifndef JIG_IDLE_PATTERN
#define JIG_IDLE_PATTERN 0xFF  // driver-port preload before enabling
#endif

// =============================================
// ====   Rate blocks                       ====
// =============================================

// One rate block = a pacing delay plus an I2C clock. The schedule runs
// fast-first, pushes to the bus limits, and comes back down ("and back
// again") — the descent re-checks at slow pace whatever the fast blocks
// saw, which is what separates hard faults from speed-marginal ones.
// Above 400 kHz the bus is out of the SSD1306's spec: an OLED update
// may fail there, which is fine (boundary doctrine), and a loopback
// fault that appears only at 800k/1M is a real datum — though it may
// characterize the bench topology (pull-ups, capacitance) as much as
// the DUT. 1 MHz is not required to work; "works at 400/800 but not
// 1M" is itself a finding, recorded per block in the verdict.
struct RateBlock {
  uint32_t delayMs;  // per-step settle
  uint32_t i2cKhz;   // SCL clock for the block
};

constexpr RateBlock kRateBlocks[] = {
    {50, 100},   // slowest: the characterization pace ("slow is ms")
    {10, 400},   // fast-mode bus
    {0, 800},    // full-out, above OLED spec
    {0, 1000},   // the bus limit — may not hold; that is a datum
    {10, 400},   // descent
    {50, 100},   // back to the characterization pace
};
constexpr uint8_t kBlockCount = sizeof(kRateBlocks) / sizeof(kRateBlocks[0]);
constexpr uint8_t kMaxBlocks = 8;

constexpr uint16_t kAutoRunDelaySecs = 3;  // boot -> run grace window
constexpr uint32_t kAutoRunDelayMs = kAutoRunDelaySecs * 1000UL;

// =============================================
// ====   Run state                         ====
// =============================================

constexpr uint8_t kMaxChips = 8;   // the 0x20-0x27 window
constexpr uint8_t kMaxPairs = 64;  // generous ceiling for the map

// Discovery results from the bus scan.
uint8_t gFound[kMaxChips];  // expander addresses that ACKed, ascending
uint8_t gFoundCount = 0;
uint8_t gUnknownResponders = 0;  // non-expander, non-OLED ACKs

// Per-pair soak counters (READ-level: retries included), indexed by
// map pair. gFaultsSlowest counts failed reads in block 0 — the
// hard-vs-speed-marginal datum.
uint16_t gAttempts[kMaxPairs] = {};
uint16_t gFaults[kMaxPairs] = {};
uint16_t gFaultsSlowest[kMaxPairs] = {};

uint16_t gStepsRun = 0;
uint16_t gFaultCount = 0;  // faulted STEPS (verdict currency)
uint8_t gClasses = kClassNone;
uint16_t gFaultsByBlock[kMaxBlocks] = {};

// Most recent fault, kept for the next OLED boundary draw.
char gLastFault[22] = "";

JigDisplay gOled;
bool gRunComplete = false;

// =============================================
// ====   Reporting                         ====
// =============================================

const char* mapFaultName(MapFault f) {
  switch (f) {
    case kMapOk: return "ok";
    case kMapBadBit: return "map_bad_bit";
    case kMapBadAddr: return "map_bad_addr";
    case kMapSamePortPair: return "map_same_port_pair";
    case kMapDuplicatePair: return "map_duplicate_pair";
    case kMapRoleConflict: return "map_role_conflict";
  }
  return "map_unknown";
}

const char* stepFaultName(StepFault f) {
  switch (f) {
    case kStepOk: return "ok";
    case kStepDriverFault: return "driver";
    case kStepLoopbackFault: return "loopback";
  }
  return "unknown";
}

// Render the VerdictClass bitmask as e.g. "setup|loopback".
void classNames(uint8_t classes, char* out, size_t cap) {
  out[0] = '\0';
  const char* sep = "";
  if (classes & kClassSetup) {
    snprintf(out + strlen(out), cap - strlen(out), "%ssetup", sep);
    sep = "|";
  }
  if (classes & kClassDriver) {
    snprintf(out + strlen(out), cap - strlen(out), "%sdriver", sep);
    sep = "|";
  }
  if (classes & kClassLoopback) {
    snprintf(out + strlen(out), cap - strlen(out), "%sloopback", sep);
    sep = "|";
  }
  if (out[0] == '\0') {
    snprintf(out, cap, "none");
  }
}

// =============================================
// ====   Matrix engine                     ====
// =============================================

// Bus-scan visitor: classify each ACK by address range. The OLED is
// the known other bus citizen; everything else in the expander window
// is a candidate chip.
void onScanHit(void*, uint8_t addr) {
  if (addr == 0x3C) return;  // OLED — expected, not an expander
  if (isExpanderAddr(addr)) {
    if (gFoundCount < kMaxChips) {
      gFound[gFoundCount++] = addr;
    }
  } else {
    gUnknownResponders++;
  }
}

bool mapNamesChip(uint8_t addr) {
  for (uint8_t i = 0; i < kMap.pairCount; ++i) {
    if (kMap.pairs[i].addr1 == addr || kMap.pairs[i].addr2 == addr) {
      return true;
    }
  }
  return false;
}

// The pairs judged by a step: those whose driver end sits on the
// step's driver port and reader end on its reader port. Every read of
// the step judges all of them.
uint8_t couplingPairList(const DriveStep& s, uint8_t* out) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kMap.pairCount; ++i) {
    uint8_t dAddr, dBit, rAddr, rBit;
    Port dPort, rPort;
    driverEndpoint(kMap, s.phase, i, dAddr, dPort, dBit);
    readerEndpoint(kMap, s.phase, i, rAddr, rPort, rBit);
    if (!sameChipPort(dAddr, dPort, s.driverAddr, s.driverPort)) continue;
    if (!sameChipPort(rAddr, rPort, s.readerAddr, s.readerPort)) continue;
    if (n < 8) out[n++] = i;  // one port byte couples at most 8 pairs
  }
  return n;
}

// Charge the per-pair read counters for one read attempt. `miss` holds
// the mismatched bits of the judged byte; `isDriver` selects which
// endpoint side they map to.
void chargePairs(const DriveStep& s, const uint8_t* pairs, uint8_t pairCount,
                 uint8_t miss, bool isDriver, bool slowestBlock) {
  for (uint8_t i = 0; i < pairCount; ++i) {
    gAttempts[pairs[i]]++;
  }
  for (uint8_t bit = 0; bit < 8; ++bit) {
    if (((miss >> bit) & 1u) == 0) continue;
    const uint8_t p =
        isDriver ? pairForDriverBit(kMap, s.phase, s.driverAddr, s.driverPort,
                                    bit)
                 : pairForReaderBit(kMap, s.phase, s.readerAddr, s.readerPort,
                                    bit);
    if (p >= kMaxPairs) continue;  // unpaired bit: chip fault, no pair
    gFaults[p]++;
    if (slowestBlock) gFaultsSlowest[p]++;
  }
}

// One drive step at one rate. Strict: the first miss faults the step;
// the retries that follow are statistics, never absolution.
void executeStep(uint16_t stepIndex, uint8_t rep, uint8_t blockIdx,
                 uint32_t settleMs) {
  const DriveStep s = stepAt(kMap, stepIndex);
  uint8_t pairs[8];
  const uint8_t pairCount = couplingPairList(s, pairs);
  const bool slowest = (blockIdx == 0);

  const uint8_t writeStatus =
      jigWriteGpio(s.driverAddr, s.driverPort == kPortA, s.drivePattern);
  if (writeStatus != kI2cOk) {
    gStepsRun++;
    gFaultCount++;
    gFaultsByBlock[blockIdx]++;
    gClasses = static_cast<uint8_t>(gClasses | kClassDriver);
    Serial.printf(
        "{\"event\":\"step_fault\",\"rep\":%u,\"block\":%u,\"phase\":%u,"
        "\"family\":\"%s\",\"pattern\":\"0x%02X\",\"class\":\"driver\","
        "\"cause\":\"i2c_write\",\"i2c_status\":%u}\n",
        static_cast<unsigned>(rep), static_cast<unsigned>(blockIdx),
        static_cast<unsigned>(s.phase), familyName(s.family),
        static_cast<unsigned>(s.drivePattern),
        static_cast<unsigned>(writeStatus));
    snprintf(gLastFault, sizeof(gLastFault), "b%u %s wr-NAK",
             static_cast<unsigned>(blockIdx), familyName(s.family));
    return;
  }
  delay(settleMs);

  uint8_t selfRead = 0;
  uint8_t observed = 0;
  const uint8_t selfStatus =
      jigReadGpio(s.driverAddr, s.driverPort == kPortA, &selfRead);
  const uint8_t readerStatus =
      jigReadGpio(s.readerAddr, s.readerPort == kPortA, &observed);
  if (selfStatus != kI2cOk || readerStatus != kI2cOk) {
    gStepsRun++;
    gFaultCount++;
    gFaultsByBlock[blockIdx]++;
    gClasses = static_cast<uint8_t>(gClasses | kClassSetup);
    Serial.printf(
        "{\"event\":\"step_fault\",\"rep\":%u,\"block\":%u,\"phase\":%u,"
        "\"family\":\"%s\",\"pattern\":\"0x%02X\",\"class\":\"setup\","
        "\"cause\":\"i2c_read\",\"self_status\":%u,\"reader_status\":%u}\n",
        static_cast<unsigned>(rep), static_cast<unsigned>(blockIdx),
        static_cast<unsigned>(s.phase), familyName(s.family),
        static_cast<unsigned>(s.drivePattern),
        static_cast<unsigned>(selfStatus),
        static_cast<unsigned>(readerStatus));
    snprintf(gLastFault, sizeof(gLastFault), "b%u %s rd-NAK",
             static_cast<unsigned>(blockIdx), familyName(s.family));
    return;
  }

  gStepsRun++;
  StepFault fault = classifyStep(s.drivePattern, selfRead, s.expectedReader,
                                 observed, s.pairedMask);
  if (fault == kStepOk) {
    chargePairs(s, pairs, pairCount, 0, false, slowest);
    return;
  }

  // Faulted step: charge counters, classify with bounded re-reads, and
  // report once with the retry evidence.
  gFaultCount++;
  gFaultsByBlock[blockIdx]++;
  gClasses = static_cast<uint8_t>(gClasses | classForStepFault(fault));

  uint8_t miss = (fault == kStepDriverFault)
                     ? static_cast<uint8_t>(s.drivePattern ^ selfRead)
                     : static_cast<uint8_t>((s.expectedReader ^ observed) &
                                            s.pairedMask);
  chargePairs(s, pairs, pairCount, miss, fault == kStepDriverFault, slowest);

  uint8_t retryPass = 0;
  for (uint8_t retry = 0; retry < JIG_RETRIES; ++retry) {
    delay(5);  // a little settle: recovery under retry = marginal
    uint8_t rSelf = 0;
    uint8_t rObs = 0;
    if (jigReadGpio(s.driverAddr, s.driverPort == kPortA, &rSelf) != kI2cOk ||
        jigReadGpio(s.readerAddr, s.readerPort == kPortA, &rObs) != kI2cOk) {
      // A retry read that NAKs is a failed attempt on every coupled pair.
      chargePairs(s, pairs, pairCount, s.pairedMask, false, slowest);
      continue;
    }
    const StepFault rFault = classifyStep(s.drivePattern, rSelf,
                                          s.expectedReader, rObs, s.pairedMask);
    if (rFault == kStepOk) {
      retryPass++;
      chargePairs(s, pairs, pairCount, 0, false, slowest);
    } else {
      const uint8_t rMiss =
          (rFault == kStepDriverFault)
              ? static_cast<uint8_t>(s.drivePattern ^ rSelf)
              : static_cast<uint8_t>((s.expectedReader ^ rObs) & s.pairedMask);
      chargePairs(s, pairs, pairCount, rMiss, rFault == kStepDriverFault,
                  slowest);
    }
  }

  Serial.printf(
      "{\"event\":\"step_fault\",\"rep\":%u,\"block\":%u,\"phase\":%u,"
      "\"family\":\"%s\",\"pattern\":\"0x%02X\",\"class\":\"%s\","
      "\"drv\":\"0x%02X\",\"self\":\"0x%02X\",\"exp\":\"0x%02X\","
      "\"obs\":\"0x%02X\",\"mask\":\"0x%02X\",\"miss\":\"0x%02X\","
      "\"retry_pass\":%u}\n",
      static_cast<unsigned>(rep), static_cast<unsigned>(blockIdx),
      static_cast<unsigned>(s.phase), familyName(s.family),
      static_cast<unsigned>(s.drivePattern), stepFaultName(fault),
      static_cast<unsigned>(s.drivePattern), static_cast<unsigned>(selfRead),
      static_cast<unsigned>(s.expectedReader), static_cast<unsigned>(observed),
      static_cast<unsigned>(s.pairedMask), static_cast<unsigned>(miss),
      static_cast<unsigned>(retryPass));
  // Boundary summary names the JUMPER (pair), which is what the
  // operator can act on; unpaired driver-bit misses fall back to
  // "chip".
  uint8_t faultPair = 0xFF;
  for (uint8_t bit = 0; bit < 8 && faultPair >= kMaxPairs; ++bit) {
    if (((miss >> bit) & 1u) == 0) continue;
    faultPair = (fault == kStepDriverFault)
                    ? pairForDriverBit(kMap, s.phase, s.driverAddr,
                                       s.driverPort, bit)
                    : pairForReaderBit(kMap, s.phase, s.readerAddr,
                                       s.readerPort, bit);
  }
  if (faultPair < kMaxPairs) {
    snprintf(gLastFault, sizeof(gLastFault), "b%u p%u %s",
             static_cast<unsigned>(blockIdx),
             static_cast<unsigned>(faultPair), stepFaultName(fault));
  } else {
    snprintf(gLastFault, sizeof(gLastFault), "b%u %s chip",
             static_cast<unsigned>(blockIdx), stepFaultName(fault));
  }
}

// One phase inside one rate block: apply directions at the block's
// clock, then run every step of the phase. False on a direction-config
// fault (the caller halts).
bool runPhaseInBlock(uint8_t rep, uint8_t blockIdx, uint8_t phase,
                     uint32_t settleMs) {
  ChipRoles roles[kMaxChips];
  const uint8_t roleCount = phaseRoles(kMap, phase, roles, kMaxChips);
  const uint8_t cfgStatus =
      jigApplyPhaseRoles(roles, roleCount, JIG_IDLE_PATTERN);
  if (cfgStatus != kI2cOk) {
    Serial.printf(
        "{\"event\":\"setup_fault\",\"class\":\"setup\","
        "\"cause\":\"direction_config_fault\",\"rep\":%u,\"block\":%u,"
        "\"phase\":%u,\"i2c_status\":%u}\n",
        static_cast<unsigned>(rep), static_cast<unsigned>(blockIdx),
        static_cast<unsigned>(phase), static_cast<unsigned>(cfgStatus));
    return false;
  }
  Serial.printf("{\"event\":\"phase\",\"rep\":%u,\"block\":%u,\"phase\":%u}\n",
                static_cast<unsigned>(rep), static_cast<unsigned>(blockIdx),
                static_cast<unsigned>(phase));
  // Step-index range of this phase (phase-major enumeration).
  const uint16_t phase1Steps = static_cast<uint16_t>(couplingCount(kMap, 1)) *
                               patternCount();
  const uint16_t lo = (phase == 1) ? 0 : phase1Steps;
  const uint16_t hi = (phase == 1) ? phase1Steps : stepCount(kMap);
  for (uint16_t idx = lo; idx < hi; ++idx) {
    executeStep(idx, rep, blockIdx, settleMs);
  }
  return true;
}

void verdictAndHalt(uint16_t stepsRun, uint16_t faults, uint8_t classes) {
  Wire.setClock(100000);  // standard rate for the verdict draw and idle
  const Verdict v = makeVerdict(stepsRun, faults, classes);
  char names[24];
  classNames(v.classes, names, sizeof(names));
  char byBlock[kMaxBlocks * 6 + 2];
  byBlock[0] = '\0';
  for (uint8_t b = 0; b < kBlockCount; ++b) {
    snprintf(byBlock + strlen(byBlock), sizeof(byBlock) - strlen(byBlock),
             "%s%u", b == 0 ? "" : ",",
             static_cast<unsigned>(gFaultsByBlock[b]));
  }
  Serial.printf(
      "{\"event\":\"verdict\",\"green\":%s,\"steps\":%u,\"faults\":%u,"
      "\"classes\":\"%s\",\"faults_by_block\":[%s]}\n",
      v.green ? "true" : "false", static_cast<unsigned>(v.stepsRun),
      static_cast<unsigned>(v.faults), names, byBlock);
  Serial.printf("VERDICT: %s (steps=%u faults=%u classes=%s)\n",
                v.green ? "GREEN OK" : "RED FAIL",
                static_cast<unsigned>(v.stepsRun),
                static_cast<unsigned>(v.faults), names);

  // Fault map for the RED screen: mark BOTH endpoints of every pair
  // that saw faults — through a loopback the two ends are not
  // separable, so the marked path is the honest unit. Plus the
  // worst-pair text: "p7 A7-B7".
  uint8_t addrs[kMaxChips];
  uint8_t masks[kMaxChips][2];
  uint8_t mapChips = 0;
  uint8_t worst = 0xFF;
  uint16_t worstFaults = 0;
  for (uint8_t p = 0; p < kMap.pairCount; ++p) {
    if (gFaults[p] > worstFaults) {
      worstFaults = gFaults[p];
      worst = p;
    }
    if (gFaults[p] == 0) continue;
    for (uint8_t side = 0; side < 2; ++side) {
      const uint8_t addr =
          side == 0 ? kMap.pairs[p].addr1 : kMap.pairs[p].addr2;
      const Port port = side == 0 ? kMap.pairs[p].port1 : kMap.pairs[p].port2;
      const uint8_t bit = side == 0 ? kMap.pairs[p].bit1 : kMap.pairs[p].bit2;
      uint8_t idx = 0xFF;
      for (uint8_t c = 0; c < mapChips; ++c) {
        if (addrs[c] == addr) {
          idx = c;
          break;
        }
      }
      if (idx == 0xFF) {
        if (mapChips >= kMaxChips) continue;
        idx = mapChips++;
        addrs[idx] = addr;
        masks[idx][0] = 0;
        masks[idx][1] = 0;
      }
      masks[idx][port == kPortA ? 0 : 1] =
          static_cast<uint8_t>(masks[idx][port == kPortA ? 0 : 1] |
                               (1u << bit));
    }
  }
  // Bottom identification line: chip address(es) + worst pair, e.g.
  // "20 p6 A6-B6" (two chips: "20+21 ...", more: "20+N ..."). Built
  // with bounded directives only (%02X on uint8 addresses, %u on
  // known-narrow values) so the compiler can prove no truncation.
  char detail[22];
  detail[0] = '\0';
  if (worst < kMap.pairCount) {
    const JumperPair& jp = kMap.pairs[worst];
    const unsigned p = static_cast<unsigned>(worst);
    const char c1 = (jp.port1 == kPortA) ? 'A' : 'B';
    const unsigned b1 = static_cast<unsigned>(jp.bit1);
    const char c2 = (jp.port2 == kPortA) ? 'A' : 'B';
    const unsigned b2 = static_cast<unsigned>(jp.bit2);
    if (mapChips == 1) {
      snprintf(detail, sizeof(detail), "%02X p%u %c%u-%c%u",
               static_cast<unsigned>(addrs[0]), p, c1, b1, c2, b2);
    } else if (mapChips == 2) {
      snprintf(detail, sizeof(detail), "%02X+%02X p%u %c%u-%c%u",
               static_cast<unsigned>(addrs[0]),
               static_cast<unsigned>(addrs[1]), p, c1, b1, c2, b2);
    } else {
      // uint8_t keeps the value provably in [0, 255] for the compiler
      // (mapChips - 1 as int could wrap to a huge unsigned).
      const uint8_t more = static_cast<uint8_t>(mapChips - 1);
      snprintf(detail, sizeof(detail), "%02X+%u p%u %c%u-%c%u",
               static_cast<unsigned>(addrs[0]), static_cast<unsigned>(more),
               p, c1, b1, c2, b2);
    }
  }
  gOled.showVerdict(v.green, v.stepsRun, v.faults, names, mapChips, masks,
                    detail);
  digitalWrite(LED_BUILTIN, HIGH);
  gRunComplete = true;
}

void run() {
  gStepsRun = 0;
  gFaultCount = 0;
  gClasses = kClassNone;
  gLastFault[0] = '\0';
  for (uint8_t i = 0; i < kMaxPairs; ++i) {
    gAttempts[i] = 0;
    gFaults[i] = 0;
    gFaultsSlowest[i] = 0;
  }
  for (uint8_t b = 0; b < kMaxBlocks; ++b) {
    gFaultsByBlock[b] = 0;
  }
  const uint16_t stepsPerRep = stepCount(kMap);
  Serial.printf(
      "{\"event\":\"run_start\",\"seq\":1,\"pairs\":%u,\"blocks\":%u,"
      "\"reps\":%u,\"steps_per_rep\":%u}\n",
      static_cast<unsigned>(kMap.pairCount),
      static_cast<unsigned>(kBlockCount),
      static_cast<unsigned>(JIG_MATRIX_REPEATS),
      static_cast<unsigned>(stepsPerRep));

  // Safety first: validate the declared map before ANY direction write.
  const MapFault mapFault = validateMap(kMap);
  if (mapFault != kMapOk) {
    Serial.printf(
        "{\"event\":\"refused\",\"class\":\"setup\",\"cause\":\"%s\"}\n",
        mapFaultName(mapFault));
    gOled.showRefused(mapFaultName(mapFault));
    verdictAndHalt(0, 1, kClassSetup);
    return;
  }

  // Discovery: who is on the bus?
  gFoundCount = 0;
  gUnknownResponders = 0;
  jigScanBus(0x03, 0x77, onScanHit, nullptr);
  {
    char chips[kMaxChips * 4 + 2];  // "20 " per chip
    chips[0] = '\0';
    for (uint8_t i = 0; i < gFoundCount; ++i) {
      snprintf(chips + strlen(chips), sizeof(chips) - strlen(chips), "%s%02X",
               i == 0 ? "" : " ", static_cast<unsigned>(gFound[i]));
    }
    Serial.printf(
        "{\"event\":\"scan\",\"expanders\":[%s],\"unknown\":%u}\n",
        chips, static_cast<unsigned>(gUnknownResponders));
    gOled.showInventory(gFound, gFoundCount, gUnknownResponders);
  }

  // Chips named by the map but absent: setup fault, no bit-fault cascade.
  uint16_t setupFaults = 0;
  for (uint8_t i = 0; i < kMap.pairCount; ++i) {
    for (uint8_t side = 0; side < 2; ++side) {
      const uint8_t addr =
          side == 0 ? kMap.pairs[i].addr1 : kMap.pairs[i].addr2;
      bool present = false;
      for (uint8_t f = 0; f < gFoundCount; ++f) {
        if (gFound[f] == addr) {
          present = true;
          break;
        }
      }
      if (!present) {
        Serial.printf(
            "{\"event\":\"setup_fault\",\"class\":\"setup\","
            "\"cause\":\"chip_absent\",\"addr\":\"0x%02X\"}\n",
            static_cast<unsigned>(addr));
        setupFaults++;
        break;  // one absence report per pair is enough
      }
    }
  }
  // Chips found but not covered by the map: present-not-walkable.
  for (uint8_t f = 0; f < gFoundCount; ++f) {
    if (!mapNamesChip(gFound[f])) {
      Serial.printf(
          "{\"event\":\"scan_note\",\"addr\":\"0x%02X\","
          "\"note\":\"present_not_walkable\"}\n",
          static_cast<unsigned>(gFound[f]));
    }
  }
  if (setupFaults > 0) {
    verdictAndHalt(0, setupFaults, kClassSetup);
    return;
  }

  // Register health floor for every found chip: ACK already proven by
  // discovery, so probe IODIR write-readback on both ports.
  for (uint8_t f = 0; f < gFoundCount; ++f) {
    const uint8_t addr = gFound[f];
    bool ok = true;
    for (uint8_t side = 0; side < 2 && ok; ++side) {
      const bool isPortA = (side == 0);
      if (jigWriteReg(addr, jigRegFor(isPortA, kRegIODIR), 0xFF) != kI2cOk) {
        ok = false;
        break;
      }
      uint8_t readback = 0;
      if (jigReadReg(addr, jigRegFor(isPortA, kRegIODIR), &readback) !=
              kI2cOk ||
          readback != 0xFF) {
        ok = false;
        break;
      }
    }
    Serial.printf(
        "{\"event\":\"chip_health\",\"addr\":\"0x%02X\",\"ok\":%s}\n",
        static_cast<unsigned>(addr), ok ? "true" : "false");
    if (!ok) {
      Serial.printf(
          "{\"event\":\"setup_fault\",\"class\":\"setup\","
          "\"cause\":\"register_fault\",\"addr\":\"0x%02X\"}\n",
          static_cast<unsigned>(addr));
      verdictAndHalt(0, 1, kClassSetup);
      return;
    }
  }

  // The matrix: repetitions of the rate-block sweep; each block runs
  // both directions at its pace and clock. OLED updates at block
  // boundaries only.
  const uint16_t totalSteps = static_cast<uint16_t>(
      static_cast<uint32_t>(stepsPerRep) * kBlockCount * JIG_MATRIX_REPEATS);
  for (uint8_t rep = 0; rep < JIG_MATRIX_REPEATS; ++rep) {
    for (uint8_t block = 0; block < kBlockCount; ++block) {
      const RateBlock& rb = kRateBlocks[block];
      Wire.setClock(rb.i2cKhz * 1000UL);
      Serial.printf(
          "{\"event\":\"rate\",\"rep\":%u,\"block\":%u,\"delay_ms\":%u,"
          "\"i2c_khz\":%u}\n",
          static_cast<unsigned>(rep), static_cast<unsigned>(block),
          static_cast<unsigned>(rb.delayMs),
          static_cast<unsigned>(rb.i2cKhz));
      gOled.showBlock(block, kBlockCount, rb.i2cKhz, rb.delayMs, gStepsRun,
                      totalSteps, gFaultCount, gLastFault);
      const uint16_t stepsBefore = gStepsRun;
      const uint16_t faultsBefore = gFaultCount;
      bool ok = runPhaseInBlock(rep, block, 1, rb.delayMs);
      if (ok) ok = runPhaseInBlock(rep, block, 2, rb.delayMs);
      if (!ok) {
        verdictAndHalt(gStepsRun, static_cast<uint16_t>(gFaultCount + 1),
                       static_cast<uint8_t>(gClasses | kClassSetup));
        return;
      }
      Serial.printf(
          "{\"event\":\"soak\",\"rep\":%u,\"block\":%u,\"delay_ms\":%u,"
          "\"i2c_khz\":%u,\"steps\":%u,\"faults\":%u}\n",
          static_cast<unsigned>(rep), static_cast<unsigned>(block),
          static_cast<unsigned>(rb.delayMs),
          static_cast<unsigned>(rb.i2cKhz),
          static_cast<unsigned>(gStepsRun - stepsBefore),
          static_cast<unsigned>(gFaultCount - faultsBefore));
    }
  }

  // Per-pair soak health after the matrix. Retries were counted, so
  // stuck_at means every read failed and marginal means some passed —
  // and faults_slowest says whether the slowest pace saw it too (hard)
  // or only speed did (margin).
  for (uint8_t i = 0; i < kMap.pairCount; ++i) {
    if (gAttempts[i] == 0) continue;
    const BitHealth h = bitHealth(gAttempts[i], gFaults[i]);
    if (h == kBitHealthy) continue;
    Serial.printf(
        "{\"event\":\"pair_health\",\"pair\":%u,\"health\":\"%s\","
        "\"attempts\":%u,\"faults\":%u,\"faults_slowest\":%u}\n",
        static_cast<unsigned>(i),
        h == kBitStuckAt ? "stuck_at" : "marginal",
        static_cast<unsigned>(gAttempts[i]),
        static_cast<unsigned>(gFaults[i]),
        static_cast<unsigned>(gFaultsSlowest[i]));
  }

  verdictAndHalt(gStepsRun, gFaultCount, gClasses);
}

// =============================================
// ====   Setup and loop                    ====
// =============================================

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#if JIG_USE_BUTTON
  pinMode(D2, INPUT_PULLUP);
#endif
  Wire.begin();  // D4/D5 on cpNode-Xiao
#if JIG_USE_OLED
  gOled.begin();  // degrades headless when no panel answers
#endif

  Serial.printf(
      "{\"event\":\"boot\",\"image\":\"iox_jig\",\"version\":\"%s\","
      "\"pairs\":%u}\n",
      JIG_VERSION, static_cast<unsigned>(kMap.pairCount));
  gOled.showBoot(JIG_VERSION, kAutoRunDelaySecs);

  // Boot grace window: lets the operator read the boot screen and
  // gives the serial console time to attach before the run starts.
  const uint32_t bootAt = millis();
  while (millis() - bootAt < kAutoRunDelayMs) {
    Serial.flush();  // let the boot line land before the bus wakes
  }
  run();
}

void loop() {
#if JIG_USE_BUTTON
  if (gRunComplete && digitalRead(D2) == LOW) {
    digitalWrite(LED_BUILTIN, LOW);
    gRunComplete = false;
    run();
    return;
  }
#endif
  // Any serial line re-triggers a fresh run after the verdict.
  if (gRunComplete && Serial.available() > 0) {
    while (Serial.available() > 0) {
      Serial.read();
    }
    digitalWrite(LED_BUILTIN, LOW);
    gRunComplete = false;
    run();
  }
}
