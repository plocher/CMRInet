// test_conformance_fault.cpp — the shared conformance fault vocabulary.
//
// The value of one flat enum plus free classifiers is that an invalid
// (layer, attribution) pair cannot be constructed. These tests hold the
// classifiers total and exhaustive, so adding a fault without
// classifying it fails here rather than silently reading as kNone at a
// call site.
//
// VALIDATION: Design v1.2 D14: faults carry a layer and an attribution,
// derived rather than stored.

#include <string.h>

#include "ConformanceFault.h"
#include "unity.h"

using CMRInet::attributionOf;
using CMRInet::conformanceFaultString;
using CMRInet::conformanceLayerString;
using CMRInet::ConformanceFault;
using CMRInet::ConformanceLayer;
using CMRInet::FaultAttribution;
using CMRInet::faultAttributionString;
using CMRInet::layerOf;

void setUp(void) {}
void tearDown(void) {}

/// Every fault this library names. Kept explicit rather than derived
/// from a numeric range so adding an enumerator is a deliberate edit
/// here too -- the list is the test's own statement of completeness.
static const ConformanceFault kAllFaults[] = {
    ConformanceFault::kNone,
    ConformanceFault::kImageGeometryMismatch,
    ConformanceFault::kPacketUnexpectedAddress,
    ConformanceFault::kPacketUnexpectedType,
};

static const size_t kFaultCount =
    sizeof(kAllFaults) / sizeof(kAllFaults[0]);

// ------------------------------------------------------------ totality

// kNone is the only fault without a layer or an attribution. Any real
// fault that classified to kNone would be an unclassified enumerator
// reading as "no fault" at a call site -- silently wrong rather than
// loudly wrong.
static void test_every_real_fault_has_a_layer_and_an_attribution(void) {
  for (size_t i = 0; i < kFaultCount; ++i) {
    const ConformanceFault fault = kAllFaults[i];
    const bool isNone = (fault == ConformanceFault::kNone);
    const bool hasLayer = (layerOf(fault) != ConformanceLayer::kNone);
    const bool hasAttribution =
        (attributionOf(fault) != FaultAttribution::kNone);
    TEST_ASSERT_EQUAL_MESSAGE(!isNone, hasLayer,
                              "fault layer disagrees with kNone-ness");
    TEST_ASSERT_EQUAL_MESSAGE(!isNone, hasAttribution,
                              "fault attribution disagrees with kNone-ness");
  }
}

// The classifiers are pure lookups: asking twice must not differ, and
// they must not depend on call order.
static void test_classifiers_are_stable(void) {
  for (size_t i = 0; i < kFaultCount; ++i) {
    const ConformanceFault fault = kAllFaults[i];
    TEST_ASSERT_EQUAL(layerOf(fault), layerOf(fault));
    TEST_ASSERT_EQUAL(attributionOf(fault), attributionOf(fault));
  }
}

// --------------------------------------------------------- assignments

// Geometry mismatch is the fault that motivated the taxonomy. It is an
// image-layer *disagreement*: the Node is doing exactly what it was
// built to do, and the remedy is a configuration fix.
static void test_geometry_mismatch_is_an_image_layer_disagreement(void) {
  TEST_ASSERT_EQUAL(ConformanceLayer::kImage,
                    layerOf(ConformanceFault::kImageGeometryMismatch));
  TEST_ASSERT_EQUAL(FaultAttribution::kDisagreement,
                    attributionOf(ConformanceFault::kImageGeometryMismatch));
}

static void test_packet_faults_sit_on_the_packet_layer(void) {
  TEST_ASSERT_EQUAL(ConformanceLayer::kPacket,
                    layerOf(ConformanceFault::kPacketUnexpectedAddress));
  TEST_ASSERT_EQUAL(ConformanceLayer::kPacket,
                    layerOf(ConformanceFault::kPacketUnexpectedType));
}

// An unexpected message type is the counterparty breaking the exchange
// rules, so it is a defect -- unlike the two configuration cases.
static void test_unexpected_type_is_a_defect(void) {
  TEST_ASSERT_EQUAL(FaultAttribution::kDefect,
                    attributionOf(ConformanceFault::kPacketUnexpectedType));
}

// The taxonomy is only useful if both attributions actually occur;
// a vocabulary where everything is a defect would not guide remediation.
static void test_both_attributions_are_populated(void) {
  bool sawDisagreement = false;
  bool sawDefect = false;
  for (size_t i = 0; i < kFaultCount; ++i) {
    switch (attributionOf(kAllFaults[i])) {
      case FaultAttribution::kDisagreement: sawDisagreement = true; break;
      case FaultAttribution::kDefect:       sawDefect = true; break;
      case FaultAttribution::kNone:         break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(sawDisagreement, "no disagreement-class fault");
  TEST_ASSERT_TRUE_MESSAGE(sawDefect, "no defect-class fault");
}

// ------------------------------------------------------------- strings

// Telemetry and bench tooling read these, so an unnamed enumerator must
// not silently surface as "unknown".
static void test_every_fault_has_a_distinct_name(void) {
  for (size_t i = 0; i < kFaultCount; ++i) {
    const char* name = conformanceFaultString(kAllFaults[i]);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE_MESSAGE(strcmp(name, "unknown") != 0,
                             "fault has no human-readable name");
    for (size_t j = i + 1; j < kFaultCount; ++j) {
      TEST_ASSERT_TRUE_MESSAGE(
          strcmp(name, conformanceFaultString(kAllFaults[j])) != 0,
          "two faults share a name");
    }
  }
}

static void test_layer_and_attribution_names_are_present(void) {
  const ConformanceLayer layers[] = {
      ConformanceLayer::kNone, ConformanceLayer::kImage,
      ConformanceLayer::kPacket, ConformanceLayer::kFraming};
  for (size_t i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(strcmp(conformanceLayerString(layers[i]), "unknown") != 0);
  }
  const FaultAttribution attributions[] = {
      FaultAttribution::kNone, FaultAttribution::kDisagreement,
      FaultAttribution::kDefect};
  for (size_t i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(
        strcmp(faultAttributionString(attributions[i]), "unknown") != 0);
  }
}

// kFraming is defined but deliberately has no members yet: framing
// faults resist the two-value attribution. This test records that as an
// intentional state rather than an oversight -- when framing faults are
// classified, this test is the one that must be updated.
static void test_framing_layer_has_no_members_yet(void) {
  for (size_t i = 0; i < kFaultCount; ++i) {
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        static_cast<int>(ConformanceLayer::kFraming),
        static_cast<int>(layerOf(kAllFaults[i])),
        "a framing fault was added without settling its attribution");
  }
}

// ----------------------------------------------------------------- runner

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_every_real_fault_has_a_layer_and_an_attribution);
  RUN_TEST(test_classifiers_are_stable);
  RUN_TEST(test_geometry_mismatch_is_an_image_layer_disagreement);
  RUN_TEST(test_packet_faults_sit_on_the_packet_layer);
  RUN_TEST(test_unexpected_type_is_a_defect);
  RUN_TEST(test_both_attributions_are_populated);
  RUN_TEST(test_every_fault_has_a_distinct_name);
  RUN_TEST(test_layer_and_attribution_names_are_present);
  RUN_TEST(test_framing_layer_has_no_members_yet);
  return UNITY_END();
}
