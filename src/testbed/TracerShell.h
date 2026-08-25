// TracerShell.h — the shared testbed command-and-control shell:
// plain verbs in, JSON-lines telemetry out, wrapped around CMRIHost
// and its D7 listener seam.
//
// This header IS the stage-2 invariant "same shell, same listeners,
// different main()" (map issue #21): the desktop tracer
// (extras/desktop/cmri_tracer.cpp) and the Xiao Host R&D sketch
// (examples/XiaoHostTracer) both wrap this shell, so a scenario that
// passes against one Host image passes against the other by
// construction. Only the mains differ: option parsing, the clock
// source, and the C&C byte stream.
//
// This is a Shell, not an Engine (see CONTEXT.md): it drives CMRIHost
// and renders its observability seam, but it does not implement the
// image contract. The name carries no CMRI qualifier — it speaks a
// private C&C vocabulary, not the CMRInet protocol (D1).
//
// Testbed-only: nothing in the product library includes this header,
// and it costs nothing in a sketch that never includes it (the D7
// linker-drops-unreferenced spirit). Testbed types live in
// CMRInet::testbed so the product namespace stays unambiguous and the
// D1 naming-grammar guarantees keep applying to the product surface.
//
// Stream shape per docs/testbed-software-notes.md: cumulative counters
// only, a monotonic seq on every line, and an explicit epoch marker.
// `ts` is integer milliseconds since this stream's epoch line, in
// every image alike, so scenario diffs never special-case the clock;
// the epoch line itself carries the absolute anchor where one exists
// (wall clock on the desktop, boot milliseconds on the Xiao).

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CMRIHost.h"
#include "RemoteNodeHandle.h"
#include "SerialCMRITransport.h"

namespace CMRInet {
namespace testbed {

/// Telemetry spelling of a node health state.
inline const char* stateName(RemoteNodeState state) {
  switch (state) {
    case RemoteNodeState::kUninitialized: return "UNINITIALIZED";
    case RemoteNodeState::kOnline: return "ONLINE";
    case RemoteNodeState::kStale: return "STALE";
    case RemoteNodeState::kOffline: return "OFFLINE";
    case RemoteNodeState::kMisconfigured: return "MISCONFIGURED";
    case RemoteNodeState::kDegraded: return "DEGRADED";
  }
  return "UNKNOWN";
}

/// Telemetry spelling of a host engine event.
inline const char* eventName(CMRIHostEventType type) {
  switch (type) {
    case CMRIHostEventType::kReplyAccepted: return "reply";
    case CMRIHostEventType::kReplyRejected: return "reject";
    case CMRIHostEventType::kReplyTimeout: return "miss";
    case CMRIHostEventType::kReinitScheduled: return "reinit";
    case CMRIHostEventType::kPollBackoffChanged: return "backoff";
    case CMRIHostEventType::kNodeStateChanged: return "state";
  }
  return "unknown";
}

/// The command-and-control shell one tracer main() wraps: it owns
/// the telemetry line format, the seq counter, the quiesced flag, and
/// the verb vocabulary (quiesce | resume | status | setbit <n> <0|1>
/// | writeoutputs <hex> | forcetx | quit). It registers both D7
/// listeners — onEvent for exchange/health events and onTrace for
/// per-packet TX/RX visibility (I, T, P, R with MT and body) — so the
/// bench sees the full I/T exchange, not just its counters.
///
/// Lifecycle mirrors the library's two-phase rule: bind() during the
/// configuration phase (it registers the D7 listeners, which
/// host.begin() locks), then setNow()/handleVerb()/emitLine() at
/// runtime. Nothing here allocates or blocks.
class TracerShell {
 public:
  /// Writes one completed telemetry line (no trailing newline) to the
  /// C&C stream. The writer appends the line terminator and flushes
  /// as its stream requires.
  using LineWriter = void (*)(void* context, const char* line);

  /// Callback to append additional JSON fields when emitLineAt_ fires.
  using StatusExtender = void (*)(void* context, char* buffer, size_t remaining_capacity);

  /// What handleVerb() asks of the surrounding main loop.
  enum class VerbResult : uint8_t {
    kEmpty,    ///< blank input; nothing happened
    kHandled,  ///< verb consumed (including the unknown-verb error line)
    kQuit,     ///< end the loop; the main emits "final" on its way out
  };

  /// Wire the shell to a configured-but-not-begun host. Registers
  /// both D7 listeners (onEvent and onTrace), so it MUST run before
  /// host.begin() locks the configuration. `image` and `version`
  /// identify the wrapping main on identity lines (`epoch` and
  /// `status`); both strings must outlive the shell.
  void bind(CMRIHost& host, SerialCMRITransport& transport,
            RemoteNodeHandle& node, const char* image, const char* version,
            LineWriter writeLine, void* writeContext) {
    host_ = &host;
    transport_ = &transport;
    node_ = &node;
    image_ = image;
    version_ = version;
    writeLine_ = writeLine;
    writeContext_ = writeContext;
    host.onEvent(&TracerShell::onHostEvent_, this);
    host.onTrace(&TracerShell::onHostTrace_, this);
  }

  /// Register an optional callback that can append additional JSON fields (e.g. ",\"generators\":{...}") 
  /// to the end of any line emitted.
  void setStatusExtender(StatusExtender extender, void* context = nullptr) {
    statusExtender_ = extender;
    statusExtenderContext_ = context;
  }

  /// Refresh the shell's clock. Call once per loop iteration, with
  /// the same monotonic value handed to host.tick(), before verbs are
  /// dispatched.
  void setNow(uint32_t nowMs) { nowMs_ = nowMs; }

  /// Manually emit a trace record into the telemetry stream (e.g. for replaying
  /// a captured ring buffer). Defers to the same formatter used by the listener.
  void emitPacket(bool transmit, const CMRIPacket& packet) {
    emitTrace_(transmit, packet);
  }

  /// Emit the epoch marker: ts restarts at 0 here, so a runner seeing
  /// this line knows every cumulative counter restarted with it. The
  /// anchor pair carries the image's absolute time reference (e.g.
  /// "wallClock" on the desktop, "bootMs" on the Xiao).
  void emitEpoch(const char* anchorKey, const char* anchorValue) {
    epochMs_ = nowMs_;
    emitLineAt_(nowMs_, "epoch", anchorKey, anchorValue, true);
  }

  /// Emit one telemetry line at the shell's current clock. `event`
  /// names why the line exists; extraKey/extraValue append one
  /// event-specific string field.
  void emitLine(const char* event, const char* extraKey = nullptr,
                const char* extraValue = nullptr) {
    emitLineAt_(nowMs_, event, extraKey, extraValue);
  }

  /// Dispatch one C&C verb. Unknown verbs emit an error line and
  /// count as handled; "quit" is not acted on here — the main owns
  /// its own exit (and emits "final").
  VerbResult handleVerb(const char* verb) {
    if (verb == nullptr || verb[0] == '\0') {
      return VerbResult::kEmpty;
    }
    if (strcmp(verb, "quit") == 0) {
      return VerbResult::kQuit;
    }
    if (strcmp(verb, "status") == 0) {
      emitLine("status");
      return VerbResult::kHandled;
    }
    if (strcmp(verb, "quiesce") == 0) {
      node_->setEnabled(false);
      quiesced_ = true;
      emitLine("quiesce");
      return VerbResult::kHandled;
    }
    if (strcmp(verb, "resume") == 0) {
      node_->setEnabled(true);
      quiesced_ = false;
      emitLine("resume");
      return VerbResult::kHandled;
    }
    if (strcmp(verb, "forcetx") == 0) {
      node_->forceTransmit();
      emitLine("forcetx");
      return VerbResult::kHandled;
    }
    if (strncmp(verb, "setbit ", 7) == 0) {
      return handleSetbit_(verb + 7);
    }
    if (strncmp(verb, "writeoutputs ", 13) == 0) {
      return handleWriteoutputs_(verb + 13);
    }
    emitLine("error", "unknownVerb", verb);
    return VerbResult::kHandled;
  }

  /// True while polling is suspended by the quiesce verb.
  bool quiesced() const { return quiesced_; }

  /// When enabled, emit only backoff-change host events; suppress all
  /// other event lines. Useful for preserving diagnostic trace density.
  void setBackoffTraceOnly(bool enabled) { backoffTraceOnly_ = enabled; }

 private:
  // Inputs hex + outputs hex (2 chars per byte each) plus ~400 chars of
  // fixed fields and counters and one extra field. Truncation is
  // guarded, not expected.
  static constexpr size_t kLineCapacity =
      2 * CMRINET_HOST_MAX_INPUT_BYTES +
      2 * CMRINET_HOST_MAX_OUTPUT_BYTES + 640;

  /// CMRIHost event listener: one telemetry line per engine event,
  /// stamped with the event's own tick time.
  static void onHostEvent_(void* context, const CMRIHostEvent& event) {
    TracerShell& self = *static_cast<TracerShell*>(context);
    if (event.type == CMRIHostEventType::kPollBackoffChanged) {
      self.emitBackoffTrace_(event);
      return;
    }
    if (self.backoffTraceOnly_) {
      return;
    }
    if (event.type == CMRIHostEventType::kNodeStateChanged) {
      self.emitLineAt_(event.nowMs, eventName(event.type), "previousState",
                       stateName(event.previousState));
      return;
    }
    self.emitLineAt_(event.nowMs, eventName(event.type));
  }

  /// CMRIHost trace listener: one telemetry line per packet the host
  /// hands to the transport (transmit == true: I, T, P) or the transport
  /// hands up (transmit == false: R, and any unsolicited frame). Fires
  /// inside host.tick() at the shell's current clock, so I/T visibility
  /// lands in the stream beside the counters.
  static void onHostTrace_(void* context, bool transmit,
                           const CMRIPacket& packet) {
    TracerShell& self = *static_cast<TracerShell*>(context);
    self.emitTrace_(transmit, packet);
  }

  void emitTrace_(bool transmit, const CMRIPacket& packet) {
    char bodyHex[2 * kMaxBody + 1] = "";
    const size_t len = packet.length;
    for (size_t i = 0; i < len; ++i) {
      snprintf(&bodyHex[2 * i], 3, "%02X", packet.body[i]);
    }
    char mtBuf[2] = {static_cast<char>(packet.mt), '\0'};
    int written = snprintf(
        line_, sizeof(line_),
        "{\"seq\":%u,\"ts\":%u,\"event\":\"trace\",\"role\":\"host\","
        "\"address\":%u,\"ua\":%u,\"dir\":\"%s\",\"mt\":\"%s\",\"body\":\"%s\"",
        ++seq_, nowMs_ - epochMs_, node_->address(),
        node_->ua(), transmit ? "tx" : "rx", mtBuf, bodyHex);
    if (written < 0 || written >= static_cast<int>(sizeof(line_))) {
      written = static_cast<int>(sizeof(line_)) - 1;
    }
    snprintf(line_ + written, sizeof(line_) - written, "}");
    writeLine_(writeContext_, line_);
  }

  void emitBackoffTrace_(const CMRIHostEvent& event) {
    if (event.node == nullptr) {
      return;
    }
    const char* deadlineAction = "none";
    if (event.pollBackoffReason == PollBackoffChangeReason::kAccept ||
        event.pollBackoffReason == PollBackoffChangeReason::kGeometryMismatch) {
      deadlineAction = "disarm";
    } else if (event.pollBackoffReason == PollBackoffChangeReason::kInitial ||
               event.pollBackoffReason == PollBackoffChangeReason::kMiss) {
      deadlineAction = "arm";
    }
    int written = snprintf(
        line_, sizeof(line_),
        "{\"seq\":%u,\"ts\":%u,\"event\":\"diag_backoff_trace\",\"role\":\"host\","
        "\"address\":%u,\"ua\":%u,"
        "\"old_backoff_ms\":%lu,\"new_backoff_ms\":%lu,\"reason\":\"%s\","
        "\"deadline_action\":\"%s\",\"now_ms\":%lu}",
        ++seq_, event.nowMs - epochMs_, event.node->address(),
        event.node->ua(),
        static_cast<unsigned long>(event.previousPollBackoffMs),
        static_cast<unsigned long>(event.newPollBackoffMs),
        pollBackoffChangeReasonString(event.pollBackoffReason),
        deadlineAction,
        static_cast<unsigned long>(event.nowMs));
    if (written < 0 || written >= static_cast<int>(sizeof(line_))) {
      line_[sizeof(line_) - 1] = '\0';
    }
    writeLine_(writeContext_, line_);
  }

  void emitLineAt_(uint32_t nowMs, const char* event,
                   const char* extraKey = nullptr,
                   const char* extraValue = nullptr,
                   bool emitConfig = false) {
    const bool includeIdentity =
        emitConfig || (strcmp(event, "status") == 0);
    char inputsHex[2 * RemoteNodeHandle::kMaxInputBytes + 1] = "";
    const size_t inputLength = node_->inputLength();
    for (size_t i = 0; i < inputLength; ++i) {
      snprintf(&inputsHex[2 * i], 3, "%02X", node_->inputByte(i));
    }
    char outputsHex[2 * RemoteNodeHandle::kMaxOutputBytes + 1] = "";
    const size_t outputLength = node_->outputLength();
    for (size_t i = 0; i < outputLength; ++i) {
      snprintf(&outputsHex[2 * i], 3, "%02X", node_->outputByte(i));
    }

    const CMRIHostStatistics& host = host_->statistics();
    const RemoteNodeStatistics& node = node_->statistics();
    const LinkStatistics& link = transport_->stats();
    const CMRIFrameDecoder::Statistics& decoder =
        transport_->decoderStatistics();

    int written = 0;
    if (includeIdentity) {
      written = snprintf(
          line_, sizeof(line_),
          "{\"seq\":%u,\"ts\":%u,\"event\":\"%s\","
          "\"role\":\"host\",\"image\":\"%s\",\"version\":\"%s\","
          "\"address\":%u,\"ua\":%u,\"state\":\"%s\",\"quiesced\":%s,"
          "\"polls\":%u,\"pollRetries\":%u,\"replies\":%u,"
          "\"misses\":%u,\"rejected\":%u,\"unsolicited\":%u,"
          "\"exchanges\":%u,\"errors\":%u,\"recoveries\":%u,"
          "\"consecutiveMisses\":%u,\"lastTurnaroundMs\":%u,"
          "\"decodeErrors\":%u,\"slowGaps\":%u,\"maxGapMs\":%u,"
          "\"inputs\":\"%s\",\"outputs\":\"%s\"",
          ++seq_, nowMs - epochMs_, event, image_, version_, node_->address(),
          node_->ua(), stateName(node_->state()),
          quiesced_ ? "true" : "false", host.pollsSent, host.pollSendRetries,
          host.repliesAccepted, node.noReplies, host.repliesRejected,
          host.unsolicitedPackets, node.exchanges, node.errors,
          node.recoveries, node_->consecutiveMisses(), node.lastTurnaroundMs,
          link.decodeErrors, decoder.slowGaps, decoder.maxGapMs, inputsHex,
          outputsHex);
    } else {
      written = snprintf(
          line_, sizeof(line_),
          "{\"seq\":%u,\"ts\":%u,\"event\":\"%s\","
          "\"role\":\"host\","
          "\"address\":%u,\"ua\":%u,\"state\":\"%s\",\"quiesced\":%s,"
          "\"polls\":%u,\"pollRetries\":%u,\"replies\":%u,"
          "\"misses\":%u,\"rejected\":%u,\"unsolicited\":%u,"
          "\"exchanges\":%u,\"errors\":%u,\"recoveries\":%u,"
          "\"consecutiveMisses\":%u,\"lastTurnaroundMs\":%u,"
          "\"decodeErrors\":%u,\"slowGaps\":%u,\"maxGapMs\":%u,"
          "\"inputs\":\"%s\",\"outputs\":\"%s\"",
          ++seq_, nowMs - epochMs_, event, node_->address(), node_->ua(),
          stateName(node_->state()), quiesced_ ? "true" : "false",
          host.pollsSent, host.pollSendRetries, host.repliesAccepted,
          node.noReplies, host.repliesRejected, host.unsolicitedPackets,
          node.exchanges, node.errors, node.recoveries, node_->consecutiveMisses(),
          node.lastTurnaroundMs, link.decodeErrors, decoder.slowGaps,
          decoder.maxGapMs, inputsHex, outputsHex);
    }
    if (written < 0 || written >= static_cast<int>(sizeof(line_))) {
      written = static_cast<int>(sizeof(line_)) - 1;
    }
    if (extraKey != nullptr && extraValue != nullptr) {
      const int extra =
          snprintf(line_ + written, sizeof(line_) - written,
                   ",\"%s\":\"%s\"", extraKey, extraValue);
      if (extra > 0) {
        written += extra;
        if (written >= static_cast<int>(sizeof(line_))) {
          written = static_cast<int>(sizeof(line_)) - 1;
        }
      }
    }
    if (emitConfig) {
      // The epoch line carries the gap-observability band as config (not
      // signal): the three thresholds every counter below was collected
      // against. A reader interprets slowGaps/maxGapMs against these.
      const int cfg =
          snprintf(line_ + written, sizeof(line_) - written,
                   ",\"slowGapLoMs\":%u,\"slowGapHiMs\":%u,"
                   "\"interByteTimeoutMs\":%u",
                   transport_->slowGapLoMs(), transport_->slowGapHiMs(),
                   transport_->interByteTimeoutMs());
      if (cfg > 0) {
        written += cfg;
        if (written >= static_cast<int>(sizeof(line_))) {
          written = static_cast<int>(sizeof(line_)) - 1;
        }
      }
    }
    
    if (statusExtender_ != nullptr && (strcmp(event, "status") == 0)) {
      size_t capacity = sizeof(line_) - written - 1; // reserve 1 for '}'
      if (capacity > 0) {
         char extBuf[256] = {0};
         statusExtender_(statusExtenderContext_, extBuf, sizeof(extBuf));
         int ext_len = snprintf(line_ + written, capacity, "%s", extBuf);
         if (ext_len > 0) written += ext_len;
      }
    }

    snprintf(line_ + written, sizeof(line_) - written, "}");
    writeLine_(writeContext_, line_);
  }

  /// Parse "setbit <bit> <0|1>" (the text after "setbit ").
  VerbResult handleSetbit_(const char* args) {
    char* end = nullptr;
    const unsigned long bit = strtoul(args, &end, 10);
    if (end == args) {
      emitLine("error", "badVerb", "setbit: missing bit index");
      return VerbResult::kHandled;
    }
    while (*end == ' ') ++end;
    const char* vstart = end;
    const unsigned long val = strtoul(vstart, &end, 10);
    if (end == vstart) {
      emitLine("error", "badVerb", "setbit: missing value");
      return VerbResult::kHandled;
    }
    if (val > 1u) {
      emitLine("error", "badValue", "setbit: value must be 0 or 1");
      return VerbResult::kHandled;
    }
    if (bit >= node_->outputLength() * 8u) {
      emitLine("error", "outOfRange", "setbit: bit beyond output image");
      return VerbResult::kHandled;
    }
    node_->setOutputBit(static_cast<size_t>(bit), val != 0u);
    emitLine("setbit");
    return VerbResult::kHandled;
  }

  /// Parse "writeoutputs <hex>" (the text after "writeoutputs ").
  VerbResult handleWriteoutputs_(const char* hex) {
    uint8_t buf[RemoteNodeHandle::kMaxOutputBytes];
    size_t len = 0;
    const char* p = hex;
    while (p[0] != '\0' && p[1] != '\0') {
      const int hi = hexVal_(p[0]);
      const int lo = hexVal_(p[1]);
      if (hi < 0 || lo < 0) {
        emitLine("error", "badHex", "writeoutputs: non-hex digit");
        return VerbResult::kHandled;
      }
      if (len >= sizeof(buf)) {
        emitLine("error", "outOfRange", "writeoutputs: length beyond image");
        return VerbResult::kHandled;
      }
      buf[len++] = static_cast<uint8_t>((hi << 4) | lo);
      p += 2;
    }
    if (p[0] != '\0') {
      emitLine("error", "badHex", "writeoutputs: odd hex length");
      return VerbResult::kHandled;
    }
    if (!node_->setOutputs(buf, len)) {
      emitLine("error", "outOfRange", "writeoutputs: length beyond image");
      return VerbResult::kHandled;
    }
    emitLine("writeoutputs");
    return VerbResult::kHandled;
  }

  /// Hex digit value, or -1 for a non-hex character.
  static int hexVal_(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  CMRIHost* host_ = nullptr;
  SerialCMRITransport* transport_ = nullptr;
  RemoteNodeHandle* node_ = nullptr;
  const char* image_ = "";
  const char* version_ = "";
  LineWriter writeLine_ = nullptr;
  void* writeContext_ = nullptr;
  StatusExtender statusExtender_ = nullptr;
  void* statusExtenderContext_ = nullptr;

  uint32_t nowMs_ = 0;    ///< the main loop's injected clock
  uint32_t epochMs_ = 0;  ///< clock value at the epoch line; ts base
  uint32_t seq_ = 0;      ///< monotonic line sequence number
  bool quiesced_ = false; ///< bus handed off; polling suspended
  bool backoffTraceOnly_ = false;
  char line_[kLineCapacity] = {0};
};

}  // namespace testbed
}  // namespace CMRInet
