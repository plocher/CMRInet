// SerialCMRITransport.cpp — serial/RS-485 packet transport. See
// SerialCMRITransport.h for the model.

#include "serial.h"

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
  pendingIdle_ = false;
  echoDiscardRemaining_ = 0;
  echoCancelArmed_ = false;
  rxDuringTx_ = 0;
  lastTickMs_ = 0;
  decoder_.reset();
  decoder_.resetStatistics();
  stats_ = LinkStatistics();

  byteMicros_ = port_.byteDurationMicros();
  if (byteMicros_ == 0) {
    byteMicros_ = 1;  // defensive: the port contract says nonzero
  }
  hardwareErrorBaseline_ = port_.hardwareErrorCount();
  // Echo cancellation: default Auto (ADR-0003). Survives a
  // setEchoCancelMode() override like the inter-byte timeout.
  if (!echoCancelOverridden_) {
    echoCancelMode_ = EchoCancelMode::kAuto;
  }
  if (!timeoutOverridden_) {
    interByteTimeoutMs_ = kShippedInterByteTimeoutMs;
  }
  if (!slowGapOverridden_) {
    slowGapLoMs_ = defaultSlowGapLoMs_();
    slowGapHiMs_ = defaultSlowGapHiMs_();
  }
  decoder_.setSlowGapThresholdsMs(slowGapLoMs_, slowGapHiMs_);
}

void SerialCMRITransport::tick(uint32_t nowMs) {
  lastTickMs_ = nowMs;
  // Tick consistency (ADR-0003 / #112): pumpTransmit_ may detect
  // drain-complete mid-tick, drop TXEN, and latch pendingIdle_
  // without flipping txState_ yet. pumpReceive_ then finishes any
  // remaining own-frame echo budget and feeds post-deassert RX
  // (a prompt R arrives only after TXEN falls — E10). The flip
  // commits after pumpReceive_ — one truth, no mid-tick race.
  pendingIdle_ = false;
  pumpTransmit_(nowMs);
  pumpReceive_(nowMs);
  if (pendingIdle_) {
    txState_ = TxState::kIdle;
  }
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
  // Own-frame echo budget: discard at most one wire-frame of RX while
  // transmitting (2-wire self-echo under misconfigured !RE). A Node
  // cannot begin R until it has received ETX, so a legitimate reply
  // arrives only after TXEN deasserts (interop 2.3.15 / E10) — never
  // treat post-deassert RX as echo (issue #112).
  echoDiscardRemaining_ = n;
  stats_.packetsSent++;

  // TXEN discipline, step 1: assert before the first byte
  // (interop 2.3.14).
  port_.setTransmitEnable(true);
  txState_ = TxState::kWriting;
  // Whole-frame wire-time floor: transmission starts now, and while
  // the UART is kept busy the frame occupies the wire for exactly
  // frame-length character times.
  drainDueMs_ = lastTickMs_ + wireTimeMs_(n) + oneCharTimeMs_();
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
    // Step 3 and 4: flush to full drain, then deassert at once.
    //
    // Hardware-truth ports (Esp32SerialPort): transmitDrained() is the
    // shift register. Trust it alone — the wire-time estimate must not
    // veto silicon (issue #112). That is why Esp32SerialPort exists.
    //
    // Estimate-only ports: require estimate elapsed AND transmitDrained()
    // (buffer empty may be optimistic by ignorance; estimate covers the
    // shift register the port cannot see).
    //
    // txState_ flip is deferred via pendingIdle_ so pumpReceive_ can
    // finish any remaining own-frame echo budget this tick.
    const bool drained = port_.transmitDrained();
    const bool estimateOk = timeReached(nowMs, drainDueMs_);
    const bool done =
        drained && (port_.hardwareTransmitDrain() || estimateOk);
    if (done) {
      port_.setTransmitEnable(false);
      pendingIdle_ = true;  // commit the flip after pumpReceive_
    }
  }
}

void SerialCMRITransport::pumpReceive_(uint32_t nowMs) {
  // Echo cancellation (issue #104, ADR-0003; issue #112):
  // on a 2-wire bus with !RE tied active, the Host hears its own
  // TX echo. Discard only that echo — at most one wire-frame of
  // RX per send (echoDiscardRemaining_).
  //
  // Wire physics / interop 2.3.15 + E10: a Node cannot begin R until it
  // has received the poll's ETX. The reply therefore arrives at the Host
  // after TXEN deasserts, not while ETX drains. Whole-window discard that
  // kept treating the drain-complete tick as "still transmitting" could
  // throw away a prompt post-deassert R (dense full-T empty gates, #112).
  //
  // Modes (ADR-0003):
  // - AlwaysOn: length-limited discard of own-frame echo.
  // - Auto: arm permanently on first RX-during-TX; then AlwaysOn shape.
  // - AlwaysOff: never discard; feed the decoder.
  //
  // Discard window = TXEN up, OR the drain-complete tick (pendingIdle_)
  // while own-frame budget remains. After the budget is done (or TXEN is
  // down with no residual budget), feed RX to the decoder.
  const bool txenUp =
      (txState_ != TxState::kIdle) && !pendingIdle_;
  const bool discardActive =
      (echoCancelMode_ == EchoCancelMode::kAlwaysOn) ||
      (echoCancelMode_ == EchoCancelMode::kAuto && echoCancelArmed_);
  const bool inEchoWindow =
      txenUp || (pendingIdle_ && echoDiscardRemaining_ > 0);

  if (inEchoWindow || txenUp) {
    int byte = port_.readByte();
    while (byte >= 0) {
      if (discardActive && echoDiscardRemaining_ > 0) {
        // Own-frame echo: throw away, do not feed the decoder.
        --echoDiscardRemaining_;
      } else if (discardActive) {
        // Budget exhausted (or post-deassert surplus): not own-frame echo.
        // Feed the decoder — #112 vs discarding the whole TXEN-shaped window.
        if (decoder_.feed(static_cast<uint8_t>(byte), nowMs)) {
          drainDecoder_();
        }
      } else if (txenUp) {
        // Discard not active (AlwaysOff, or Auto not yet armed) while
        // TXEN is up: feed the decoder so the defect is observable, and
        // count each byte as the rxDuringTx defect signal. In Auto, the
        // first observation arms the discard for later ticks.
        ++rxDuringTx_;
        if (echoCancelMode_ == EchoCancelMode::kAuto && !echoCancelArmed_) {
          echoCancelArmed_ = true;  // arm permanently
        }
        // VALIDATION: Interop v1.1 2.2.9: received bytes normalize
        // to unsigned 8-bit values at the read.
        if (decoder_.feed(static_cast<uint8_t>(byte), nowMs)) {
          drainDecoder_();
        }
      } else {
        // pendingIdle with budget already 0 and discard off: feed.
        if (decoder_.feed(static_cast<uint8_t>(byte), nowMs)) {
          drainDecoder_();
        }
      }
      byte = port_.readByte();
    }
    if (pendingIdle_) {
      echoDiscardRemaining_ = 0;
    }
    return;
  }
  // Fully idle, no residual budget: drain everything into the decoder.
  // Bytes arrive at line rate, far below CPU rate, so this loop is
  // bounded by the port's buffer.
  echoDiscardRemaining_ = 0;
  int byte = port_.readByte();
  while (byte >= 0) {
    // VALIDATION: Interop v1.1 2.2.9: received bytes normalize to
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

uint32_t SerialCMRITransport::threeCharTimesMs_(uint32_t byteMicros) const {
  // Three character times (interop 2.2.6), rounded up to the injected
  // clock's millisecond granularity, never below 1 ms.
  const uint32_t ms = (3u * byteMicros + 999u) / 1000u;
  return (ms == 0) ? 1u : ms;
}

uint32_t SerialCMRITransport::oneCharTimeMs_() const {
  // One character time (interop 2.2.6), rounded up to ms, min 1.
  // The echo-cancel guard band in sendPacket adds this to the
  // whole-frame wire-time estimate so TXEN stays asserted
  // one char time past shift-register-empty, giving pumpReceive_
  // a window to discard the echo's trailing ETX before the
  // drain gate closes.
  return threeCharTimesMs_(byteMicros_) / 3u;
}

uint32_t SerialCMRITransport::rateDerivedInterByteTimeoutMs() const {
  // Reads the port's character time directly so the value is valid
  // before begin() as well as after (Design v1.1 D13: this is the
  // conformance instrument, not the shipped default).
  return threeCharTimesMs_(port_.byteDurationMicros());
}

uint32_t SerialCMRITransport::defaultSlowGapLoMs_() const {
  // One character time: the streaming floor. Below this, bytes arrive
  // contiguously and no gap is recorded. Rounded up to ms, min 1.
  const uint32_t ms = (byteMicros_ + 999u) / 1000u;
  return (ms == 0) ? 1u : ms;
}

uint32_t SerialCMRITransport::defaultSlowGapHiMs_() const {
  // Three character times: the suspicion floor. Gaps at/above this and
  // below the abort limit increment slowGaps. At ms granularity this
  // often coincides with the rate-derived abort value, so the slow band
  // is widest when the abort limit is raised (USB chunking, deployed
  // tolerance); lower hi to open the band on a clean UART.
  return threeCharTimesMs_(byteMicros_);
}

}  // namespace CMRInet
