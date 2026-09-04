// test_node_init.cpp — golden vectors for per-NDP I-body builders.
// Evidence: docs/research/node-type-init-bodies.md

#include <string.h>

#include "NodeInit.h"
#include "unity.h"

using CMRInet::buildCpnodeInitBody;
using CMRInet::buildSminiInitBody;
using CMRInet::buildUsicFamilyInitBody;
using CMRInet::CpnodeInit;
using CMRInet::InitBody;
using CMRInet::InitBuildStatus;
using CMRInet::NodeType;
using CMRInet::SminiInit;
using CMRInet::UsicFamilyInit;
using CMRInet::nodeTypeFromNdp;
using CMRInet::nodeTypeNdp;

void setUp(void) {}
void tearDown(void) {}

void test_ndp_map_fielded_letters(void) {
  NodeType t;
  TEST_ASSERT_TRUE(nodeTypeFromNdp('C', t));
  TEST_ASSERT_EQUAL(NodeType::kCpnode, t);
  TEST_ASSERT_TRUE(nodeTypeFromNdp('M', t));
  TEST_ASSERT_EQUAL(NodeType::kSmini, t);
  TEST_ASSERT_TRUE(nodeTypeFromNdp('N', t));
  TEST_ASSERT_EQUAL(NodeType::kUsic, t);
  TEST_ASSERT_TRUE(nodeTypeFromNdp('X', t));
  TEST_ASSERT_EQUAL(NodeType::kSusic, t);
  TEST_ASSERT_FALSE(nodeTypeFromNdp('O', t));  // CPMEGA deferred
  TEST_ASSERT_EQUAL('M', nodeTypeNdp(NodeType::kSmini));
}

void test_cpnode_default_body_matches_current_host(void) {
  // Golden: NI=2 NO=0 opts=0 dH/dL=0
  // 43 00 00 00 00 02 00 FF FF FF FF FF FF
  CpnodeInit init;
  init.inputBytes = 2;
  init.outputBytes = 0;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kOk, buildCpnodeInitBody(init, 0, 0, body));
  TEST_ASSERT_EQUAL_UINT8(13, body.length);
  const uint8_t expect[13] = {
      0x43, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, body.data, 13);
}

void test_cpnode_opts_and_delay_round_trip(void) {
  // JMRI delay 2000 → dH=0x07 dL=0xD0; NI=2 NO=3; opts1 bit0 set
  CpnodeInit init;
  init.inputBytes = 2;
  init.outputBytes = 3;
  init.opts1 = 0x01;  // USECMRIX
  init.opts2 = 0x00;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kOk,
                    buildCpnodeInitBody(init, 0x07, 0xD0, body));
  TEST_ASSERT_EQUAL_UINT8(13, body.length);
  TEST_ASSERT_EQUAL_HEX8(0x43, body.data[0]);
  TEST_ASSERT_EQUAL_HEX8(0x07, body.data[1]);
  TEST_ASSERT_EQUAL_HEX8(0xD0, body.data[2]);
  TEST_ASSERT_EQUAL_HEX8(0x01, body.data[3]);
  TEST_ASSERT_EQUAL_HEX8(0x00, body.data[4]);
  TEST_ASSERT_EQUAL_HEX8(0x02, body.data[5]);
  TEST_ASSERT_EQUAL_HEX8(0x03, body.data[6]);
}

void test_cpnode_rejects_ni_above_255(void) {
  CpnodeInit init;
  init.inputBytes = 256;
  init.outputBytes = 0;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kBadGeometry,
                    buildCpnodeInitBody(init, 0, 0, body));
}

void test_smini_no_signals_body(void) {
  // Golden: 4D 00 00 00
  SminiInit init;
  init.ns = 0;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kOk, buildSminiInitBody(init, 0, 0, body));
  TEST_ASSERT_EQUAL_UINT8(4, body.length);
  const uint8_t expect[4] = {0x4D, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, body.data, 4);
}

void test_smini_with_ct_bytes(void) {
  SminiInit init;
  init.ns = 2;
  for (uint8_t i = 0; i < 6; ++i) {
    init.ct[i] = static_cast<uint8_t>(0x10 + i);
  }
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kOk, buildSminiInitBody(init, 0, 1, body));
  TEST_ASSERT_EQUAL_UINT8(10, body.length);
  TEST_ASSERT_EQUAL_HEX8(0x4D, body.data[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, body.data[1]);
  TEST_ASSERT_EQUAL_HEX8(0x01, body.data[2]);
  TEST_ASSERT_EQUAL_HEX8(0x02, body.data[3]);
  TEST_ASSERT_EQUAL_HEX8(0x10, body.data[4]);
  TEST_ASSERT_EQUAL_HEX8(0x15, body.data[9]);
}

void test_smini_rejects_ns_above_24(void) {
  SminiInit init;
  init.ns = 25;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kBadNs, buildSminiInitBody(init, 0, 0, body));
}

void test_susic_body_shape(void) {
  UsicFamilyInit init;
  init.ns = 2;
  init.ct[0] = 169;  // IOOO example from JMRI review
  init.ct[1] = 0x05;
  init.inputBytes = 8;
  init.outputBytes = 8;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kOk,
                    buildUsicFamilyInitBody(NodeType::kSusic, init, 0, 0, body));
  TEST_ASSERT_EQUAL_UINT8(6, body.length);
  TEST_ASSERT_EQUAL_HEX8(0x58, body.data[0]);  // 'X'
  TEST_ASSERT_EQUAL_HEX8(0x02, body.data[3]);
  TEST_ASSERT_EQUAL_HEX8(169, body.data[4]);
  TEST_ASSERT_EQUAL_HEX8(0x05, body.data[5]);
}

void test_usic_ndp_letter(void) {
  UsicFamilyInit init;
  init.ns = 1;
  init.ct[0] = 0x01;
  InitBody body;
  TEST_ASSERT_EQUAL(InitBuildStatus::kOk,
                    buildUsicFamilyInitBody(NodeType::kUsic, init, 0, 0, body));
  TEST_ASSERT_EQUAL_HEX8(0x4E, body.data[0]);  // 'N'
}

void test_usic_rejects_ns_zero(void) {
  UsicFamilyInit init;
  init.ns = 0;
  InitBody body;
  TEST_ASSERT_EQUAL(
      InitBuildStatus::kBadNs,
      buildUsicFamilyInitBody(NodeType::kSusic, init, 0, 0, body));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ndp_map_fielded_letters);
  RUN_TEST(test_cpnode_default_body_matches_current_host);
  RUN_TEST(test_cpnode_opts_and_delay_round_trip);
  RUN_TEST(test_cpnode_rejects_ni_above_255);
  RUN_TEST(test_smini_no_signals_body);
  RUN_TEST(test_smini_with_ct_bytes);
  RUN_TEST(test_smini_rejects_ns_above_24);
  RUN_TEST(test_susic_body_shape);
  RUN_TEST(test_usic_ndp_letter);
  RUN_TEST(test_usic_rejects_ns_zero);
  return UNITY_END();
}
