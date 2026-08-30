// MockCMRITransport.h — the packet-seam test double and scripted-replay
// rig.
//
// The mock has lower fidelity than an emulator: it delivers exactly the
// packets and bytes a test scripts, at exactly the mock-clock times the
// test chooses, so recorded pathological sequences replay byte for byte.
// Time advances only through tick(nowMs). Tests run without a wall
// clock: deterministic, and faster than real time.
//
// VALIDATION: Design v1.1 D3: scripted byte/packet replay is
// counterparty fidelity 1, below the correct and warty CMRINode
// emulators.
//
// Two injection levels:
// - Packet level: injectPacket[At]() puts a whole packet on the receive
//   queue and bypasses the codec. Use it for engine tests that do not
//   exercise framing.
// - Byte level: injectBytes[At]() feeds raw wire bytes through a real
//   CMRIFrameDecoder, optionally metered with an inter-byte gap. Use it
//   to replay truncations, dangling DLEs, garbage, and slow gapped
//   transmitters.
//
// VALIDATION: Interop v1.1 2.2.6: gapped byte injection exercises the
// receiver's inter-byte abandon rule under a deterministic clock.
//
// The replay script plays counterparty to the engine. Script steps fire
// in order when a completed send matches (UA, MT). A step answers with a
// packet, raw bytes, or scripted silence, after a scripted delay.
//
// This class never allocates memory, not even in begin(). All storage is
// fixed-capacity, sized by the CMRINET_MOCK_* knobs below.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CMRIFrameCodec.h"
#include "CMRIPacket.h"
#include "CMRITime.h"
#include "CMRITransport.h"

// ---- Geometry knobs: shrink for small targets. ----
// VALIDATION: Design v1.1 D8: geometry ceilings are compile-time knobs.

// Received packets waiting for receivePacket().
#ifndef CMRINET_MOCK_RX_QUEUE
#define CMRINET_MOCK_RX_QUEUE 8
#endif

// Sent packets waiting for takeSent() observation.
#ifndef CMRINET_MOCK_SENT_LOG
#define CMRINET_MOCK_SENT_LOG 16
#endif

// Pending scheduled deliveries (direct injections + fired script steps).
#ifndef CMRINET_MOCK_EVENTS
#define CMRINET_MOCK_EVENTS 16
#endif

// Script steps.
#ifndef CMRINET_MOCK_SCRIPT_STEPS
#define CMRINET_MOCK_SCRIPT_STEPS 16
#endif

// Raw-byte payload capacity per injection or script step. Default: one
// worst-case fully escaped wire frame (kMaxWireFrame).
#ifndef CMRINET_MOCK_EVENT_BYTES
#define CMRINET_MOCK_EVENT_BYTES (6 + 2 * CMRINET_MAX_BODY)
#endif

namespace CMRInet {

class MockCMRITransport : public CMRITransport {
 public:
  static constexpr size_t kRxQueueCapacity = CMRINET_MOCK_RX_QUEUE;
  static constexpr size_t kSentLogCapacity = CMRINET_MOCK_SENT_LOG;
  static constexpr size_t kEventCapacity = CMRINET_MOCK_EVENTS;
  static constexpr size_t kScriptCapacity = CMRINET_MOCK_SCRIPT_STEPS;
  static constexpr size_t kEventByteCapacity = CMRINET_MOCK_EVENT_BYTES;

  /// Wildcard for script matchers: matches any UA or any MT.
  static constexpr int kMatchAny = -1;

  /// Repeat count meaning "fire on every match, never consume the step".
  static constexpr uint16_t kRepeatForever = 0;

  MockCMRITransport() = default;

  // ------------------------------------------------ CMRITransport seam

  /// Resets all runtime state: queues, script, in-flight send,
  /// statistics, clock, decoder. The link comes up. Allocates nothing.
  void begin() override;

  /// Advances the mock clock. Completes an in-flight send and fires the
  /// script, delivers due events, and expires stale partial frames in
  /// the byte decoder. `nowMs` must be monotonic. Never blocks.
  void tick(uint32_t nowMs) override;

  /// Accepts one packet for "transmission" and records it for
  /// takeSent(). Returns false, and counts a sendReject, when the link
  /// is down, when a previous send has not completed (backpressure), or
  /// when the packet body is oversized. The send completes at the
  /// current mock time plus setSendLatencyMs() (immediately when the
  /// latency is 0). On completion the packet is matched against the
  /// replay script.
  bool sendPacket(const CMRIPacket& packet) override;

  /// True once the last accepted send is fully delivered (or nothing was
  /// ever sent). Gates the strategy's reply timer.
  bool sendComplete() const override { return !sendCompleteAt_.armed(); }

  /// Pop the oldest received packet, in arrival order, at most one per
  /// call. Whole validated packets only: byte-level injections surface
  /// here only after the real decoder committed a frame.
  bool receivePacket(CMRIPacket& out) override;

  const LinkStatistics& stats() const override { return stats_; }

  // ------------------------------------------------ link shaping (test)

  /// Raise or drop the carrier. While down, sendPacket() refuses and
  /// scheduled deliveries still flow (the test controls both sides).
  void setLinkUp(bool up);

  /// Milliseconds from sendPacket() acceptance to sendComplete() truth.
  /// Default 0: sends complete immediately. Models TX drain time.
  void setSendLatencyMs(uint32_t ms) { sendLatencyMs_ = ms; }

  /// Passthrough to the byte decoder's inter-byte timeout (0 disables).
  void setDecoderInterByteTimeoutMs(uint32_t ms) {
    decoder_.setInterByteTimeoutMs(ms);
  }

  // ------------------------------------------------ direct injection (test)

  /// Queue a whole packet for delivery at the current mock time.
  bool injectPacket(const CMRIPacket& packet) {
    return injectPacketAt(packet, lastTickMs_);
  }

  /// Queue a whole packet for delivery once tick(nowMs) reaches `dueMs`.
  /// Returns false if the event queue is full or the packet is oversized.
  bool injectPacketAt(const CMRIPacket& packet, uint32_t dueMs);

  /// Queue raw wire bytes for the decoder at the current mock time,
  /// gapless.
  bool injectBytes(const uint8_t* bytes, size_t len) {
    return injectBytesAt(bytes, len, lastTickMs_, 0);
  }

  /// Queue raw wire bytes: byte i is fed to the decoder with timestamp
  /// dueMs + i * interByteGapMs, once tick() has advanced that far. A gap
  /// larger than the decoder's inter-byte timeout replays a slow or
  /// stalling transmitter. The bytes are copied, so the caller can
  /// discard its buffer after the call. Returns false if the event queue
  /// is full, `len` exceeds kEventByteCapacity, or bytes is null with
  /// nonzero len.
  bool injectBytesAt(const uint8_t* bytes, size_t len, uint32_t dueMs,
                     uint32_t interByteGapMs);

  // ------------------------------------------------ sent-side observation

  /// Packets accepted by sendPacket() and not yet taken.
  size_t sentCount() const { return sentLog_.count; }

  /// Pop the oldest observed sent packet.
  bool takeSent(CMRIPacket& out);

  /// Sent packets that fell off a full observation log (send succeeded;
  /// only the observation was lost — drain with takeSent()).
  uint32_t sentLogDrops() const { return sentLogDrops_; }

  // ------------------------------------------------ scripted replay (test)

  /// Append a step: when the head-of-script step sees a completed send
  /// matching (UA, mt) — kMatchAny wildcards either — reply with `reply`
  /// after `delayMs`. `repeat` is how many matches the step consumes
  /// before retiring (kRepeatForever = every match, never retires).
  /// Returns false if the script is full or the reply is oversized.
  bool onSendReplyPacket(int wireUA, int mt, const CMRIPacket& reply,
                         uint32_t delayMs = 0, uint16_t repeat = 1);

  /// Append a step that replies with raw wire bytes through the decoder,
  /// metered by `interByteGapMs` (see injectBytesAt). Bytes are copied.
  bool onSendReplyBytes(int wireUA, int mt, const uint8_t* bytes, size_t len,
                        uint32_t delayMs = 0, uint16_t repeat = 1,
                        uint32_t interByteGapMs = 0);

  /// Append a step that consumes a matching send and deliberately answers
  /// nothing — a dead or mute Node for `repeat` exchanges.
  bool onSendStaySilent(int wireUA, int mt, uint16_t repeat = 1);

  /// Steps not yet retired (a kRepeatForever step never retires).
  size_t scriptRemaining() const;

  /// Completed sends that did not match the head-of-script step. The
  /// step is not consumed; the mismatch is counted here for the test.
  uint32_t scriptMismatches() const { return scriptMismatches_; }

  /// Byte-decoder health counters (the mock's own receive integrity).
  const CMRIFrameDecoder::Statistics& decoderStatistics() const {
    return decoder_.statistics();
  }

 private:
  enum class ActionKind : uint8_t { kSilence, kPacket, kBytes };

  /// One scheduled delivery: a packet, or a metered run of raw bytes.
  struct Delivery {
    ActionKind kind = ActionKind::kSilence;
    uint32_t dueMs = 0;   ///< delivery time (first byte, for kBytes)
    uint32_t gapMs = 0;   ///< inter-byte gap (kBytes only)
    size_t nextByte = 0;  ///< progress cursor (kBytes only)
    size_t length = 0;    ///< byte count (kBytes only)
    CMRIPacket packet;
    uint8_t bytes[kEventByteCapacity] = {0};
  };

  /// One script step: matcher plus the delivery it schedules.
  struct ReplayStep {
    int matchWireUA = kMatchAny;
    int matchMt = kMatchAny;
    uint16_t repeat = 1;  ///< matches left; kRepeatForever = infinite
    ActionKind kind = ActionKind::kSilence;
    uint32_t delayMs = 0;
    uint32_t gapMs = 0;
    size_t length = 0;
    CMRIPacket packet;
    uint8_t bytes[kEventByteCapacity] = {0};
  };

  bool pushEvent_(const Delivery& event);
  void deliverDueEvents_(uint32_t nowMs);
  void feedDecoder_(uint8_t byte, uint32_t atMs);
  void drainDecoder_();
  void syncDecodeErrors_();
  void completeSend_(uint32_t completedAtMs);
  void matchScript_(const CMRIPacket& sent, uint32_t completedAtMs);
  bool appendStep_(const ReplayStep& step);

  // Receive queue (FIFO ring). Arrival order is preserved by keeping the
  // oldest and dropping the newest on overflow (mirrors the decoder).
  struct {
    CMRIPacket slots[kRxQueueCapacity];
    size_t head = 0;
    size_t count = 0;
  } rxQueue_;

  // Sent-packet observation log (FIFO ring; overflow drops the oldest).
  struct {
    CMRIPacket slots[kSentLogCapacity];
    size_t head = 0;
    size_t count = 0;
  } sentLog_;

  // Scheduled deliveries (FIFO ring). Events are strictly ordered: the
  // head event must finish before the next starts, regardless of due
  // times, so delivery order always equals injection order.
  struct {
    Delivery slots[kEventCapacity];
    size_t head = 0;
    size_t count = 0;
  } events_;

  // Replay script (FIFO; steps are appended, only the head matches).
  struct {
    ReplayStep steps[kScriptCapacity];
    size_t head = 0;
    size_t count = 0;
  } script_;

  CMRIFrameDecoder decoder_;
  LinkStatistics stats_;
  // The mock clock: the time of the most recent tick(). The convenience
  // "now" overloads (injectPacket, injectBytes) schedule against it, so
  // a test should tick() to establish time before it uses them.
  uint32_t lastTickMs_ = 0;
  uint32_t sendLatencyMs_ = 0;
  // In-flight send state. Armed while a send drains, disarmed when idle
  // (explicit state, no zero-time sentinel — see CMRITime.h). This is
  // why sendComplete() is simply !armed().
  Deadline sendCompleteAt_;
  CMRIPacket inFlight_;  ///< the send awaiting completion
  uint32_t sentLogDrops_ = 0;
  uint32_t scriptMismatches_ = 0;
};

}  // namespace CMRInet
