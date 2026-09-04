// CMRINode.h — the CMRInet polled-strategy Node engine.
//
// Receives addressed I, T, and P; emits R in reply to P.
// I and T expect no reply (interop E8). The Node serves
// one UA, so there is no schedule or round-robin.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>  // millis(), for the tick() convenience overload
#endif

#include "CMRIPacket.h"
#include "CMRITransport.h"
#include "IOBuffer.h"

// Geometry knobs: input and output image capacities,
// in data bytes. Shrink for memory-limited targets.
#ifndef CMRINET_NODE_MAX_INPUT_BYTES
#define CMRINET_NODE_MAX_INPUT_BYTES 118
#endif

#ifndef CMRINET_NODE_MAX_OUTPUT_BYTES
#define CMRINET_NODE_MAX_OUTPUT_BYTES 118
#endif

namespace CMRInet {

/// Node configuration.
struct CMRINodeConfig {
  uint8_t ua = 0;                ///< Unit Address (0..127)
  char nodeType = 'C';            ///< NDP: first byte of the I body
  uint16_t inputBytes = 0;        ///< NI: input image size
  uint16_t outputBytes = 0;       ///< NO: output image size
  uint8_t transmissionDelayDh = 0; ///< dH: per-char delay (10 µs units)
  uint8_t transmissionDelayDl = 0; ///< dL: per-char delay (10 µs units)
};

/// Handler called at P time with the input image buffer.
/// The library sends the buffer as R regardless.
using PackHandler = void (*)(void* ctx, IOBuffer& ib);

/// Context-free pack handler — no void* parameter. The peer
/// API for sketches that don't need a context cookie (the
/// common case). The ctx version above is for the testbed and
/// the future sibling library, which bind their own state.
using PackHandlerNoCtx = void (*)(IOBuffer& ib);

/// Handler called at T time with the received output image.
using UnpackHandler = void (*)(void* ctx, IOBuffer& ob);

/// Context-free unpack handler — no void* parameter.
using UnpackHandlerNoCtx = void (*)(IOBuffer& ob);

/// Optional trace listener: fires for every received and
/// sent packet, before UA matching and I/T/P dispatch.
using TraceListener = void (*)(void* ctx, bool transmit,
                                 const CMRIPacket& packet);

class CMRINode {
 public:
  static constexpr size_t kMaxInputBytes = CMRINET_NODE_MAX_INPUT_BYTES;
  static constexpr size_t kMaxOutputBytes = CMRINET_NODE_MAX_OUTPUT_BYTES;

  explicit CMRINode(CMRITransport& transport,
                    const CMRINodeConfig& config = CMRINodeConfig());

  /// Set the node configuration. Must be called before begin();
  /// a no-op after begin() (geometry cannot change at runtime).
  void config(const CMRINodeConfig& cfg) {
    if (!began_) {
      config_ = cfg;
      size_t inputLen = (cfg.inputBytes <= kMaxInputBytes)
                          ? cfg.inputBytes
                          : kMaxInputBytes;
      size_t outputLen = (cfg.outputBytes <= kMaxOutputBytes)
                          ? cfg.outputBytes
                          : kMaxOutputBytes;
      input_.setLength(inputLen);
      output_.setLength(outputLen);
    }
  }

  /// Register the pack handler (with context). Called at P time
  /// with ib and the context cookie. If not registered, the
  /// library sends the image the direct accessors maintain.
  void onPack(PackHandler handler, void* context = nullptr) {
    packHandler_ = handler;
    packContext_ = context;
    packHandlerNoCtx_ = nullptr;
  }

  /// Register the pack handler (context-free). The peer API
  /// for sketches that don't need a context cookie.
  void onPack(PackHandlerNoCtx handler) {
    packHandlerNoCtx_ = handler;
    packHandler_ = nullptr;
  }

  /// Register the unpack handler (with context). Called at T
  /// time with ob and the context cookie. If not registered,
  /// the library stores ob and the direct accessors can read it.
  void onUnpack(UnpackHandler handler, void* context = nullptr) {
    unpackHandler_ = handler;
    unpackContext_ = context;
    unpackHandlerNoCtx_ = nullptr;
  }

  /// Register the unpack handler (context-free). The peer API
  /// for sketches that don't need a context cookie.
  void onUnpack(UnpackHandlerNoCtx handler) {
    unpackHandlerNoCtx_ = handler;
    unpackHandler_ = nullptr;
  }

  /// Register the optional trace listener.
  void onTrace(TraceListener listener, void* context = nullptr) {
    traceListener_ = listener;
    traceContext_ = context;
  }

  /// Move from configuration to running. Idempotent.
  void begin();

  /// Advance the engine to nowMs. Pumps the transport,
  /// receives packets, matches UA, dispatches I/T/P.
  void tick(uint32_t nowMs);

#ifdef ARDUINO
  /// Convenience for sketches: tick with the Arduino clock.
  void tick() { tick(millis()); }
#endif

  // ---- input image (IB) ----

  /// Set one input bit. Bit 0 is the LSB of the named byte.
  /// Out-of-range indexes are ignored.
  //  The input image is not guaranteed to have storage 
  //  for all bits a user might want to set:
  //    setInputBit(10, 0, true) when only inputBytes = 2
  //    are configured will see no effect
  void setInputBit(size_t byte, size_t bit, bool v) {
    input_.setBit(byte, bit, v);
  }

  /// Set one input byte. Out-of-range indexes are ignored.
  void setInputByte(size_t index, uint8_t v) {
    input_.setByte(index, v);
  }

  /// Copy len bytes into the input image. Returns false
  /// if len > inputBytes or data is null with nonzero len.
  bool setInputs(const uint8_t* data, size_t len) {
    return input_.setData(data, len);
  }

  /// Read one input bit. Bit 0 is the LSB of the named byte.
  /// Out-of-range indexes read false.
  bool inputBit(size_t byte, size_t bit) const {
    return input_.getBit(byte, bit);
  }

  /// Read one input byte. Out-of-range indexes read 0.
  uint8_t inputByte(size_t index) const {
    return input_.byte(index);
  }

  /// The input image and its configured length.
  const uint8_t* inputs() const { return input_.data(); }
  size_t inputLength() const { return config_.inputBytes; }

  // ---- output image (OB) ----

  /// Read one output bit. Bit 0 is the LSB of the named byte.
  /// Out-of-range indexes read false.
  bool outputBit(size_t byte, size_t bit) const {
    return output_.getBit(byte, bit);
  }

  /// Read one output byte. Out-of-range indexes read 0.
  uint8_t outputByte(size_t index) const {
    return output_.byte(index);
  }

  /// The last received output image and its length.
  const uint8_t* outputs() const { return output_.data(); }
  size_t outputLength() const { return config_.outputBytes; }

  // ---- identity ----

  /// The node's UA (0..127) as configured.
  uint8_t UA() const { return config_.ua; }

  /// The node's wire UA byte as transmitted (UA + 65).
  uint8_t wireUA() const {
    return static_cast<uint8_t>(config_.ua + kWireUAOffset);
  }

  /// True once begin() has been called.
  bool begun() const { return began_; }

 private:
  CMRITransport& transport_;
  CMRINodeConfig config_;

  PackHandler packHandler_ = nullptr;
  void* packContext_ = nullptr;
  PackHandlerNoCtx packHandlerNoCtx_ = nullptr;
  UnpackHandler unpackHandler_ = nullptr;
  void* unpackContext_ = nullptr;
  UnpackHandlerNoCtx unpackHandlerNoCtx_ = nullptr;
  TraceListener traceListener_ = nullptr;
  void* traceContext_ = nullptr;

  IOBuffer input_;
  IOBuffer output_;

  CMRIPacket reply_;
  uint8_t transmissionDelayDh_ = 0;
  uint8_t transmissionDelayDl_ = 0;

  bool began_ = false;

  void drainReceive_();
  void dispatch_(const CMRIPacket& rx);
  void handleInit_(const CMRIPacket& rx);
  void handleTransmit_(const CMRIPacket& rx);
  void handlePoll_();
  void emitTrace_(bool transmit, const CMRIPacket& packet);
};

}  // namespace CMRInet
