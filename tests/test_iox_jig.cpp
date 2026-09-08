// test_iox_jig.cpp — desktop tests for the IoxJig planning core
// (examples/IoxJig/jigplan.h, issue #32).
//
// The jig validates cpNode-IOX expander cards over plain I2C — no CMRI
// involved. jigplan.h is its Arduino-free half: jumper-map validation,
// pattern-matrix step planning, expected-value math, fault
// classification, and verdict aggregation. These tests exercise that
// seam with hand-worked maps and independently computed expectations.
//
// Step granularity is the whole driver-port BYTE: one step writes one
// pattern byte and judges every paired reader bit of the coupling at
// once. Per-bit attribution (which PAIR missed) is a separate mapping,
// pairForReaderBit/pairForDriverBit — that is what feeds per-pair soak
// health.

#include "unity.h"
// Use relative path since it's an example sketch (test_xiao_generators
// precedent).
#include "../examples/IoxJig/jigplan.h"

void setUp(void) {}
void tearDown(void) {}

using namespace jigplan;

namespace {

// The bench testbed: one MCP23017 at 0x20, port A jumpered to port B,
// bit i to bit i (8 conductors, one cable set).
const JumperPair kFullABPairs[] = {
    {0x20, kPortA, 0, 0x20, kPortB, 0},
    {0x20, kPortA, 1, 0x20, kPortB, 1},
    {0x20, kPortA, 2, 0x20, kPortB, 2},
    {0x20, kPortA, 3, 0x20, kPortB, 3},
    {0x20, kPortA, 4, 0x20, kPortB, 4},
    {0x20, kPortA, 5, 0x20, kPortB, 5},
    {0x20, kPortA, 6, 0x20, kPortB, 6},
    {0x20, kPortA, 7, 0x20, kPortB, 7},
};
const JigMap kFullAB = {kFullABPairs, 8};

// One cross-chip pair: 0x20 port A bit 3 wired to 0x21 port B bit 5.
const JumperPair kCrossChipPairs[] = {
    {0x20, kPortA, 3, 0x21, kPortB, 5},
};
const JigMap kCrossChip = {kCrossChipPairs, 1};

}  // namespace

// ---- map validation ----------------------------------------------------

void test_full_ab_map_is_valid() {
  TEST_ASSERT_EQUAL(kMapOk, validateMap(kFullAB));
}

void test_cross_chip_map_is_valid() {
  TEST_ASSERT_EQUAL(kMapOk, validateMap(kCrossChip));
}

void test_empty_map_is_valid() {
  const JigMap empty = {nullptr, 0};
  TEST_ASSERT_EQUAL(kMapOk, validateMap(empty));
}

void test_bit_out_of_range_rejected() {
  const JumperPair pairs[] = {{0x20, kPortA, 8, 0x20, kPortB, 0}};
  const JigMap map = {pairs, 1};
  TEST_ASSERT_EQUAL(kMapBadBit, validateMap(map));
}

void test_address_out_of_range_rejected() {
  // 0x3C is the OLED: a responder on the bus, but not an expander and
  // never a legal jumper endpoint.
  const JumperPair pairs[] = {{0x3C, kPortA, 0, 0x20, kPortB, 0}};
  const JigMap map = {pairs, 1};
  TEST_ASSERT_EQUAL(kMapBadAddr, validateMap(map));
}

void test_same_port_pair_rejected() {
  // A jumper between two pins of the SAME port can never be walked:
  // direction is per-port, so both ends would share one role.
  const JumperPair pairs[] = {{0x20, kPortA, 0, 0x20, kPortA, 1}};
  const JigMap map = {pairs, 1};
  TEST_ASSERT_EQUAL(kMapSamePortPair, validateMap(map));
}

void test_duplicate_endpoint_rejected() {
  // 0x20 B0 claimed by two pairs: one wire per pin, so the map lies.
  const JumperPair pairs[] = {
      {0x20, kPortA, 0, 0x20, kPortB, 0},
      {0x20, kPortA, 1, 0x20, kPortB, 0},
  };
  const JigMap map = {pairs, 2};
  TEST_ASSERT_EQUAL(kMapDuplicatePair, validateMap(map));
}

void test_role_conflict_rejected() {
  // Pair 1 makes 0x20 port B a reader; pair 2 makes it a driver. In one
  // phase that port would need to be OUTPUT and INPUT at once — and its
  // partner pair would see both jumper ends driven. This is the
  // structural both-OUT refusal: role consistency makes double-drive
  // unrepresentable.
  const JumperPair pairs[] = {
      {0x20, kPortA, 0, 0x20, kPortB, 0},
      {0x20, kPortB, 1, 0x21, kPortA, 0},
  };
  const JigMap map = {pairs, 2};
  TEST_ASSERT_EQUAL(kMapRoleConflict, validateMap(map));
}

// ---- pattern table ------------------------------------------------------

void test_pattern_count() {
  // 8 walker-set + 8 walker-cleared + 4 roll2 + 2 roll4 + 2 checker +
  // 2 flash = 26.
  TEST_ASSERT_EQUAL_UINT8(26, patternCount());
}

void test_pattern_walker_set() {
  TEST_ASSERT_EQUAL(kWalkerSet, patternAt(0).family);
  TEST_ASSERT_EQUAL_HEX8(0x01, patternAt(0).value);
  TEST_ASSERT_EQUAL_HEX8(0x02, patternAt(1).value);
  TEST_ASSERT_EQUAL_HEX8(0x80, patternAt(7).value);
}

void test_pattern_walker_cleared() {
  TEST_ASSERT_EQUAL(kWalkerCleared, patternAt(8).family);
  TEST_ASSERT_EQUAL_HEX8(0xFE, patternAt(8).value);
  TEST_ASSERT_EQUAL_HEX8(0xFD, patternAt(9).value);
  TEST_ASSERT_EQUAL_HEX8(0x7F, patternAt(15).value);
}

void test_pattern_roll2() {
  // Adjacent-bit pairs rolling through the byte: 00000011 -> 00001100
  // -> 00110000 -> 11000000.
  TEST_ASSERT_EQUAL(kRoll2, patternAt(16).family);
  TEST_ASSERT_EQUAL_HEX8(0x03, patternAt(16).value);
  TEST_ASSERT_EQUAL_HEX8(0x0C, patternAt(17).value);
  TEST_ASSERT_EQUAL_HEX8(0x30, patternAt(18).value);
  TEST_ASSERT_EQUAL_HEX8(0xC0, patternAt(19).value);
}

void test_pattern_roll4() {
  TEST_ASSERT_EQUAL(kRoll4, patternAt(20).family);
  TEST_ASSERT_EQUAL_HEX8(0x0F, patternAt(20).value);
  TEST_ASSERT_EQUAL_HEX8(0xF0, patternAt(21).value);
}

void test_pattern_checker() {
  // Maximum adjacent-bit alternation: every neighbor disagrees.
  TEST_ASSERT_EQUAL(kChecker, patternAt(22).family);
  TEST_ASSERT_EQUAL_HEX8(0x55, patternAt(22).value);
  TEST_ASSERT_EQUAL_HEX8(0xAA, patternAt(23).value);
}

void test_pattern_flash() {
  // All-off / all-on: every bit switches together — the simultaneous
  // switching stress (di/dt, pull-up headroom).
  TEST_ASSERT_EQUAL(kFlash, patternAt(24).family);
  TEST_ASSERT_EQUAL_HEX8(0x00, patternAt(24).value);
  TEST_ASSERT_EQUAL_HEX8(0xFF, patternAt(25).value);
}

void test_family_names() {
  TEST_ASSERT_EQUAL_STRING("walker_set", familyName(kWalkerSet));
  TEST_ASSERT_EQUAL_STRING("walker_cleared", familyName(kWalkerCleared));
  TEST_ASSERT_EQUAL_STRING("roll2", familyName(kRoll2));
  TEST_ASSERT_EQUAL_STRING("roll4", familyName(kRoll4));
  TEST_ASSERT_EQUAL_STRING("checker", familyName(kChecker));
  TEST_ASSERT_EQUAL_STRING("flash", familyName(kFlash));
}

void test_drive_pattern_for() {
  // The walker families are generated from this helper.
  TEST_ASSERT_EQUAL_HEX8(0x08, drivePatternFor(3, kPolaritySetInCleared));
  TEST_ASSERT_EQUAL_HEX8(0xF7, drivePatternFor(3, kPolarityClearedInSet));
}

// ---- couplings -----------------------------------------------------------

void test_couplings_full_ab() {
  // One driver port couples to one reader port per phase.
  TEST_ASSERT_EQUAL_UINT8(1, couplingCount(kFullAB, 1));
  const PortPair c1 = couplingAt(kFullAB, 1, 0);
  TEST_ASSERT_EQUAL_HEX8(0x20, c1.driverAddr);
  TEST_ASSERT_EQUAL(kPortA, c1.driverPort);
  TEST_ASSERT_EQUAL_HEX8(0x20, c1.readerAddr);
  TEST_ASSERT_EQUAL(kPortB, c1.readerPort);
  // Phase 2 flips the roles.
  const PortPair c2 = couplingAt(kFullAB, 2, 0);
  TEST_ASSERT_EQUAL(kPortB, c2.driverPort);
  TEST_ASSERT_EQUAL(kPortA, c2.readerPort);
}

void test_couplings_cross_chip() {
  TEST_ASSERT_EQUAL_UINT8(1, couplingCount(kCrossChip, 1));
  const PortPair c = couplingAt(kCrossChip, 1, 0);
  TEST_ASSERT_EQUAL_HEX8(0x20, c.driverAddr);
  TEST_ASSERT_EQUAL(kPortA, c.driverPort);
  TEST_ASSERT_EQUAL_HEX8(0x21, c.readerAddr);
  TEST_ASSERT_EQUAL(kPortB, c.readerPort);
}

// ---- step enumeration ------------------------------------------------------

void test_step_count() {
  // 2 phases x 1 coupling x 26 patterns = 52 for both maps.
  TEST_ASSERT_EQUAL_UINT16(52, stepCount(kFullAB));
  TEST_ASSERT_EQUAL_UINT16(52, stepCount(kCrossChip));
  const JigMap empty = {nullptr, 0};
  TEST_ASSERT_EQUAL_UINT16(0, stepCount(empty));
}

void test_full_ab_first_step() {
  const DriveStep s = stepAt(kFullAB, 0);
  TEST_ASSERT_EQUAL_UINT8(1, s.phase);
  TEST_ASSERT_EQUAL_UINT8(0, s.coupling);
  TEST_ASSERT_EQUAL_UINT8(0, s.patternIndex);
  TEST_ASSERT_EQUAL(kWalkerSet, s.family);
  TEST_ASSERT_EQUAL_HEX8(0x20, s.driverAddr);
  TEST_ASSERT_EQUAL(kPortA, s.driverPort);
  TEST_ASSERT_EQUAL_HEX8(0x20, s.readerAddr);
  TEST_ASSERT_EQUAL(kPortB, s.readerPort);
  TEST_ASSERT_EQUAL_HEX8(0x01, s.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0x01, s.expectedReader);
  TEST_ASSERT_EQUAL_HEX8(0xFF, s.pairedMask);
}

void test_full_ab_flash_steps_drive_all_paired_bits() {
  // Pattern 24 (all-off): every paired reader bit must see 0.
  const DriveStep off = stepAt(kFullAB, 24);
  TEST_ASSERT_EQUAL(kFlash, off.family);
  TEST_ASSERT_EQUAL_HEX8(0x00, off.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0x00, off.expectedReader);
  // Pattern 25 (all-on).
  const DriveStep on = stepAt(kFullAB, 25);
  TEST_ASSERT_EQUAL_HEX8(0xFF, on.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0xFF, on.expectedReader);
}

void test_full_ab_checker_step() {
  const DriveStep s = stepAt(kFullAB, 22);
  TEST_ASSERT_EQUAL_HEX8(0x55, s.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0x55, s.expectedReader);
}

void test_full_ab_phase2_flips_roles() {
  // Step 26 = phase 2, coupling 0, pattern 0: port B now drives.
  const DriveStep s = stepAt(kFullAB, 26);
  TEST_ASSERT_EQUAL_UINT8(2, s.phase);
  TEST_ASSERT_EQUAL(kPortB, s.driverPort);
  TEST_ASSERT_EQUAL(kPortA, s.readerPort);
  TEST_ASSERT_EQUAL_HEX8(0x01, s.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0x01, s.expectedReader);
}

void test_cross_chip_expected_reader_marks_unpaired_bits_high() {
  // 0x20 A3 -> 0x21 B5. Only bit 3 of the driver byte is paired; only
  // bit 5 of the reader byte is judged. Unpaired reader bits sit at
  // their pull-up (1).
  // walker_set 0x01: driver bit 3 == 0 -> reader bit 5 sees 0 -> 0xDF.
  const DriveStep s0 = stepAt(kCrossChip, 0);
  TEST_ASSERT_EQUAL_HEX8(0x01, s0.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0xDF, s0.expectedReader);
  TEST_ASSERT_EQUAL_HEX8(0x20, s0.pairedMask);
  // walker_set 0x08 (pattern index 3): bit 3 == 1 -> reader bit 5 = 1.
  const DriveStep hot = stepAt(kCrossChip, 3);
  TEST_ASSERT_EQUAL_HEX8(0x08, hot.drivePattern);
  TEST_ASSERT_EQUAL_HEX8(0xFF, hot.expectedReader);
  // roll4 0x0F (index 20): bit 3 == 1 -> 0xFF; roll4 0xF0 (21): bit 3
  // == 0 -> 0xDF.
  TEST_ASSERT_EQUAL_HEX8(0xFF, stepAt(kCrossChip, 20).expectedReader);
  TEST_ASSERT_EQUAL_HEX8(0xDF, stepAt(kCrossChip, 21).expectedReader);
  // Phase 2 drives from 0x21 B5 and reads 0x20 A3.
  const DriveStep p2 = stepAt(kCrossChip, 26);
  TEST_ASSERT_EQUAL_HEX8(0x21, p2.driverAddr);
  TEST_ASSERT_EQUAL(kPortB, p2.driverPort);
  TEST_ASSERT_EQUAL_HEX8(0x20, p2.readerAddr);
  TEST_ASSERT_EQUAL(kPortA, p2.readerPort);
  TEST_ASSERT_EQUAL_HEX8(0x08, p2.pairedMask);  // reader A bit 3 judged
}

// ---- bit -> pair attribution -----------------------------------------------

void test_pair_for_reader_bit_full_ab() {
  for (uint8_t bit = 0; bit < 8; ++bit) {
    TEST_ASSERT_EQUAL_UINT8(bit,
                            pairForReaderBit(kFullAB, 1, 0x20, kPortB, bit));
    // Phase 2 reads from port A.
    TEST_ASSERT_EQUAL_UINT8(bit,
                            pairForReaderBit(kFullAB, 2, 0x20, kPortA, bit));
  }
}

void test_pair_for_reader_bit_cross_chip() {
  TEST_ASSERT_EQUAL_UINT8(0, pairForReaderBit(kCrossChip, 1, 0x21, kPortB, 5));
  // Unpaired reader bits and wrong ports have no pair.
  TEST_ASSERT_EQUAL_UINT8(0xFF,
                          pairForReaderBit(kCrossChip, 1, 0x21, kPortB, 4));
  TEST_ASSERT_EQUAL_UINT8(0xFF,
                          pairForReaderBit(kCrossChip, 1, 0x20, kPortA, 3));
}

void test_pair_for_driver_bit() {
  TEST_ASSERT_EQUAL_UINT8(0, pairForDriverBit(kCrossChip, 1, 0x20, kPortA, 3));
  // Unpaired driver bits (driven but not judged) have no pair — a
  // self-read miss there is a chip fault with no pair to name.
  TEST_ASSERT_EQUAL_UINT8(0xFF,
                          pairForDriverBit(kCrossChip, 1, 0x20, kPortA, 0));
  // Phase 2 drives from the other end.
  TEST_ASSERT_EQUAL_UINT8(0, pairForDriverBit(kCrossChip, 2, 0x21, kPortB, 5));
  for (uint8_t bit = 0; bit < 8; ++bit) {
    TEST_ASSERT_EQUAL_UINT8(
        bit, pairForDriverBit(kFullAB, 1, 0x20, kPortA, bit));
  }
}

// ---- phase roles ---------------------------------------------------------

void test_phase_roles_full_ab() {
  ChipRoles roles[4];
  const uint8_t n1 = phaseRoles(kFullAB, 1, roles, 4);
  TEST_ASSERT_EQUAL_UINT8(1, n1);
  TEST_ASSERT_EQUAL_HEX8(0x20, roles[0].addr);
  TEST_ASSERT_EQUAL(kRoleDriver, roles[0].portA);
  TEST_ASSERT_EQUAL(kRoleReader, roles[0].portB);
  const uint8_t n2 = phaseRoles(kFullAB, 2, roles, 4);
  TEST_ASSERT_EQUAL_UINT8(1, n2);
  TEST_ASSERT_EQUAL(kRoleReader, roles[0].portA);
  TEST_ASSERT_EQUAL(kRoleDriver, roles[0].portB);
}

void test_phase_roles_cross_chip() {
  ChipRoles roles[4];
  const uint8_t n = phaseRoles(kCrossChip, 1, roles, 4);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  // First-appearance order: each pair lists its driver-side chip first.
  TEST_ASSERT_EQUAL_HEX8(0x20, roles[0].addr);
  TEST_ASSERT_EQUAL(kRoleDriver, roles[0].portA);
  TEST_ASSERT_EQUAL(kRoleUnused, roles[0].portB);
  TEST_ASSERT_EQUAL_HEX8(0x21, roles[1].addr);
  TEST_ASSERT_EQUAL(kRoleUnused, roles[1].portA);
  TEST_ASSERT_EQUAL(kRoleReader, roles[1].portB);
}

// ---- fault classification ------------------------------------------------

void test_classify_step_ok() {
  TEST_ASSERT_EQUAL(kStepOk, classifyStep(0x01, 0x01, 0x01, 0x01, 0xFF));
}

void test_classify_step_driver_fault() {
  // Self-read disagrees with the commanded pattern: the driver stage
  // failed, and the loopback read is moot.
  TEST_ASSERT_EQUAL(kStepDriverFault,
                    classifyStep(0x01, 0x00, 0x01, 0x01, 0xFF));
}

void test_classify_step_loopback_fault() {
  // Self-read healthy but a paired reader bit missed: the fault is
  // downstream of the driver (jumper conductor or reader pin).
  TEST_ASSERT_EQUAL(kStepLoopbackFault,
                    classifyStep(0x01, 0x01, 0x01, 0x00, 0xFF));
}

void test_classify_step_ignores_unpaired_reader_bits() {
  // Only bit 0 is paired; noise on floating bit 4 must not fail the step.
  TEST_ASSERT_EQUAL(kStepOk, classifyStep(0x01, 0x01, 0x01, 0x11, 0x01));
}

void test_classify_step_partial_mask_fault() {
  // Cross-chip shape: drive 0x20 A = 0xF7, reader 0x21 B bit 5 paired.
  TEST_ASSERT_EQUAL(kStepLoopbackFault,
                    classifyStep(0xF7, 0xF7, 0xDF, 0xFF, 0x20));
}

void test_classify_step_multi_bit_pattern() {
  // Checker pattern with the whole reader byte collapsed to 0: still
  // one loopback fault for the step; the mismatch byte names the bits.
  TEST_ASSERT_EQUAL(kStepLoopbackFault,
                    classifyStep(0x55, 0x55, 0x55, 0x00, 0xFF));
  TEST_ASSERT_EQUAL_HEX8(0x55, static_cast<uint8_t>((0x55 ^ 0x00) & 0xFF));
}

void test_class_for_step_fault() {
  TEST_ASSERT_EQUAL_UINT8(kClassNone, classForStepFault(kStepOk));
  TEST_ASSERT_EQUAL_UINT8(kClassDriver, classForStepFault(kStepDriverFault));
  TEST_ASSERT_EQUAL_UINT8(kClassLoopback, classForStepFault(kStepLoopbackFault));
}

// ---- counters and verdict ------------------------------------------------

void test_bit_health() {
  TEST_ASSERT_EQUAL(kBitHealthy, bitHealth(10, 0));
  TEST_ASSERT_EQUAL(kBitStuckAt, bitHealth(10, 10));
  // Some attempts failed: marginal territory — flaky contact or
  // speed-sensitive, distinguished by the per-block fault data.
  TEST_ASSERT_EQUAL(kBitMarginal, bitHealth(10, 3));
  // Never-walked bits are reported elsewhere, not as healthy-by-soak.
  TEST_ASSERT_EQUAL(kBitHealthy, bitHealth(0, 0));
}

void test_all_stuck() {
  const BitHealth h1[] = {kBitStuckAt, kBitStuckAt};
  TEST_ASSERT_TRUE(allStuck(h1, 2));
  const BitHealth h2[] = {kBitStuckAt, kBitMarginal};
  TEST_ASSERT_FALSE(allStuck(h2, 2));
  // Empty is not "all stuck" — a whole-port escalation needs evidence.
  TEST_ASSERT_FALSE(allStuck(nullptr, 0));
}

void test_verdict_aggregation() {
  const Verdict green = makeVerdict(64, 0, kClassNone);
  TEST_ASSERT_TRUE(green.green);
  TEST_ASSERT_EQUAL_UINT16(64, green.stepsRun);
  TEST_ASSERT_EQUAL_UINT16(0, green.faults);
  const Verdict red = makeVerdict(64, 2, kClassLoopback);
  TEST_ASSERT_FALSE(red.green);
  TEST_ASSERT_EQUAL_UINT16(2, red.faults);
  TEST_ASSERT_EQUAL_UINT8(kClassLoopback, red.classes);
  // A setup fault with zero steps run (refused map) is still RED.
  const Verdict refused = makeVerdict(0, 1, kClassSetup);
  TEST_ASSERT_FALSE(refused.green);
  // Classes accumulate as a bitmask.
  const Verdict mixed = makeVerdict(64, 3, kClassDriver | kClassLoopback);
  TEST_ASSERT_FALSE(mixed.green);
  TEST_ASSERT_EQUAL_UINT8(kClassDriver | kClassLoopback, mixed.classes);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_full_ab_map_is_valid);
  RUN_TEST(test_cross_chip_map_is_valid);
  RUN_TEST(test_empty_map_is_valid);
  RUN_TEST(test_bit_out_of_range_rejected);
  RUN_TEST(test_address_out_of_range_rejected);
  RUN_TEST(test_same_port_pair_rejected);
  RUN_TEST(test_duplicate_endpoint_rejected);
  RUN_TEST(test_role_conflict_rejected);
  RUN_TEST(test_pattern_count);
  RUN_TEST(test_pattern_walker_set);
  RUN_TEST(test_pattern_walker_cleared);
  RUN_TEST(test_pattern_roll2);
  RUN_TEST(test_pattern_roll4);
  RUN_TEST(test_pattern_checker);
  RUN_TEST(test_pattern_flash);
  RUN_TEST(test_family_names);
  RUN_TEST(test_drive_pattern_for);
  RUN_TEST(test_couplings_full_ab);
  RUN_TEST(test_couplings_cross_chip);
  RUN_TEST(test_step_count);
  RUN_TEST(test_full_ab_first_step);
  RUN_TEST(test_full_ab_flash_steps_drive_all_paired_bits);
  RUN_TEST(test_full_ab_checker_step);
  RUN_TEST(test_full_ab_phase2_flips_roles);
  RUN_TEST(test_cross_chip_expected_reader_marks_unpaired_bits_high);
  RUN_TEST(test_pair_for_reader_bit_full_ab);
  RUN_TEST(test_pair_for_reader_bit_cross_chip);
  RUN_TEST(test_pair_for_driver_bit);
  RUN_TEST(test_phase_roles_full_ab);
  RUN_TEST(test_phase_roles_cross_chip);
  RUN_TEST(test_classify_step_ok);
  RUN_TEST(test_classify_step_driver_fault);
  RUN_TEST(test_classify_step_loopback_fault);
  RUN_TEST(test_classify_step_ignores_unpaired_reader_bits);
  RUN_TEST(test_classify_step_partial_mask_fault);
  RUN_TEST(test_classify_step_multi_bit_pattern);
  RUN_TEST(test_class_for_step_fault);
  RUN_TEST(test_bit_health);
  RUN_TEST(test_all_stuck);
  RUN_TEST(test_verdict_aggregation);
  return UNITY_END();
}
