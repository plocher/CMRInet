// CMRIPacket.h — the direction-neutral CMRInet packet value type.
//
// A packet is the protocol's logical datagram {UA, MT, body}. It is NOT
// bytes on a wire: serial framing (SYN/0xFF preamble, STX/0x02, DLE/0x10
// escaping, ETX/0x03) is the serial codec's business (CMRIFrameCodec.h).
//
// References:
//   docs/DESIGN.md — Terms, D4, D7, D8.
//   docs/cmrinet-interop-profile-and-errata.md — Part 2 (esp. E7 / 2.2.5).

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Geometry ceiling (DESIGN.md D8): compile-time knob so a '328-class build
// can shrink packet buffers. The interop default is 256 logical body bytes,
// counted after DLE removal (profile E7).
#ifndef CMRINET_MAX_BODY
#define CMRINET_MAX_BODY 256
#endif

namespace CMRInet {

// Protocol characters. This library always names a protocol character
// together with its hex value (profile "Terms").
constexpr uint8_t kSyn = 0xFFu;  // SYN/0xFF — TX preamble only, never escaped
constexpr uint8_t kStx = 0x02u;  // STX/0x02 — frame start
constexpr uint8_t kEtx = 0x03u;  // ETX/0x03 — frame end
constexpr uint8_t kDle = 0x10u;  // DLE/0x10 — escape prefix

// UA = Node address + 65 (profile "Terms").
constexpr uint8_t kUaOffset = 65u;

// Message types the polled strategy speaks. The codec itself never
// validates MT: fielded networks carry JMRI extensions (E/Q/D/W/A/C/M,
// profile E9) and the codec must not choke on them.
namespace MessageType {
constexpr uint8_t kInit = 'I';          // session setup
constexpr uint8_t kPoll = 'P';          // media-access control
constexpr uint8_t kReceiveData = 'R';   // Node -> Host inputs
constexpr uint8_t kTransmitData = 'T';  // Host -> Node outputs
}  // namespace MessageType

// Logical body ceiling, counted after DLE removal (profile E7).
constexpr size_t kMaxBody = CMRINET_MAX_BODY;

// Worst-case wire length of one frame: 2 SYN + STX + UA + MT + ETX plus a
// fully escaped body at two bytes per data byte (profile 2.1.6: size the
// TX staging buffer for full escaping).
constexpr size_t kMaxWireFrame = 6u + 2u * kMaxBody;

/// The direction-neutral packet value type: {UA, MT, body}.
///
/// UA and MT are stored exactly as they appear on the wire (UA already
/// includes the +65 offset). The body holds logical data bytes — no SYN,
/// no framing, no DLE stuffing.
struct CMRIPacket {
  uint8_t ua = 0;       ///< unit address byte as transmitted (address + 65)
  uint8_t mt = 0;       ///< message type byte ('I','P','T','R', extensions)
  uint16_t length = 0;  ///< logical body bytes in use, <= kMaxBody
  uint8_t body[kMaxBody] = {0};

  /// Reset to an empty packet (UA 0, MT 0, zero-length body).
  void clear() {
    ua = 0;
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

}  // namespace CMRInet
