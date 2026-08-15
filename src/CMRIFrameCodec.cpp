// CMRIFrameCodec.cpp — CMRInet serial codec implementation.
//
// VALIDATION: Interop v1.1 Part 2: wire behavior implements the
// profile's TX (2.1.x) and RX (2.2.x) rules. Bare rule ids in this file
// inherit this tag's version.

#include "CMRIFrameCodec.h"

namespace CMRInet {

namespace {

/// True for the three data values that must be DLE-escaped in every
/// message body: STX/0x02, ETX/0x03, DLE/0x10 (rule 2.1.2 / E1).
/// SYN/0xFF is deliberately absent (rule 2.1.3).
inline bool needsEscape(uint8_t b) {
  return b == kStx || b == kEtx || b == kDle;
}

}  // namespace

size_t encodeFrame(const CMRIPacket& packet, uint8_t* out, size_t capacity) {
  if (out == nullptr) {
    return 0;
  }
  if (packet.length > kMaxBody) {
    return 0;
  }

  // Measure first so a too-small buffer produces no partial frame.
  size_t required = 6;  // 2 SYN + STX + UA + MT + ETX
  for (uint16_t i = 0; i < packet.length; ++i) {
    required += needsEscape(packet.body[i]) ? 2 : 1;
  }
  if (capacity < required) {
    return 0;
  }

  size_t n = 0;
  out[n++] = kSyn;  // exactly two SYN/0xFF (rule 2.1.1)
  out[n++] = kSyn;
  out[n++] = kStx;
  out[n++] = packet.ua;
  out[n++] = packet.mt;
  for (uint16_t i = 0; i < packet.length; ++i) {
    const uint8_t b = packet.body[i];
    if (needsEscape(b)) {
      out[n++] = kDle;
    }
    out[n++] = b;
  }
  out[n++] = kEtx;
  return n;
}

bool CMRIFrameDecoder::feed(uint8_t byte, uint32_t nowMs) {
  // A stale partial frame dies before the new byte is considered
  // (rule 2.2.6). unsigned subtraction is wrap-safe. The same mid-frame
  // gap is observed through the 2.2.6 grace-band table before the abort
  // decision: a fatal gap stamps the maxGapMs watermark, then abandons.
  const bool midFrame = (state_ != State::kHunt || escaped_);
  if (midFrame) {
    const uint32_t gap = nowMs - lastByteMs_;
    observeGap_(gap);
    if (interByteTimeoutMs_ != 0 && gap > interByteTimeoutMs_) {
      abandonFrame_();
    }
  }
  lastByteMs_ = nowMs;

  // DLE processing comes before the STX and ETX tests (rule 2.2.2), and
  // stays active while hunting/discarding (rule 2.4.1): an escaped byte
  // is data everywhere, with no interpretation.
  if (escaped_) {
    escaped_ = false;
    handleData_(byte);
    return false;
  }
  if (byte == kDle) {
    escaped_ = true;
    return false;
  }
  if (byte == kStx) {
    // Frame start. Mid-frame this is a restart: reset the body index and
    // keep nothing received before the STX (rule 2.2.4).
    if (state_ != State::kHunt) {
      statistics_.framesRestarted++;
    }
    startFrame_();
    return false;
  }
  if (byte == kEtx) {
    if (state_ == State::kBody) {
      return commitFrame_();
    }
    if (state_ == State::kUa || state_ == State::kMt) {
      // Frame ended before UA and MT arrived: malformed, discard.
      statistics_.headerAborts++;
      state_ = State::kHunt;
    }
    // Stray ETX/0x03 while hunting is ignored.
    return false;
  }
  handleData_(byte);
  return false;
}

bool CMRIFrameDecoder::expireIdle(uint32_t nowMs) {
  if (interByteTimeoutMs_ == 0) {
    return false;
  }
  if (state_ == State::kHunt && !escaped_) {
    return false;
  }
  if ((nowMs - lastByteMs_) <= interByteTimeoutMs_) {
    return false;
  }
  observeGap_(nowMs - lastByteMs_);
  const bool wasFrame = (state_ != State::kHunt);
  abandonFrame_();
  return wasFrame;
}

bool CMRIFrameDecoder::take(CMRIPacket& out) {
  if (!readyValid_) {
    return false;
  }
  out = ready_;
  readyValid_ = false;
  return true;
}

void CMRIFrameDecoder::reset() {
  state_ = State::kHunt;
  escaped_ = false;
  readyValid_ = false;
  staging_.clear();
}

void CMRIFrameDecoder::startFrame_() {
  staging_.clear();
  state_ = State::kUa;
  escaped_ = false;
}

void CMRIFrameDecoder::abandonFrame_() {
  // A frame that ends in a dangling DLE/0x10, or without ETX/0x03, is an
  // error and is discarded — never delivered (rule 2.2.7).
  if (state_ != State::kHunt) {
    statistics_.timeoutAborts++;
    if (escaped_) {
      statistics_.danglingDle++;
    }
  }
  state_ = State::kHunt;
  escaped_ = false;
}

bool CMRIFrameDecoder::commitFrame_() {
  // Commit to the ready slot only on a valid ETX/0x03 (rule 2.2.8).
  state_ = State::kHunt;
  if (readyValid_) {
    // Caller has not taken the previous packet; keep arrival order by
    // retaining the oldest and counting the loss.
    statistics_.droppedPackets++;
    return false;
  }
  ready_ = staging_;
  readyValid_ = true;
  statistics_.framesDecoded++;
  return true;
}

void CMRIFrameDecoder::handleData_(uint8_t byte) {
  switch (state_) {
    case State::kHunt:
      // Pre-STX bytes are never stored (rule 2.2.4).
      break;
    case State::kUa:
      staging_.ua = byte;
      state_ = State::kMt;
      break;
    case State::kMt:
      staging_.mt = byte;
      state_ = State::kBody;
      break;
    case State::kBody:
      // Guard before every store (rule 2.2.5).
      if (staging_.length >= kMaxBody) {
        statistics_.overflowAborts++;
        state_ = State::kHunt;
        break;
      }
      staging_.body[staging_.length++] = byte;
      break;
  }
}

void CMRIFrameDecoder::observeGap_(uint32_t gapMs) {
  // 2.2.6 grace-band observability. The codec does not diagnose root
  // cause; it emits raw gap signal for a systemic viewer to correlate
  // with UA and event timestamps on the telemetry line. Off entirely
  // when the observation floor is 0 (the codec default — it does not
  // know the baud rate, so transports set rate-derived thresholds).
  if (slowGapLoMs_ == 0) {
    return;  // observability off
  }
  if (gapMs < slowGapLoMs_) {
    return;  // nominal region: record nothing (a tuned system stays zero)
  }
  // grace / slow / fatal: stamp the cumulative max-gap watermark. A
  // fatal gap stamps it too — the abort-gap itself is the diagnostically
  // critical measurement (it would have located the stage-2 2 s stall
  // from telemetry alone).
  if (gapMs > statistics_.maxGapMs) {
    statistics_.maxGapMs = gapMs;
  }
  // slowGaps counts the suspect band [hi, abort) only. A fatal gap has
  // its own counter (timeoutAborts, bumped by abandonFrame_), so it is
  // not double-counted here. hi <= lo disables slowGaps (watermark-only).
  const bool fatal =
      (interByteTimeoutMs_ != 0 && gapMs > interByteTimeoutMs_);
  if (!fatal && slowGapHiMs_ > slowGapLoMs_ && gapMs >= slowGapHiMs_) {
    statistics_.slowGaps++;
  }
}

}  // namespace CMRInet
