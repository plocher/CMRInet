// CMRINode.cpp — the CMRInet polled-strategy Node engine.

#include "CMRINode.h"

#include <string.h>

namespace CMRInet {

CMRINode::CMRINode(CMRITransport& transport,
                     const CMRINodeConfig& config)
    : transport_(transport), config_(config) {
}

void CMRINode::begin() {
  if (!began_) {
    transport_.begin();
    began_ = true;
  }
}

void CMRINode::tick(uint32_t nowMs) {
  if (!began_) {
    return;
  }
  transport_.tick(nowMs);
  drainReceive_();
}

void CMRINode::drainReceive_() {
  CMRIPacket rx;
  while (transport_.receivePacket(rx)) {
    emitTrace_(/*transmit=*/false, rx);
    if (rx.wireUA != wireUA()) {
      continue;
    }
    dispatch_(rx);
  }
}

void CMRINode::dispatch_(const CMRIPacket& rx) {
  switch (rx.mt) {
    case MessageType::kInit:
      handleInit_(rx);
      break;
    case MessageType::kTransmitData:
      handleTransmit_(rx);
      break;
    case MessageType::kPoll:
      handlePoll_();
      break;
    default:
      break;
  }
}

void CMRINode::handleInit_(const CMRIPacket& rx) {
  if (rx.length < 1 ||
      rx.body[0] != static_cast<uint8_t>(config_.nodeType)) {
    return;
  }
  if (rx.length >= 3) {
    transmissionDelayDh_ = rx.body[1];
    transmissionDelayDl_ = rx.body[2];
  }
}

void CMRINode::handleTransmit_(const CMRIPacket& rx) {
  const size_t n = (rx.length < config_.outputBytes)
                       ? rx.length
                       : config_.outputBytes;
  if (n > 0) {
    memcpy(outputs_, rx.body, n);
  }
  if (n > 0) {
    if (unpackHandler_ != nullptr) {
      unpackHandler_(unpackContext_, outputs_, n);
    } else if (unpackHandlerNoCtx_ != nullptr) {
      unpackHandlerNoCtx_(outputs_, n);
    }
  }
}

void CMRINode::handlePoll_() {
  if (config_.inputBytes > 0) {
    if (packHandler_ != nullptr) {
      packHandler_(packContext_, inputs_, config_.inputBytes);
    } else if (packHandlerNoCtx_ != nullptr) {
      packHandlerNoCtx_(inputs_, config_.inputBytes);
    }
  }
  reply_.clear();
  reply_.wireUA = wireUA();
  reply_.mt = MessageType::kReceiveData;
  reply_.setBody(inputs_, config_.inputBytes);
  (void)transport_.sendPacket(reply_);
  emitTrace_(/*transmit=*/true, reply_);
}

void CMRINode::emitTrace_(bool transmit, const CMRIPacket& packet) {
  if (traceListener_ != nullptr) {
    traceListener_(traceContext_, transmit, packet);
  }
}

// ---- input image (IB) ----

void CMRINode::setInputBit(size_t byte, size_t bit, bool v) {
  if (byte >= config_.inputBytes) {
    return;
  }
  if (v) {
    inputs_[byte] |= static_cast<uint8_t>(1u << (bit % 8u));
  } else {
    inputs_[byte] &= static_cast<uint8_t>(~(1u << (bit % 8u)));
  }
}

void CMRINode::setInputByte(size_t index, uint8_t v) {
  if (index >= config_.inputBytes) {
    return;
  }
  inputs_[index] = v;
}

bool CMRINode::setInputs(const uint8_t* data, size_t len) {
  if (len > config_.inputBytes) {
    return false;
  }
  if (data == nullptr && len != 0) {
    return false;
  }
  if (len != 0) {
    memcpy(inputs_, data, len);
  }
  return true;
}

bool CMRINode::inputBit(size_t byte, size_t bit) const {
  if (byte >= config_.inputBytes) {
    return false;
  }
  return (inputs_[byte] >> (bit % 8u)) & 0x01u;
}

uint8_t CMRINode::inputByte(size_t index) const {
  return (index < config_.inputBytes) ? inputs_[index] : 0u;
}

const uint8_t* CMRINode::inputs() const {
  return inputs_;
}

// ---- output image (OB) ----

bool CMRINode::outputBit(size_t byte, size_t bit) const {
  if (byte >= config_.outputBytes) {
    return false;
  }
  return (outputs_[byte] >> (bit % 8u)) & 0x01u;
}

uint8_t CMRINode::outputByte(size_t index) const {
  return (index < config_.outputBytes) ? outputs_[index] : 0u;
}

const uint8_t* CMRINode::outputs() const {
  return outputs_;
}

}  // namespace CMRInet
