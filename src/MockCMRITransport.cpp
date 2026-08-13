// MockCMRITransport.cpp — packet-seam test double and scripted-replay
// rig. See MockCMRITransport.h for the model.

#include "MockCMRITransport.h"

#include <string.h>

namespace CMRInet {

void MockCMRITransport::begin() {
  rxQueue_.head = 0;
  rxQueue_.count = 0;
  sentLog_.head = 0;
  sentLog_.count = 0;
  events_.head = 0;
  events_.count = 0;
  script_.head = 0;
  script_.count = 0;
  decoder_.reset();
  decoder_.resetStatistics();
  stats_ = LinkStatistics();
  lastTickMs_ = 0;
  sendCompleteAt_.disarm();
  sentLogDrops_ = 0;
  scriptMismatches_ = 0;
}

void MockCMRITransport::tick(uint32_t nowMs) {
  lastTickMs_ = nowMs;

  // Complete an in-flight send. The completed packet drives the script
  // at its completion time, not at the tick time, so a coarse tick
  // schedules the same reply times a fine tick would.
  if (sendCompleteAt_.due(nowMs)) {
    const uint32_t completedAtMs = sendCompleteAt_.dueMs();
    sendCompleteAt_.disarm();
    matchScript_(inFlight_, completedAtMs);
  }

  deliverDueEvents_(nowMs);

  // Expire stale partial frames unconditionally, exactly as a real
  // transport must: elapsed time is the only evidence of line silence a
  // receiver has. A paced injection that must survive expiry needs a
  // rate-appropriate setDecoderInterByteTimeoutMs(), the same remedy a
  // real slow transmitter needs.
  decoder_.expireIdle(nowMs);
  syncDecodeErrors_();
}

bool MockCMRITransport::sendPacket(const CMRIPacket& packet) {
  if (!stats_.linkUp || sendCompleteAt_.armed() || packet.length > kMaxBody) {
    stats_.sendRejects++;
    return false;
  }
  stats_.packetsSent++;

  // Observation log: overflow drops the oldest, so recent traffic is
  // kept for the test to inspect.
  if (sentLog_.count == kSentLogCapacity) {
    sentLog_.head = (sentLog_.head + 1) % kSentLogCapacity;
    sentLog_.count--;
    sentLogDrops_++;
  }
  sentLog_.slots[(sentLog_.head + sentLog_.count) % kSentLogCapacity] = packet;
  sentLog_.count++;

  if (sendLatencyMs_ == 0) {
    matchScript_(packet, lastTickMs_);
    return true;
  }
  inFlight_ = packet;
  sendCompleteAt_.armIn(lastTickMs_, sendLatencyMs_);
  return true;
}

bool MockCMRITransport::receivePacket(CMRIPacket& out) {
  if (rxQueue_.count == 0) {
    return false;
  }
  out = rxQueue_.slots[rxQueue_.head];
  rxQueue_.head = (rxQueue_.head + 1) % kRxQueueCapacity;
  rxQueue_.count--;
  stats_.packetsReceived++;
  return true;
}

void MockCMRITransport::setLinkUp(bool up) { stats_.linkUp = up; }

bool MockCMRITransport::injectPacketAt(const CMRIPacket& packet,
                                       uint32_t dueMs) {
  if (packet.length > kMaxBody) {
    return false;
  }
  Delivery event;
  event.kind = ActionKind::kPacket;
  event.dueMs = dueMs;
  event.packet = packet;
  return pushEvent_(event);
}

bool MockCMRITransport::injectBytesAt(const uint8_t* bytes, size_t len,
                                      uint32_t dueMs,
                                      uint32_t interByteGapMs) {
  if (len > kEventByteCapacity || (bytes == nullptr && len != 0)) {
    return false;
  }
  Delivery event;
  event.kind = ActionKind::kBytes;
  event.dueMs = dueMs;
  event.gapMs = interByteGapMs;
  event.length = len;
  if (len != 0) {
    memcpy(event.bytes, bytes, len);
  }
  return pushEvent_(event);
}

bool MockCMRITransport::takeSent(CMRIPacket& out) {
  if (sentLog_.count == 0) {
    return false;
  }
  out = sentLog_.slots[sentLog_.head];
  sentLog_.head = (sentLog_.head + 1) % kSentLogCapacity;
  sentLog_.count--;
  return true;
}

bool MockCMRITransport::onSendReplyPacket(int ua, int mt,
                                          const CMRIPacket& reply,
                                          uint32_t delayMs, uint16_t repeat) {
  if (reply.length > kMaxBody) {
    return false;
  }
  ReplayStep step;
  step.matchUa = ua;
  step.matchMt = mt;
  step.repeat = repeat;
  step.kind = ActionKind::kPacket;
  step.delayMs = delayMs;
  step.packet = reply;
  return appendStep_(step);
}

bool MockCMRITransport::onSendReplyBytes(int ua, int mt, const uint8_t* bytes,
                                         size_t len, uint32_t delayMs,
                                         uint16_t repeat,
                                         uint32_t interByteGapMs) {
  if (len > kEventByteCapacity || (bytes == nullptr && len != 0)) {
    return false;
  }
  ReplayStep step;
  step.matchUa = ua;
  step.matchMt = mt;
  step.repeat = repeat;
  step.kind = ActionKind::kBytes;
  step.delayMs = delayMs;
  step.gapMs = interByteGapMs;
  step.length = len;
  if (len != 0) {
    memcpy(step.bytes, bytes, len);
  }
  return appendStep_(step);
}

bool MockCMRITransport::onSendStaySilent(int ua, int mt, uint16_t repeat) {
  ReplayStep step;
  step.matchUa = ua;
  step.matchMt = mt;
  step.repeat = repeat;
  step.kind = ActionKind::kSilence;
  return appendStep_(step);
}

size_t MockCMRITransport::scriptRemaining() const { return script_.count; }

// ------------------------------------------------------------- internals

bool MockCMRITransport::pushEvent_(const Delivery& event) {
  if (events_.count == kEventCapacity) {
    return false;
  }
  events_.slots[(events_.head + events_.count) % kEventCapacity] = event;
  events_.count++;
  return true;
}

void MockCMRITransport::deliverDueEvents_(uint32_t nowMs) {
  // Strict order: the head event must finish before the next starts,
  // regardless of due times, so delivery order always equals injection
  // order. Replay stays deterministic under any tick granularity.
  while (events_.count != 0) {
    Delivery& event = events_.slots[events_.head];
    if (event.nextByte == 0 && !timeReached(nowMs, event.dueMs)) {
      return;  // head not yet due; later events wait behind it
    }
    if (event.kind == ActionKind::kPacket) {
      if (rxQueue_.count == kRxQueueCapacity) {
        // Arrival order holds: keep the oldest, drop the newest.
        stats_.receiveDrops++;
      } else {
        rxQueue_.slots[(rxQueue_.head + rxQueue_.count) % kRxQueueCapacity] =
            event.packet;
        rxQueue_.count++;
      }
    } else if (event.kind == ActionKind::kBytes) {
      // Byte i carries the timestamp dueMs + i * gapMs. The decoder sees
      // the same inter-byte gaps a real line would carry, under any tick
      // granularity.
      while (event.nextByte < event.length) {
        const uint32_t byteMs =
            event.dueMs + static_cast<uint32_t>(event.nextByte) * event.gapMs;
        if (!timeReached(nowMs, byteMs)) {
          return;  // next byte still in the future; event stays at head
        }
        feedDecoder_(event.bytes[event.nextByte], byteMs);
        event.nextByte++;
      }
    }
    // kSilence events and finished events retire here.
    events_.head = (events_.head + 1) % kEventCapacity;
    events_.count--;
  }
}

void MockCMRITransport::feedDecoder_(uint8_t byte, uint32_t atMs) {
  if (decoder_.feed(byte, atMs)) {
    drainDecoder_();
  }
}

void MockCMRITransport::drainDecoder_() {
  CMRIPacket decoded;
  while (decoder_.take(decoded)) {
    if (rxQueue_.count == kRxQueueCapacity) {
      stats_.receiveDrops++;
      continue;
    }
    rxQueue_.slots[(rxQueue_.head + rxQueue_.count) % kRxQueueCapacity] =
        decoded;
    rxQueue_.count++;
  }
}

void MockCMRITransport::syncDecodeErrors_() {
  const CMRIFrameDecoder::Statistics& d = decoder_.statistics();
  stats_.decodeErrors = d.framesRestarted + d.timeoutAborts + d.danglingDle +
                        d.overflowAborts + d.headerAborts;
}

void MockCMRITransport::matchScript_(const CMRIPacket& sent,
                                     uint32_t completedAtMs) {
  if (script_.count == 0) {
    return;  // no expectations: silence, uncounted
  }
  ReplayStep& step = script_.steps[script_.head];
  const bool uaMatches = (step.matchUa == kMatchAny) ||
                         (static_cast<int>(sent.ua) == step.matchUa);
  const bool mtMatches = (step.matchMt == kMatchAny) ||
                         (static_cast<int>(sent.mt) == step.matchMt);
  if (!uaMatches || !mtMatches) {
    // The head step is not consumed. The test reads the count.
    scriptMismatches_++;
    return;
  }

  if (step.kind != ActionKind::kSilence) {
    Delivery event;
    event.kind = step.kind;
    event.dueMs = completedAtMs + step.delayMs;
    event.gapMs = step.gapMs;
    event.length = step.length;
    event.packet = step.packet;
    if (step.kind == ActionKind::kBytes && step.length != 0) {
      memcpy(event.bytes, step.bytes, step.length);
    }
    // A full event queue swallows the reply. The match is still
    // consumed: the test overfilled its own rig.
    pushEvent_(event);
  }

  if (step.repeat != kRepeatForever) {
    step.repeat--;
    if (step.repeat == 0) {
      script_.head = (script_.head + 1) % kScriptCapacity;
      script_.count--;
    }
  }
}

bool MockCMRITransport::appendStep_(const ReplayStep& step) {
  if (script_.count == kScriptCapacity) {
    return false;
  }
  script_.steps[(script_.head + script_.count) % kScriptCapacity] = step;
  script_.count++;
  return true;
}

}  // namespace CMRInet
