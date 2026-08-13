// CMRIFrameCodec.h — the CMRInet serial codec: packet <-> wire bytes.
//
// Framing exists only to create message boundaries on a byte stream. The
// codec has no Arduino dependencies: time is injected as `nowMs` and
// bytes are plain uint8_t, so desktop tests compile the exact sources.
//
// VALIDATION: Design v1.0 D4: framing belongs to the serial transport
// layer. The engine above deals in packets, never bytes.
// VALIDATION: Interop v1.0 Part 2: wire behavior implements the
// profile's TX (2.1.x) and RX (2.2.x) rules. Bare rule ids in this file
// inherit this tag's version.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CMRIPacket.h"

namespace CMRInet {

/// Render one packet as a complete wire frame in `out`:
///
///   SYN/0xFF SYN/0xFF STX/0x02 UA MT <escaped body> ETX/0x03
///
/// - Exactly two SYN/0xFF preamble bytes (rule 2.1.1).
/// - Every body byte equal to STX/0x02, ETX/0x03, or DLE/0x10 is
///   DLE-escaped, in every message type including I (rule 2.1.2 / E1).
/// - SYN/0xFF body bytes are never escaped (rule 2.1.3).
/// - UA and MT are emitted raw; conforming values never collide with
///   protocol characters.
///
/// The frame is contiguous so the transport can emit it as one gapless
/// buffered write (rule 2.1.5). Size `out` with kMaxWireFrame for the
/// full-escaping worst case (rule 2.1.6).
///
/// Returns the number of bytes written, or 0 if the packet body exceeds
/// kMaxBody or `out`/`capacity` cannot hold the escaped frame.
size_t encodeFrame(const CMRIPacket& packet, uint8_t* out, size_t capacity);

/// Resumable, non-blocking, byte-at-a-time frame decoder.
///
/// Feed it received bytes as they arrive; it assembles frames into a
/// staging packet and commits to a ready slot only on a valid ETX/0x03
/// (rule 2.2.8: never act on a partial body). The state machine follows
/// the cpCMRI architecture with the hygiene the research found missing
/// (comparison.md §5): explicit escape state, DLE processed before the
/// STX/ETX tests, guard-before-store, inter-byte timeout, error counters,
/// and every member initialized.
///
/// Behavior, by profile rule:
/// - Hunts for a bare STX/0x02; SYN/0xFF is neither required nor counted
///   (rule 2.2.1). The hunt itself is DLE-aware, so an escaped 0x02 inside
///   another station's frame never starts a frame (rule 2.4.1).
/// - After a DLE/0x10, the next byte is data with no interpretation,
///   including in the last body position (rule 2.2.2).
/// - 0xFF in a body is data; the decoder never resynchronizes on it
///   (rule 2.2.3 — JMRI C-type I bodies carry six raw 0xFF pad bytes).
/// - An unescaped STX/0x02 mid-frame restarts the frame: the body index
///   resets and nothing received before the STX is kept (rule 2.2.4).
/// - Every store is bounds-checked; a body that exceeds kMaxBody aborts
///   the frame (rule 2.2.5).
/// - A partial frame is abandoned when the inter-byte gap exceeds the
///   configured limit (rule 2.2.6). A frame that ends in a dangling
///   DLE/0x10 or without ETX/0x03 is discarded, never delivered
///   (rule 2.2.7).
/// - Bytes are uint8_t end to end; no signed-char comparisons
///   (rule 2.2.9).
///
/// UA filtering and MT validation are deliberately NOT here.
/// VALIDATION: Design v1.0 "Transport contract (packet seam)": address
/// filtering is not the transport's job.
class CMRIFrameDecoder {
 public:
  /// Decoder health counters, in transport-neutral terms. All start at 0.
  struct Statistics {
    uint32_t framesDecoded = 0;    ///< frames committed on valid ETX/0x03
    uint32_t framesRestarted = 0;  ///< unescaped STX/0x02 mid-frame (2.2.4)
    uint32_t timeoutAborts = 0;    ///< inter-byte gap exceeded (2.2.6)
    uint32_t danglingDle = 0;      ///< frame died awaiting escaped byte (2.2.7)
    uint32_t overflowAborts = 0;   ///< body exceeded kMaxBody (2.2.5)
    uint32_t headerAborts = 0;     ///< ETX/0x03 before UA and MT arrived
    uint32_t droppedPackets = 0;   ///< frame completed while ready slot full
  };

  /// Conservative default inter-byte timeout. The profile suggests two to
  /// three character times (rule 2.2.6) — ~1.5 ms at 19200 BPS — but the
  /// codec does not know the baud rate, so transports should call
  /// setInterByteTimeoutMs() with a rate-appropriate value.
  static constexpr uint32_t kDefaultInterByteTimeoutMs = 20;

  CMRIFrameDecoder() = default;

  /// Set the inter-byte timeout in milliseconds. 0 disables the timeout
  /// entirely: a conformance-grade receiver must tolerate arbitrary gaps,
  /// because the reference Host lineage transmitted with interpreter-scale
  /// gaps between bytes (rule 2.2.6 exception).
  void setInterByteTimeoutMs(uint32_t ms) { interByteTimeoutMs_ = ms; }

  /// Consume one received byte. `nowMs` is the caller's injected clock
  /// (see CMRITime.h) and must be monotonic. Returns true when this
  /// byte completed a frame and a packet is now available via take().
  bool feed(uint8_t byte, uint32_t nowMs);

  /// Abandon a stale partial frame without waiting for the next byte.
  /// Transports call this from tick() so a truncated frame cannot hold
  /// the decoder mid-frame forever. Returns true if a frame was abandoned.
  bool expireIdle(uint32_t nowMs);

  /// True when a completed packet is waiting to be taken.
  bool hasPacket() const { return readyValid_; }

  /// Pop the completed packet into `out`. Returns false if none is ready.
  bool take(CMRIPacket& out);

  /// Decoder health counters (read-only).
  const Statistics& statistics() const { return statistics_; }

  /// Reset parser state (hunt for the next frame) and drop any ready
  /// packet. Counters are preserved; see resetStatistics().
  void reset();

  /// Zero all counters.
  void resetStatistics() { statistics_ = Statistics(); }

 private:
  enum class State : uint8_t {
    kHunt,  ///< between frames: waiting for a bare STX/0x02
    kUa,    ///< STX seen: next data byte is UA
    kMt,    ///< UA seen: next data byte is MT
    kBody,  ///< collecting body bytes until ETX/0x03
  };

  void startFrame_();
  void abandonFrame_();
  bool commitFrame_();
  void handleData_(uint8_t byte);

  State state_ = State::kHunt;
  bool escaped_ = false;     ///< a DLE/0x10 was seen; next byte is literal
  bool readyValid_ = false;  ///< ready_ holds an untaken packet
  uint32_t interByteTimeoutMs_ = kDefaultInterByteTimeoutMs;
  uint32_t lastByteMs_ = 0;
  CMRIPacket staging_;  ///< frame under assembly (commit-on-ETX, 2.2.8)
  CMRIPacket ready_;    ///< last completed frame awaiting take()
  Statistics statistics_;
};

}  // namespace CMRInet
