// CMRINode.cpp — the CMRInet polled-strategy Node engine.

#include "CMRINode.h"

#include <string.h>

namespace CMRInet {

CMRINode::CMRINode(CMRITransport& transport,
                     const CMRINodeConfig& config)
    : transport_(transport), config_(config) {
  input_.setLength(config.inputBytes);
  output_.setLength(config.outputBytes);
}

void CMRINode::begin() {
  if (!began_) {
    input_.setLength(config_.inputBytes);
    output_.setLength(config_.outputBytes);
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
    output_.setLength(n);
    memcpy(output_.writable(), rx.body, n);
  }
  if (n > 0) {
    if (unpackHandler_ != nullptr) {
      unpackHandler_(unpackContext_, output_);
    } else if (unpackHandlerNoCtx_ != nullptr) {
      unpackHandlerNoCtx_(output_);
    }
  }
}

void CMRINode::handlePoll_() {
  if (config_.inputBytes > 0) {
    if (packHandler_ != nullptr) {
      packHandler_(packContext_, input_);
    } else if (packHandlerNoCtx_ != nullptr) {
      packHandlerNoCtx_(input_);
    }
  }
  // ensure that the reply body is the smaller of the 
  // handler-provided input and the configured output image
  // max size.  This is a safety check to avoid sending
  // more data than the host expects.
  const size_t n = (input_.length() <= config_.outputBytes)
                      ? input_.length()
                      : config_.outputBytes;
  reply_.clear();
  reply_.wireUA = wireUA();
  reply_.mt = MessageType::kReceiveData;
  reply_.setBody(input_.data(), n);
  (void)transport_.sendPacket(reply_);
  emitTrace_(/*transmit=*/true, reply_);
}

void CMRINode::emitTrace_(bool transmit, const CMRIPacket& packet) {
  if (traceListener_ != nullptr) {
    traceListener_(traceContext_, transmit, packet);
  }
}

}  // namespace CMRInet
