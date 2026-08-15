// test_codec.cpp — byte-vector tests for the CMRInet serial codec.
//
// The test plan is the anti-checklist in docs/research/comparison.md §3
// plus the golden framing rules of the interop profile Part 2. Each test
// that pins a profile rule carries a VALIDATION tag (see
// docs/agents/validation-comments.md). The rest pin fielded defects from
// the research.
//
// Desktop-native: the codec has no Arduino dependencies, so these tests
// compile the exact library sources with the host compiler.

#include <string.h>

#include "CMRInet.h"
#include "unity.h"

using CMRInet::CMRIFrameDecoder;
using CMRInet::CMRIPacket;
using CMRInet::encodeFrame;
using CMRInet::kDle;
using CMRInet::kEtx;
using CMRInet::kMaxBody;
using CMRInet::kMaxWireFrame;
using CMRInet::kStx;
using CMRInet::kSyn;
using CMRInet::kUaOffset;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------- helpers

/// Build a packet for node address `addr` (UA = addr + 65).
static CMRIPacket makePacket(uint8_t addr, uint8_t mt, const uint8_t* body,
                             size_t len) {
  CMRIPacket p;
  p.ua = static_cast<uint8_t>(addr + kUaOffset);
  p.mt = mt;
  TEST_ASSERT_TRUE_MESSAGE(p.setBody(body, len), "setBody rejected test body");
  return p;
}

/// Feed a byte vector at one timestamp; return how many frames completed.
static int feedAll(CMRIFrameDecoder& d, const uint8_t* bytes, size_t len,
                   uint32_t nowMs) {
  int completed = 0;
  for (size_t i = 0; i < len; ++i) {
    if (d.feed(bytes[i], nowMs)) {
      ++completed;
    }
  }
  return completed;
}

// ---------------------------------------------------------------- encoder

// VALIDATION: Interop v1.1 2.1.1: a frame is two SYN/0xFF, then
// STX/0x02, UA, MT, body, ETX/0x03. Golden vector: poll of node
// address 5.
static void test_encode_poll_golden_vector(void) {
  const CMRIPacket p = makePacket(5, 'P', nullptr, 0);
  uint8_t wire[16] = {0};
  const size_t n = encodeFrame(p, wire, sizeof(wire));
  const uint8_t expected[] = {0xFF, 0xFF, 0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, wire, sizeof(expected));
}

// VALIDATION: Interop v1.1 2.1.1: exactly two SYNs — not one, not
// three (a third would drop the frame on fielded ArduinoCMRI nodes).
static void test_encode_emits_exactly_two_syns(void) {
  const CMRIPacket p = makePacket(0, 'P', nullptr, 0);
  uint8_t wire[16] = {0};
  const size_t n = encodeFrame(p, wire, sizeof(wire));
  TEST_ASSERT_EQUAL_size_t(6, n);
  TEST_ASSERT_EQUAL_HEX8(kSyn, wire[0]);
  TEST_ASSERT_EQUAL_HEX8(kSyn, wire[1]);
  TEST_ASSERT_EQUAL_HEX8(kStx, wire[2]);  // third byte is STX, not SYN
}

// VALIDATION: Interop v1.1 2.1.2, E1: STX/0x02, ETX/0x03, DLE/0x10 are
// escaped in every body.
static void test_encode_escapes_protocol_chars_in_body(void) {
  const uint8_t body[] = {0x02, 0x03, 0x10, 0x41};
  const CMRIPacket p = makePacket(5, 'T', body, sizeof(body));
  uint8_t wire[32] = {0};
  const size_t n = encodeFrame(p, wire, sizeof(wire));
  const uint8_t expected[] = {0xFF, 0xFF, 0x02, 0x46, 0x54,  // header
                              0x10, 0x02, 0x10, 0x03, 0x10, 0x10, 0x41,
                              0x03};
  TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, wire, sizeof(expected));
}

// VALIDATION: Interop v1.1 2.1.3: SYN/0xFF in a body is data and is
// never escaped.
static void test_encode_never_escapes_syn_value(void) {
  const uint8_t body[] = {0xFF};
  const CMRIPacket p = makePacket(5, 'T', body, sizeof(body));
  uint8_t wire[16] = {0};
  const size_t n = encodeFrame(p, wire, sizeof(wire));
  const uint8_t expected[] = {0xFF, 0xFF, 0x02, 0x46, 0x54, 0xFF, 0x03};
  TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, wire, sizeof(expected));
}

// VALIDATION: Interop v1.1 E3: the JMRI C-type (CPNODE) I-message
// dialect — escaped body bytes (E1 applies to I messages) and six raw,
// unescaped 0xFF pad bytes.
static void test_encode_jmri_ctype_init_body(void) {
  const uint8_t body[] = {'C',  0x00, 0x02, 0x00, 0x00, 0x04, 0x04,
                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const CMRIPacket p = makePacket(5, 'I', body, sizeof(body));
  uint8_t wire[32] = {0};
  const size_t n = encodeFrame(p, wire, sizeof(wire));
  const uint8_t expected[] = {0xFF, 0xFF, 0x02, 0x46, 0x49,        // header
                              0x43, 0x00, 0x10, 0x02, 0x00, 0x00,  // dL escaped
                              0x04, 0x04,                          // NI NO
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // raw pads
                              0x03};
  TEST_ASSERT_EQUAL_size_t(sizeof(expected), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, wire, sizeof(expected));
}

// VALIDATION: Interop v1.1 E7, 2.1.6: a fully escaped max body exactly
// fills kMaxWireFrame.
static void test_encode_max_body_worst_case_fits_staging(void) {
  uint8_t body[kMaxBody];
  memset(body, kDle, sizeof(body));  // every byte needs escaping
  const CMRIPacket p = makePacket(1, 'T', body, sizeof(body));
  static uint8_t wire[kMaxWireFrame];
  const size_t n = encodeFrame(p, wire, sizeof(wire));
  TEST_ASSERT_EQUAL_size_t(kMaxWireFrame, n);  // 6 + 2*kMaxBody
  TEST_ASSERT_EQUAL_HEX8(kEtx, wire[n - 1]);
}

// Validation at intake: oversized logical bodies are rejected, not sent.
static void test_encode_rejects_oversized_body(void) {
  CMRIPacket p = makePacket(1, 'T', nullptr, 0);
  p.length = static_cast<uint16_t>(kMaxBody + 1);  // forge a bad length
  uint8_t wire[kMaxWireFrame];
  TEST_ASSERT_EQUAL_size_t(0, encodeFrame(p, wire, sizeof(wire)));
}

// Anti-checklist: latent worst-case-escaping overflows (two fielded Hosts
// had them). A too-small buffer yields 0, never a partial frame.
static void test_encode_rejects_insufficient_capacity(void) {
  const uint8_t body[] = {0x02};  // escapes to two wire bytes
  const CMRIPacket p = makePacket(5, 'T', body, sizeof(body));
  uint8_t wire[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  // Needs 6 + 2 = 8 bytes; offer 7.
  TEST_ASSERT_EQUAL_size_t(0, encodeFrame(p, wire, 7));
  TEST_ASSERT_EQUAL_HEX8(0xAA, wire[0]);  // nothing was written
  TEST_ASSERT_EQUAL_size_t(8, encodeFrame(p, wire, 8));
}

// ---------------------------------------------------------------- decoder

// Encode -> decode round trip with escapes and raw 0xFF mixed in.
static void test_decode_roundtrip(void) {
  const uint8_t body[] = {0x00, 0x02, 0xFF, 0x03, 0x41, 0x10, 0x80};
  const CMRIPacket sent = makePacket(23, 'R', body, sizeof(body));
  uint8_t wire[64] = {0};
  const size_t n = encodeFrame(sent, wire, sizeof(wire));
  TEST_ASSERT_TRUE(n > 0);

  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, n, 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(sent.ua, got.ua);
  TEST_ASSERT_EQUAL_HEX8('R', got.mt);
  TEST_ASSERT_EQUAL_UINT16(sizeof(body), got.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got.body, sizeof(body));
}

// VALIDATION: Interop v1.1 2.2.1: hunt for a bare STX/0x02 — SYNs are
// not required.
static void test_decode_accepts_frame_without_syns(void) {
  const uint8_t wire[] = {0x02, 0x46, 0x50, 0x03};
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, sizeof(wire), 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x46, got.ua);
  TEST_ASSERT_EQUAL_HEX8('P', got.mt);
  TEST_ASSERT_EQUAL_UINT16(0, got.length);
}

// VALIDATION: Interop v1.1 2.2.1: SYNs are not counted against a
// frame. ArduinoCMRI drops the frame on a third SYN — the Host codec
// must not replicate that defect.
static void test_decode_tolerates_extra_syns(void) {
  const uint8_t wire[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x46, 0x50,
                          0x03};
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, sizeof(wire), 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8('P', got.mt);
}

// VALIDATION: Interop v1.1 2.2.3: raw 0xFF inside a body is data,
// never a resynchronization point. JMRI's C-type I bodies end with six
// raw 0xFF pads — this exact shape.
static void test_decode_ff_in_body_is_data_no_resync(void) {
  const uint8_t body[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const CMRIPacket sent = makePacket(5, 'I', body, sizeof(body));
  uint8_t wire[32] = {0};
  const size_t n = encodeFrame(sent, wire, sizeof(wire));

  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, n, 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_UINT16(6, got.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got.body, sizeof(body));
}

// VALIDATION: Interop v1.1 2.2.2: after DLE, the next byte is data
// with no interpretation — including an escaped 0x03 in the LAST body
// position (JMRI's dangling-DLE receiver bug ate the real ETX here).
static void test_decode_escaped_etx_in_last_body_position(void) {
  const uint8_t wire[] = {0x02, 0x46, 0x52, 0x41, 0x10, 0x03, 0x03};
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, sizeof(wire), 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  const uint8_t expected[] = {0x41, 0x03};
  TEST_ASSERT_EQUAL_UINT16(2, got.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, got.body, sizeof(expected));
}

// VALIDATION: Interop v1.1 2.2.2: DLE processes before the STX test —
// an escaped 0x02 mid-body is data, not a frame restart.
static void test_decode_escaped_stx_in_body_is_data(void) {
  const uint8_t wire[] = {0x02, 0x46, 0x52, 0x10, 0x02, 0x03};
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, sizeof(wire), 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_UINT16(1, got.length);
  TEST_ASSERT_EQUAL_HEX8(0x02, got.body[0]);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().framesRestarted);
}

// VALIDATION: Interop v1.1 2.2.4: an unescaped STX/0x02 mid-frame
// resets the frame. Nothing received before it is kept.
static void test_decode_unescaped_stx_restarts_frame(void) {
  const uint8_t wire[] = {0x02, 0x46, 0x54, 0x41, 0x42,  // partial frame
                          0x02, 0x47, 0x50, 0x03};       // real frame
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, sizeof(wire), 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x47, got.ua);
  TEST_ASSERT_EQUAL_HEX8('P', got.mt);
  TEST_ASSERT_EQUAL_UINT16(0, got.length);
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().framesRestarted);
}

// Anti-checklist (the inCnt bug): pre-STX noise must never be buffered
// into the body. One noise byte shifted whole bodies in two fielded nodes.
static void test_decode_pre_stx_noise_not_buffered(void) {
  const uint8_t wire[] = {0x55, 0xAA, 0x01,                    // line noise
                          0x02, 0x46, 0x52, 0x11, 0x22, 0x03}; // frame
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, sizeof(wire), 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  const uint8_t expected[] = {0x11, 0x22};
  TEST_ASSERT_EQUAL_UINT16(2, got.length);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, got.body, sizeof(expected));
}

// VALIDATION: Interop v1.1 2.4.1: DLE processing stays active while
// hunting/discarding. An escaped 0x02 seen between frames must not
// start a frame (a DLE-blind hunt was a bug in two fielded Hosts).
static void test_decode_hunt_is_dle_aware(void) {
  CMRIFrameDecoder d;
  // Escaped STX while hunting: everything through the stray ETX is debris
  // from a frame this station is discarding.
  const uint8_t debris[] = {0x10, 0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, debris, sizeof(debris), 0));
  TEST_ASSERT_FALSE(d.hasPacket());
  // A real frame right after decodes normally.
  const uint8_t frame[] = {0x02, 0x47, 0x50, 0x03};
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, frame, sizeof(frame), 1));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x47, got.ua);
}

// VALIDATION: Interop v1.1 2.2.6, 2.2.7: a frame that dies in a
// dangling DLE is discarded on timeout, and the decoder recovers on the
// next frame.
static void test_decode_dangling_dle_timeout_discards(void) {
  CMRIFrameDecoder d;
  d.setInterByteTimeoutMs(10);
  const uint8_t partial[] = {0x02, 0x46, 0x52, 0x41, 0x10};  // ends in DLE
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, partial, sizeof(partial), 1000));
  TEST_ASSERT_FALSE(d.hasPacket());

  TEST_ASSERT_TRUE(d.expireIdle(1100));  // gap > 10 ms: abandon
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().timeoutAborts);
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().danglingDle);
  TEST_ASSERT_FALSE(d.hasPacket());

  const uint8_t frame[] = {0x02, 0x47, 0x50, 0x03};
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, frame, sizeof(frame), 1200));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x47, got.ua);
}

// VALIDATION: Interop v1.1 2.2.6: a truncated frame is abandoned when
// the next traffic arrives after the inter-byte gap, and the new frame
// decodes cleanly (anti-checklist: blocking reads).
static void test_decode_truncated_frame_recovers_via_gap(void) {
  CMRIFrameDecoder d;
  d.setInterByteTimeoutMs(10);
  const uint8_t truncated[] = {0x02, 0x46, 0x52, 0x41, 0x42};  // no ETX
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, truncated, sizeof(truncated), 1000));

  const uint8_t frame[] = {0x02, 0x47, 0x50, 0x03};
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, frame, sizeof(frame), 2000));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x47, got.ua);
  TEST_ASSERT_EQUAL_UINT16(0, got.length);
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().timeoutAborts);
}

// VALIDATION: Interop v1.1 2.2.6: with the timeout disabled (0),
// arbitrarily gapped bytes still assemble — the reference Host lineage
// transmitted with interpreter-scale gaps between bytes.
static void test_decode_timeout_disabled_tolerates_gaps(void) {
  CMRIFrameDecoder d;
  d.setInterByteTimeoutMs(0);
  const uint8_t wire[] = {0x02, 0x46, 0x52, 0x41, 0x03};
  uint32_t now = 0;
  int completed = 0;
  for (size_t i = 0; i < sizeof(wire); ++i) {
    now += 1000;  // one second between bytes
    if (d.feed(wire[i], now)) {
      ++completed;
    }
  }
  TEST_ASSERT_EQUAL_INT(1, completed);
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_UINT16(1, got.length);
  TEST_ASSERT_EQUAL_HEX8(0x41, got.body[0]);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().timeoutAborts);
}

// VALIDATION: Interop v1.1 2.2.5: guard before every store. An
// oversized body aborts the frame, delivers nothing, and the decoder
// recovers.
static void test_decode_oversized_body_aborts_and_recovers(void) {
  CMRIFrameDecoder d;
  const uint8_t header[] = {0x02, 0x46, 0x52};
  feedAll(d, header, sizeof(header), 0);
  for (size_t i = 0; i < kMaxBody + 40; ++i) {
    TEST_ASSERT_FALSE(d.feed(0x41, 0));
  }
  TEST_ASSERT_FALSE(d.feed(kEtx, 0));  // stray ETX after the abort
  TEST_ASSERT_FALSE(d.hasPacket());
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().overflowAborts);

  const uint8_t frame[] = {0x02, 0x47, 0x50, 0x03};
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, frame, sizeof(frame), 1));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x47, got.ua);
}

// Anti-checklist (off-by-one overrun guards): bodies at kMaxBody-1 and
// kMaxBody decode exactly; the fielded bugs lived on these boundaries.
static void test_decode_body_length_boundaries(void) {
  static uint8_t body[kMaxBody];
  for (size_t i = 0; i < kMaxBody; ++i) {
    body[i] = static_cast<uint8_t>(i & 0x7Fu) | 0x20u;  // non-protocol bytes
  }
  static uint8_t wire[kMaxWireFrame];

  const size_t lengths[] = {kMaxBody - 1, kMaxBody};
  for (size_t li = 0; li < 2; ++li) {
    const CMRIPacket sent = makePacket(1, 'R', body, lengths[li]);
    const size_t n = encodeFrame(sent, wire, sizeof(wire));
    TEST_ASSERT_TRUE(n > 0);
    CMRIFrameDecoder d;
    TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, n, 0));
    CMRIPacket got;
    TEST_ASSERT_TRUE(d.take(got));
    TEST_ASSERT_EQUAL_UINT16(lengths[li], got.length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got.body, lengths[li]);
    TEST_ASSERT_EQUAL_UINT32(0, d.statistics().overflowAborts);
  }
}

// VALIDATION: Interop v1.1 2.2.8: parse into staging — nothing is
// visible until a valid ETX. The QBASIC Host committed early and
// reported an occupied block VACANT.
static void test_decode_commits_only_on_etx(void) {
  const uint8_t frame[] = {0x02, 0x46, 0x52, 0x41, 0x42, 0x03};
  CMRIFrameDecoder d;
  for (size_t i = 0; i + 1 < sizeof(frame); ++i) {
    d.feed(frame[i], 0);
    TEST_ASSERT_FALSE_MESSAGE(d.hasPacket(), "packet visible before ETX");
  }
  TEST_ASSERT_TRUE(d.feed(frame[sizeof(frame) - 1], 0));
  TEST_ASSERT_TRUE(d.hasPacket());
}

// Malformed header: ETX before UA and MT have arrived is discarded.
static void test_decode_etx_before_header_complete_is_abort(void) {
  CMRIFrameDecoder d;
  const uint8_t noUa[] = {0x02, 0x03};        // ETX in the UA slot
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, noUa, sizeof(noUa), 0));
  const uint8_t noMt[] = {0x02, 0x46, 0x03};  // ETX in the MT slot
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, noMt, sizeof(noMt), 0));
  TEST_ASSERT_EQUAL_UINT32(2, d.statistics().headerAborts);
  TEST_ASSERT_FALSE(d.hasPacket());

  const uint8_t frame[] = {0x02, 0x47, 0x50, 0x03};
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, frame, sizeof(frame), 1));
}

// Two back-to-back gapless frames decode in order when taken promptly.
static void test_decode_back_to_back_frames(void) {
  const uint8_t a[] = {0x02, 0x46, 0x50, 0x03};
  const uint8_t b[] = {0x02, 0x47, 0x52, 0x41, 0x03};
  CMRIFrameDecoder d;
  CMRIPacket got;

  TEST_ASSERT_EQUAL_INT(1, feedAll(d, a, sizeof(a), 0));
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x46, got.ua);

  TEST_ASSERT_EQUAL_INT(1, feedAll(d, b, sizeof(b), 0));
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x47, got.ua);
  TEST_ASSERT_EQUAL_UINT16(1, got.length);
  TEST_ASSERT_EQUAL_UINT32(2, d.statistics().framesDecoded);
}

// Arrival order is preserved under overrun: if the caller has not taken
// the previous packet, the newer frame is counted and dropped, never
// swapped in.
static void test_decode_ready_slot_overrun_keeps_oldest(void) {
  const uint8_t a[] = {0x02, 0x46, 0x50, 0x03};
  const uint8_t b[] = {0x02, 0x47, 0x50, 0x03};
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, a, sizeof(a), 0));
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, b, sizeof(b), 0));  // not taken yet
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().droppedPackets);
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0x46, got.ua);  // the oldest survived
  TEST_ASSERT_FALSE(d.take(got));
}

// VALIDATION: Interop v1.1 2.2.9: high-value bytes in every field
// survive intact (anti-checklist: signed-char traps).
static void test_decode_high_value_bytes(void) {
  const uint8_t body[] = {0x80, 0x9F, 0xFE, 0xFF};
  const CMRIPacket sent = makePacket(127, 'R', body, sizeof(body));  // UA 0xC0
  uint8_t wire[32] = {0};
  const size_t n = encodeFrame(sent, wire, sizeof(wire));
  CMRIFrameDecoder d;
  TEST_ASSERT_EQUAL_INT(1, feedAll(d, wire, n, 0));
  CMRIPacket got;
  TEST_ASSERT_TRUE(d.take(got));
  TEST_ASSERT_EQUAL_HEX8(0xC0, got.ua);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(body, got.body, sizeof(body));
}

// Exhaustive escaping symmetry: every possible single-byte body value
// round-trips through encode -> decode.
static void test_roundtrip_every_body_value(void) {
  for (int v = 0; v <= 0xFF; ++v) {
    const uint8_t body[] = {static_cast<uint8_t>(v)};
    const CMRIPacket sent = makePacket(5, 'T', body, 1);
    uint8_t wire[16] = {0};
    const size_t n = encodeFrame(sent, wire, sizeof(wire));
    TEST_ASSERT_TRUE(n > 0);
    CMRIFrameDecoder d;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, feedAll(d, wire, n, 0),
                                  "frame did not complete");
    CMRIPacket got;
    TEST_ASSERT_TRUE(d.take(got));
    TEST_ASSERT_EQUAL_UINT16(1, got.length);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(v), got.body[0]);
  }
}

// expireIdle() is a no-op while idle or mid-hunt.
static void test_expire_idle_noop_when_hunting(void) {
  CMRIFrameDecoder d;
  d.setInterByteTimeoutMs(10);
  TEST_ASSERT_FALSE(d.expireIdle(99999));
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().timeoutAborts);
}

// --------------------------------------------- gap observability (2.2.6)
//
// The 2.2.6 grace-band receive model: a lower nominal threshold splits
// "took longer than expected" (a non-fatal annotation) from "took so
// long I gave up" (the fatal abort). slowGaps counts the suspect band
// [nominalHi, abort); maxGapMs is a cumulative watermark including the
// fatal gap. Observability is off until setSlowGapThresholdsMs turns it
// on (lo=0). The abort limit is independent and unchanged.

// VALIDATION: Interop v1.1 2.2.6: observability off by default (lo=0);
// a fresh decoder records no gap counters.
static void test_decode_gap_observability_off_by_default(void) {
  CMRIFrameDecoder d;
  d.setInterByteTimeoutMs(0);  // avoid abort; isolate observability
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 100));  // gap=100: not recorded (off)
  TEST_ASSERT_FALSE(d.feed(frame[2], 100));
  TEST_ASSERT_TRUE(d.feed(frame[3], 100));
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().maxGapMs);
}

// VALIDATION: Interop v1.1 2.2.6: gaps below the nominal floor record
// nothing — a correctly tuned, well-running system stays at zero.
static void test_decode_gap_nominal_region_records_nothing(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(0);
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 5));  // gap=5 < 10: nominal
  TEST_ASSERT_FALSE(d.feed(frame[2], 5));
  TEST_ASSERT_TRUE(d.feed(frame[3], 5));
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().maxGapMs);
}

// VALIDATION: Interop v1.1 2.2.6: a gap in the grace band [lo, hi)
// stamps the watermark only — medium-explained slowness, tolerated.
static void test_decode_gap_grace_region_records_watermark_only(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(0);
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 15));  // gap=15 in [10,20): grace
  TEST_ASSERT_FALSE(d.feed(frame[2], 15));  // gap=0
  TEST_ASSERT_TRUE(d.feed(frame[3], 15));
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(15, d.statistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().timeoutAborts);
}

// VALIDATION: Interop v1.1 2.2.6: a gap in the suspect band [hi, abort)
// increments slowGaps and stamps the watermark, without aborting.
static void test_decode_gap_slow_region_increments_slowGaps(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(100);
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 50));  // gap=50 in [20,100): slow
  TEST_ASSERT_FALSE(d.feed(frame[2], 50));
  TEST_ASSERT_TRUE(d.feed(frame[3], 50));
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(50, d.statistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().timeoutAborts);
}

// VALIDATION: Interop v1.1 2.2.6: a fatal gap (> abort) abandons the
// frame and stamps the watermark with the fatal gap itself; slowGaps is
// NOT incremented (the fatal counter subsumes it — no double count).
static void test_decode_gap_fatal_region_aborts_and_stamps_watermark(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(30);
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 40));  // gap=40 > 30: fatal, abort
  TEST_ASSERT_FALSE(d.feed(frame[2], 40));  // hunting: ignored
  TEST_ASSERT_FALSE(d.feed(frame[3], 40));  // hunting: ignored
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().timeoutAborts);
  TEST_ASSERT_EQUAL_UINT32(40, d.statistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().slowGaps);
  TEST_ASSERT_FALSE(d.hasPacket());
}

// VALIDATION: Interop v1.1 2.2.6: with the abort limit disabled (0,
// conformance exception), a gap that would be fatal is observed as slow
// instead — the receiver annotates without ever abandoning the frame.
static void test_decode_gap_conformance_observes_without_aborting(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(0);  // conformance: tolerate any gap
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 50));  // gap=50: slow (no fatal)
  TEST_ASSERT_FALSE(d.feed(frame[2], 50));
  TEST_ASSERT_TRUE(d.feed(frame[3], 50));
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(50, d.statistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().timeoutAborts);
}

// VALIDATION: Interop v1.1 2.2.6: lo=0 disables observability entirely
// even if hi is nonzero — the codec default (it does not know baud).
static void test_decode_gap_disabled_when_lo_zero(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(0, 20);  // lo=0: off
  d.setInterByteTimeoutMs(0);
  const uint8_t frame[] = {0x02, 0x46, 0x50, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));
  TEST_ASSERT_FALSE(d.feed(frame[1], 50));
  TEST_ASSERT_FALSE(d.feed(frame[2], 50));
  TEST_ASSERT_TRUE(d.feed(frame[3], 50));
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().slowGaps);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().maxGapMs);
}

// VALIDATION: Interop v1.1 2.2.6: maxGapMs is a max watermark, not the
// last gap; slowGaps counts only the gaps at/above hi. A frame with
// gaps 15 (grace), 30 (slow), 12 (grace) records maxGapMs=30, slowGaps=1.
static void test_decode_gap_watermark_tracks_max_not_last(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(0);
  const uint8_t frame[] = {0x02, 0x46, 0x52, 0x41, 0x03};
  TEST_ASSERT_FALSE(d.feed(frame[0], 0));    // STX
  TEST_ASSERT_FALSE(d.feed(frame[1], 15));   // UA: gap=15 (grace)
  TEST_ASSERT_FALSE(d.feed(frame[2], 45));   // MT: gap=30 (slow)
  TEST_ASSERT_FALSE(d.feed(frame[3], 57));   // body: gap=12 (grace)
  TEST_ASSERT_TRUE(d.feed(frame[4], 57));    // ETX: gap=0
  TEST_ASSERT_EQUAL_UINT32(30, d.statistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().slowGaps);
}

// VALIDATION: Interop v1.1 2.2.6: a frame that dies in line silence
// (no arriving byte to measure the gap) still stamps the watermark via
// expireIdle — the elapsed silence is a lower bound on the fatal gap.
static void test_decode_gap_fatal_silence_stamps_watermark(void) {
  CMRIFrameDecoder d;
  d.setSlowGapThresholdsMs(10, 20);
  d.setInterByteTimeoutMs(30);
  const uint8_t partial[] = {0x02, 0x46, 0x52, 0x41};  // no ETX
  TEST_ASSERT_EQUAL_INT(0, feedAll(d, partial, sizeof(partial), 0));
  TEST_ASSERT_FALSE(d.hasPacket());
  TEST_ASSERT_TRUE(d.expireIdle(40));  // 40 ms silence > 30: abandon
  TEST_ASSERT_EQUAL_UINT32(1, d.statistics().timeoutAborts);
  TEST_ASSERT_EQUAL_UINT32(40, d.statistics().maxGapMs);
  TEST_ASSERT_EQUAL_UINT32(0, d.statistics().slowGaps);
}

// ------------------------------------------------------------------ main

int main(void) {
  UNITY_BEGIN();
  // encoder
  RUN_TEST(test_encode_poll_golden_vector);
  RUN_TEST(test_encode_emits_exactly_two_syns);
  RUN_TEST(test_encode_escapes_protocol_chars_in_body);
  RUN_TEST(test_encode_never_escapes_syn_value);
  RUN_TEST(test_encode_jmri_ctype_init_body);
  RUN_TEST(test_encode_max_body_worst_case_fits_staging);
  RUN_TEST(test_encode_rejects_oversized_body);
  RUN_TEST(test_encode_rejects_insufficient_capacity);
  // decoder
  RUN_TEST(test_decode_roundtrip);
  RUN_TEST(test_decode_accepts_frame_without_syns);
  RUN_TEST(test_decode_tolerates_extra_syns);
  RUN_TEST(test_decode_ff_in_body_is_data_no_resync);
  RUN_TEST(test_decode_escaped_etx_in_last_body_position);
  RUN_TEST(test_decode_escaped_stx_in_body_is_data);
  RUN_TEST(test_decode_unescaped_stx_restarts_frame);
  RUN_TEST(test_decode_pre_stx_noise_not_buffered);
  RUN_TEST(test_decode_hunt_is_dle_aware);
  RUN_TEST(test_decode_dangling_dle_timeout_discards);
  RUN_TEST(test_decode_truncated_frame_recovers_via_gap);
  RUN_TEST(test_decode_timeout_disabled_tolerates_gaps);
  RUN_TEST(test_decode_oversized_body_aborts_and_recovers);
  RUN_TEST(test_decode_body_length_boundaries);
  RUN_TEST(test_decode_commits_only_on_etx);
  RUN_TEST(test_decode_etx_before_header_complete_is_abort);
  RUN_TEST(test_decode_back_to_back_frames);
  RUN_TEST(test_decode_ready_slot_overrun_keeps_oldest);
  RUN_TEST(test_decode_high_value_bytes);
  RUN_TEST(test_roundtrip_every_body_value);
  RUN_TEST(test_expire_idle_noop_when_hunting);
  // gap observability (2.2.6 grace-band receive model)
  RUN_TEST(test_decode_gap_observability_off_by_default);
  RUN_TEST(test_decode_gap_nominal_region_records_nothing);
  RUN_TEST(test_decode_gap_grace_region_records_watermark_only);
  RUN_TEST(test_decode_gap_slow_region_increments_slowGaps);
  RUN_TEST(test_decode_gap_fatal_region_aborts_and_stamps_watermark);
  RUN_TEST(test_decode_gap_conformance_observes_without_aborting);
  RUN_TEST(test_decode_gap_disabled_when_lo_zero);
  RUN_TEST(test_decode_gap_watermark_tracks_max_not_last);
  RUN_TEST(test_decode_gap_fatal_silence_stamps_watermark);
  return UNITY_END();
}
