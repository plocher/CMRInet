// CMRIPacket.h — the direction-neutral CMRInet packet value type.
//
// A packet is the protocol's logical datagram {UA, MT, body}. It is NOT
// bytes on a wire: serial framing (SYN/0xFF preamble, STX/0x02, DLE/0x10
// escaping, ETX/0x03) is the serial codec's business (CMRIFrameCodec.h).

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time knobs,
// so a '328-class build can shrink packet buffers.
// VALIDATION: Interop v1.1 E7: the default is 256 logical body bytes,
// counted after DLE removal.
#ifndef CMRINET_MAX_BODY
#define CMRINET_MAX_BODY 256
#endif

namespace CMRInet {

// Protocol characters. This library always names a protocol character
// together with its hex value.
constexpr uint8_t kSyn = 0xFFu;  // SYN/0xFF — TX preamble only, never escaped
constexpr uint8_t kStx = 0x02u;  // STX/0x02 — frame start
constexpr uint8_t kEtx = 0x03u;  // ETX/0x03 — frame end
constexpr uint8_t kDle = 0x10u;  // DLE/0x10 — escape prefix

// VALIDATION: Interop v1.1 "Terms": wire UA = UA + 65.
// A node's Unit Address (UA) is an ordinal in the range 0..127. The
// wire UA is the byte transmitted on the serial line: UA + 65.
constexpr uint8_t kWireUAOffset = 65u;

/// True when `wireUA` is a legal on-the-wire unit-address byte: in the
/// range [kWireUAOffset, kWireUAOffset + 127] = [65, 192]. A byte
/// outside this range is not carrying a UA at all — no conforming
/// station can emit it. Pure and stateless, shared by the Host's
/// packet-verify gate, the tracer, and the tests.
inline bool isLegalWireUA(uint8_t wireUA) {
  return wireUA >= kWireUAOffset &&
         wireUA <= static_cast<uint8_t>(kWireUAOffset + 127u);
}

// Message types the polled strategy speaks.
// VALIDATION: Interop v1.1 E9: the codec never validates MT — fielded
// networks carry JMRI extensions (E/Q/D/W/A/C/M) and the codec must not
// choke on them.
namespace MessageType {
constexpr uint8_t kInit = 'I';          // session setup
constexpr uint8_t kPoll = 'P';          // media-access control
constexpr uint8_t kReceiveData = 'R';   // Node -> Host inputs
constexpr uint8_t kTransmitData = 'T';  // Host -> Node outputs
}  // namespace MessageType

// Logical body ceiling, counted after DLE removal (E7).
constexpr size_t kMaxBody = CMRINET_MAX_BODY;

// VALIDATION: Interop v1.1 2.1.6: size the TX staging buffer for full
// escaping — the worst case is 2 SYN + STX + UA + MT + ETX plus two
// wire bytes per data byte.
constexpr size_t kMaxWireFrame = 6u + 2u * kMaxBody;

/// The direction-neutral packet value type: {UA, MT, body}.
///
/// UA and MT are stored exactly as they appear on the wire (UA already
/// includes the +65 offset). The body holds logical data bytes — no SYN,
/// no framing, no DLE stuffing.
struct CMRIPacket {
  uint8_t wireUA = 0;   ///< wire unit-UA byte as transmitted (UA + 65)
  uint8_t mt = 0;       ///< message type byte ('I','P','T','R', extensions)
  uint16_t length = 0;  ///< logical body bytes in use, <= kMaxBody
  uint8_t body[kMaxBody] = {0};

  /// Reset to an empty packet (UA 0, MT 0, zero-length body).
  void clear() {
    wireUA = 0;
    mt = 0;
    length = 0;
  }

  /// Copy `len` data bytes into the body, validating length at intake.
  /// Returns false (and leaves the packet unchanged) if len > kMaxBody
  /// or data is null with a nonzero length.
  bool setBody(const uint8_t* data, size_t len) {
    if (len > kMaxBody) {
      return false;
    }
    if (data == nullptr && len != 0) {
      return false;
    }
    if (len != 0) {
      memcpy(body, data, len);
    }
    length = static_cast<uint16_t>(len);
    return true;
  }
};

/// Set one bit in a raw image buffer. Bit 0 is the LSB of the
/// named byte. For use inside pack/unpack callbacks where the
/// engine hands you a uint8_t* directly. Peer to CMRINode::setInputBit
/// and RemoteNodeHandle::setOutputBit, which do the same thing on the
/// engine-owned buffers.
inline void setBit(uint8_t* image, size_t byte, size_t bit, bool v) {
  if (v) {
    image[byte] |= static_cast<uint8_t>(1u << (bit % 8u));
  } else {
    image[byte] &= static_cast<uint8_t>(~(1u << (bit % 8u)));
  }
}

/// Read one bit from a raw image buffer. Bit 0 is the LSB of the
/// named byte. For use inside pack/unpack callbacks where the
/// engine hands you a const uint8_t* directly.
inline bool getBit(const uint8_t* image, size_t byte, size_t bit) {
  return (image[byte] >> (bit % 8u)) & 0x01u;
}

}  // namespace CMRInet
