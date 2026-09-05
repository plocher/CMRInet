#include "capture.h"

void TracerCapture::record(bool transmit, const CMRInet::CMRIPacket& packet,
                            uint32_t nowMs) {
  if (!active_) return;
  if (packet.mt == 'I' || packet.mt == 'T') {
    itFrames_++;
  }
  if (ringUsed_ < kCapacity) {
    RingRecord& r = ring_[ringUsed_++];
    const bool legalUa = CMRInet::isLegalWireUA(packet.wireUA);
    r.t_ms = nowMs;
    r.UA = legalUa ? CMRInet::toSemanticUA(packet.wireUA) : packet.wireUA;
    r.mt = packet.mt;
    r.flags = transmit ? kFlagTx : 0u;
    if (!legalUa) {
      r.flags |= kFlagInvalidUa;
      invalidUaRecords_++;
    }
    r.len = packet.length;
  }
}

void TracerCapture::start(uint32_t nowMs, uint32_t secs,
                          uint32_t pollsSentNow) {
  ringUsed_ = 0;
  pollsAtStart_ = pollsSentNow;
  loopIterations_ = 0;
  itFrames_ = 0;
  invalidUaRecords_ = 0;
  startMs_ = nowMs;
  endMs_ = startMs_ + secs * 1000;
  active_ = true;
  Serial.print("BEGIN CAPTURE t=");
  Serial.println(startMs_);
}

bool TracerCapture::tick(uint32_t nowMs, uint32_t pollsSentNow) {
  if (!active_) return false;
  loopIterations_++;
  if (nowMs < endMs_) return false;

  active_ = false;
  const uint32_t totalPolls = pollsSentNow - pollsAtStart_;
  Serial.print("END CAPTURE t="); Serial.print(nowMs);
  Serial.print(" polls="); Serial.print(totalPolls);
  Serial.print(" its="); Serial.print(itFrames_);
  Serial.print(" loops="); Serial.print(loopIterations_);
  Serial.print(" invalid_ua="); Serial.print(invalidUaRecords_);
  Serial.print(" ring_used="); Serial.print(ringUsed_);
  Serial.print("/"); Serial.println(kCapacity);
  return true;
}

void TracerCapture::dump() const {
  Serial.print("BEGIN DUMP records=");
  Serial.println(ringUsed_);
  for (size_t i = 0; i < ringUsed_; ++i) {
    const RingRecord& r = ring_[i];
    Serial.print("PKT t="); Serial.print(r.t_ms);
    Serial.print((r.flags & kFlagTx) != 0u ? " TX " : " RX ");
    Serial.print("UA="); Serial.print(r.UA);
    if ((r.flags & kFlagInvalidUa) != 0u) {
      Serial.print(" wireUA_invalid=1");
    }
    Serial.print(" mt="); Serial.print(static_cast<char>(r.mt));
    Serial.print(" len="); Serial.print(r.len);
    Serial.print(" n="); Serial.println(i);
  }
  Serial.println("END DUMP");
}

void TracerCapture::reset() {
  active_ = false;
  endMs_ = 0;
  ringUsed_ = 0;
  pollsAtStart_ = 0;
  loopIterations_ = 0;
  itFrames_ = 0;
  invalidUaRecords_ = 0;
}
