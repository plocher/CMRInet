// SerialCMRITransport.cpp — serial/RS-485 packet transport. See
// SerialCMRITransport.h for the model.

#include "SerialCMRITransport.h"

#include "CMRITime.h"

namespace CMRInet {

void SerialCMRITransport::begin() {
  port_.begin();
  // Release the driver: a transport must never come up holding the
  // bus (interop 2.3.14 — TXEN is asserted only around a frame).
  port_.setTransmitEnable(false);

  rxQueue_.head = 0;
  rxQueue_.count = 0;
  txLength_ = 0;
  txWritten_ = 0;
  txState_ = TxState::kIdle;
  drainDueMs_ = 0;
  lastTickMs_ = 0;
  decoder_.reset();
  decoder_.resetStatistics();
  stats_ = LinkStatistics();

  byteMicros_ = port_.byteDurationMicros();
  if (byteMicros_ == 0) {
    byteMicros_ = 1;  // defensive: the port contract says nonzero
  }
  hardwareErrorBaseline_ = port_.hardwareErrorCount();
  if (!timeoutOverridden_) {
    interByteTimeoutMs_ = defaultInterByteTimeoutMs_();
  }
  decoder_.setInterByteTimeoutMs(interByteTimeoutMs_);
}

void SerialCMRITransport::tick(uint32_t nowMs) {
  lastTickMs_ = nowMs;
  pumpTransmit_(nowMs);
  pumpReceive_(nowMs);
  // Expire stale partial frames: elapsed line silence is the only
  // evidence of a truncated frame a receiver has (interop 2.2.6).
  decoder_.expireIdle(nowMs);
  syncErrors_();
}

bool SerialCMRITransport::sendPacket(const CMRIPacket& packet) {
  if (txState_ != TxState::kIdle) {
    // Backpressure: the previous frame has not fully left the wire.
    stats_.sendRejects++;
    return false;
  }
  const size_t n = encodeFrame(packet, txFrame_, sizeof(txFrame_));
  if (n == 0) {
    stats_.sendRejects++;
    return false;
  }
  txLength_ = n;
  txWritten_ = 0;
  stats_.packetsSent++;

  // TXEN discipline, step 1: assert before the first byte
  // (interop 2.3.14).
  port_.setTransmitEnable(true);
  txState_ = TxState::kWriting;
  // Whole-frame wire-time floor: transmission starts now, and while
  // the UART is kept busy the frame occupies the wire for exactly
  // frame-length character times.
  drainDueMs_ = lastTickMs_ + wireTimeMs_(n);
  pumpTransmit_(lastTickMs_);
  return true;
}

bool SerialCMRITransport::receivePacket(CMRIPacket& out) {
  if (rxQueue_.count == 0) {
    return false;
  }
  out = rxQueue_.slots[rxQueue_.head];
  rxQueue_.head = (rxQueue_.head + 1) % kRxQueueCapacity;
  rxQueue_.count--;
  stats_.packetsReceived++;
  return true;
}

// ------------------------------------------------------------- internals

void SerialCMRITransport::pumpTransmit_(uint32_t nowMs) {
  if (txState_ == TxState::kWriting) {
    // Step 2: write. One gapless write when the buffer has room
    // (rule 2.1.5); otherwise the remainder trickles out on later
    // ticks while the UART transmits ahead of it.
    const size_t accepted =
        port_.writeBytes(txFrame_ + txWritten_, txLength_ - txWritten_);
    if (accepted != 0) {
      txWritten_ += accepted;
      // If the wire went idle before this chunk (slow ticks, small
      // UART buffer), the whole-frame floor undershoots: raise the
      // estimate so this chunk still gets its full wire time.
      const uint32_t chunkDueMs = nowMs + wireTimeMs_(accepted);
      if (timeReached(chunkDueMs, drainDueMs_)) {
        drainDueMs_ = chunkDueMs;
      }
      if (txWritten_ == txLength_) {
        txState_ = TxState::kDraining;
      }
    }
  }
  if (txState_ == TxState::kDraining) {
    // Step 3 and 4: flush to full drain, then deassert at once. Both
    // detectors must agree: the wire-time estimate covers ports that
    // only know their buffer; the port's own answer covers late
    // chunks still sitting in its FIFO (see header).
    if (timeReached(nowMs, drainDueMs_) && port_.transmitDrained()) {
      port_.setTransmitEnable(false);
      txState_ = TxState::kIdle;
    }
  }
}

void SerialCMRITransport::pumpReceive_(uint32_t nowMs) {
  // Drain everything the port has buffered. Bytes arrive at line rate,
  // far below CPU rate, so this loop is bounded by the port's buffer.
  int byte = port_.readByte();
  while (byte >= 0) {
    // VALIDATION: Interop v1.0 2.2.9: received bytes normalize to
    // unsigned 8-bit values at the read.
    if (decoder_.feed(static_cast<uint8_t>(byte), nowMs)) {
      drainDecoder_();
    }
    byte = port_.readByte();
  }
}

void SerialCMRITransport::drainDecoder_() {
  CMRIPacket decoded;
  while (decoder_.take(decoded)) {
    if (rxQueue_.count == kRxQueueCapacity) {
      // Arrival order holds: keep the oldest, drop the newest.
      stats_.receiveDrops++;
      continue;
    }
    rxQueue_.slots[(rxQueue_.head + rxQueue_.count) % kRxQueueCapacity] =
        decoded;
    rxQueue_.count++;
  }
}

void SerialCMRITransport::syncErrors_() {
  // One medium-neutral error figure: decoder integrity failures plus
  // whatever UART-level errors the hardware exposes.
  const CMRIFrameDecoder::Statistics& d = decoder_.statistics();
  stats_.decodeErrors = d.framesRestarted + d.timeoutAborts + d.danglingDle +
                        d.overflowAborts + d.headerAborts +
                        (port_.hardwareErrorCount() - hardwareErrorBaseline_);
}

uint32_t SerialCMRITransport::wireTimeMs_(size_t bytes) const {
  // Ceiling division: never report a frame drained before its last
  // stop bit. bytes <= kMaxWireFrame and byteMicros_ < ~1200 at CMRInet
  // rates, so the product fits comfortably in 32 bits.
  const uint32_t micros = static_cast<uint32_t>(bytes) * byteMicros_;
  return (micros + 999u) / 1000u;
}

uint32_t SerialCMRITransport::defaultInterByteTimeoutMs_() const {
  // Three character times (interop 2.2.6), rounded up to the injected
  // clock's millisecond granularity, never below 1 ms.
  const uint32_t ms = (3u * byteMicros_ + 999u) / 1000u;
  return (ms == 0) ? 1u : ms;
}

}  // namespace CMRInet
