// tcp.cpp — TCP packet transport implementation. See tcp.h for the model.

#include "tcp.h"

namespace CMRInet {

void TcpCMRITransport::begin() {
  port_.begin();

  rxQueue_.head = 0;
  rxQueue_.count = 0;
  txLength_ = 0;
  txWritten_ = 0;
  txInProgress_ = false;

  decoder_.reset();
  decoder_.resetStatistics();
  stats_ = LinkStatistics();
  stats_.linkUp = port_.connected();

  socketErrorBaseline_ = port_.socketErrorCount();

  if (!timeoutOverridden_) {
    interByteTimeoutMs_ = kDefaultInterByteTimeoutMs;
  }
  decoder_.setInterByteTimeoutMs(interByteTimeoutMs_);
}

void TcpCMRITransport::tick(uint32_t nowMs) {
  port_.poll(nowMs);

  const bool wasUp = stats_.linkUp;
  const bool isUp = port_.connected();
  stats_.linkUp = isUp;

  if (wasUp && !isUp) {
    // Carrier dropped: abort in-flight transmission and reset decoder.
    txInProgress_ = false;
    txLength_ = 0;
    txWritten_ = 0;
    decoder_.reset();
  }

  if (isUp) {
    pumpTransmit_();
    pumpReceive_(nowMs);
  }

  // Expire idle partial frame if an inter-byte timeout is configured.
  decoder_.expireIdle(nowMs);
  syncErrors_();
}

bool TcpCMRITransport::sendPacket(const CMRIPacket& packet) {
  if (!port_.connected()) {
    stats_.sendRejects++;
    return false;
  }
  if (txInProgress_) {
    // Backpressure: previous frame has not fully written to the socket.
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
  txInProgress_ = true;
  stats_.packetsSent++;

  pumpTransmit_();
  return true;
}

bool TcpCMRITransport::sendComplete() const {
  return !txInProgress_ && port_.transmitDrained();
}

bool TcpCMRITransport::receivePacket(CMRIPacket& out) {
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

void TcpCMRITransport::pumpTransmit_() {
  if (!txInProgress_) {
    return;
  }
  if (!port_.connected()) {
    txInProgress_ = false;
    txLength_ = 0;
    txWritten_ = 0;
    return;
  }
  const size_t accepted =
      port_.writeBytes(txFrame_ + txWritten_, txLength_ - txWritten_);
  if (accepted > 0) {
    txWritten_ += accepted;
  }
  if (txWritten_ >= txLength_) {
    txInProgress_ = false;
  }
}

void TcpCMRITransport::pumpReceive_(uint32_t nowMs) {
  int byte = port_.readByte();
  while (byte >= 0) {
    if (decoder_.feed(static_cast<uint8_t>(byte), nowMs)) {
      drainDecoder_();
    }
    byte = port_.readByte();
  }
}

void TcpCMRITransport::drainDecoder_() {
  while (decoder_.hasPacket()) {
    CMRIPacket pkt;
    if (!decoder_.take(pkt)) {
      break;
    }
    if (rxQueue_.count >= kRxQueueCapacity) {
      stats_.receiveDrops++;
    } else {
      const size_t tail = (rxQueue_.head + rxQueue_.count) % kRxQueueCapacity;
      rxQueue_.slots[tail] = pkt;
      rxQueue_.count++;
    }
  }
}

void TcpCMRITransport::syncErrors_() {
  const auto& ds = decoder_.statistics();
  const uint32_t decoderFails = ds.framesRestarted + ds.timeoutAborts +
                                ds.danglingDle + ds.overflowAborts +
                                ds.headerAborts;
  const uint32_t sockErrors =
      port_.socketErrorCount() - socketErrorBaseline_;
  stats_.decodeErrors = decoderFails + sockErrors;
}

}  // namespace CMRInet
