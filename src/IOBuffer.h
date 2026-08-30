// IOBuffer.h — the bounds-safe I/O image container.
//
// The natural type for a CMRInet input or output image. Both engines
// (CMRIHost, CMRINode) and the handle (RemoteNodeHandle) own IOBuffer
// members; the pack/unpack callbacks receive IOBuffer& and use its
// methods to move data between the wire and the hardware.
//
// The initial vocabulary is the raw-image primitives: setBit / bit /
// setByte / byte / data / setData / writable / length / clear. Richer
// accessors (setBCD, setEncoder, setAnalog, ...) live in the sketch
// where the hardware knowledge is — IOBuffer is the safe container,
// nothing more.
//
// Every accessor bounds-checks against the configured geometry
// (length()). Out-of-range writes are silently ignored; out-of-range
// reads return false / 0. This is the safety property that raw
// uint8_t* + size_t len cannot provide.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef CMRINET_IO_BUFFER_MAX_BYTES
#define CMRINET_IO_BUFFER_MAX_BYTES 118
#endif

namespace CMRInet {

class IOBuffer {
 public:
  static constexpr size_t kMaxBytes = CMRINET_IO_BUFFER_MAX_BYTES;

  IOBuffer() = default;

  // ---- bit access --------------------------------------------------------

  /// Set one bit. Bit 0 is the LSB of the named byte.
  /// Out-of-range indexes are ignored.
  void setBit(size_t byte, size_t bit, bool v) {
    if (byte >= length_) {
      return;
    }
    if (v) {
      data_[byte] |= static_cast<uint8_t>(1u << (bit % 8u));
    } else {
      data_[byte] &= static_cast<uint8_t>(~(1u << (bit % 8u)));
    }
  }

  /// Read one bit. Bit 0 is the LSB of the named byte.
  /// Out-of-range indexes read false.
  bool getBit(size_t byte, size_t bit) const {
    if (byte >= length_) {
      return false;
    }
    return (data_[byte] >> (bit % 8u)) & 0x01u;
  }

  // ---- byte access -------------------------------------------------------

  /// Set one byte. Out-of-range indexes are ignored.
  void setByte(size_t index, uint8_t v) {
    if (index < length_) {
      data_[index] = v;
    }
  }

  /// Read one byte. Out-of-range indexes read 0.
  uint8_t byte(size_t index) const {
    return (index < length_) ? data_[index] : 0u;
  }

  // ---- bulk access -------------------------------------------------------

  /// Copy len bytes in. Returns false if len > length() or data is
  /// null with nonzero len.
  bool setData(const uint8_t* src, size_t len) {
    if (len > length_) {
      return false;
    }
    if (src == nullptr && len != 0) {
      return false;
    }
    if (len != 0) {
      memcpy(data_, src, len);
    }
    return true;
  }

  /// Read-only raw pointer to the image data. The buffer owns the
  /// storage; the caller must not write through this pointer.
  /// Use writable() for engine-internal writes (e.g. storing a T body).
  const uint8_t* data() const { return data_; }

  /// Writable raw pointer for engine-internal bulk stores. The
  /// caller is trusted to respect length(); this is the one escape
  /// hatch that bypasses per-element bounds checking (used by
  /// CMRINode::handleTransmit_ to memcpy a received T body).
  uint8_t* writable() { return data_; }

  // ---- geometry ----------------------------------------------------------

  /// The configured image length (NI or NO).
  size_t length() const { return length_; }

  /// Set the geometry. Called by the engine from config() / begin().
  void setLength(size_t len) {
    length_ = (len <= kMaxBytes) ? len : kMaxBytes;
  }

  /// Zero the image data (preserves geometry). Used on
  /// re-init invalidation and geometry change.
  void zero() {
    memset(data_, 0, length_);
  }

  /// Zero the image and reset geometry. Used on slot reuse (D5).
  void clear() {
    memset(data_, 0, kMaxBytes);
    length_ = 0;
  }

 private:
  uint8_t data_[kMaxBytes] = {0};
  size_t length_ = 0;
};

}  // namespace CMRInet
