// CMRITracerEngine.h — the shared testbed command-and-control engine:
// plain verbs in, JSON-lines telemetry out, wrapped around CMRIHost
// and its D7 listener seam.
//
// This header IS the stage-2 invariant "same engine, same listeners,
// different main()" (map issue #21): the desktop tracer
// (extras/desktop/cmri_tracer.cpp) and the Xiao Host R&D sketch
// (examples/XiaoHostTracer) both wrap this engine, so a scenario that
// passes against one Host image passes against the other by
// construction. Only the mains differ: option parsing, the clock
// source, and the C&C byte stream.
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
  }
  return "UNKNOWN";
}

/// Telemetry spelling of a host engine event.
inline const char* eventName(CMRIHostEventType type) {
  switch (type) {
    case CMRIHostEventType::kReplyAccepted: return "reply";
    case CMRIHostEventType::kReplyRejected: return "reject";
    case CMRIHostEventType::kReplyTimeout: return "miss";
    case CMRIHostEventType::kNodeStateChanged: return "state";
  }
  return "unknown";
}

/// The command-and-control engine one tracer main() wraps: it owns
/// the telemetry line format, the seq counter, the quiesced flag, and
/// the verb vocabulary (quiesce | resume | status | quit).
///
/// Lifecycle mirrors the library's two-phase rule: bind() during the
/// configuration phase (it registers the event listener, which
/// host.begin() locks), then setNow()/handleVerb()/emitLine() at
/// runtime. Nothing here allocates or blocks.
class CMRITracerEngine {
 public:
  /// Writes one completed telemetry line (no trailing newline) to the
  /// C&C stream. The writer appends the line terminator and flushes
  /// as its stream requires.
  using LineWriter = void (*)(void* context, const char* line);

  /// What handleVerb() asks of the surrounding main loop.
  enum class VerbResult : uint8_t {
    kEmpty,    ///< blank input; nothing happened
    kHandled,  ///< verb consumed (including the unknown-verb error line)
    kQuit,     ///< end the loop; the main emits "final" on its way out
  };

  /// Wire the engine to a configured-but-not-begun host. Registers
  /// the D7 event listener, so it MUST run before host.begin() locks
  /// the configuration. `image` and `version` identify the wrapping
  /// main in every telemetry line; both strings must outlive the
  /// engine.
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
    host.onEvent(&CMRITracerEngine::onHostEvent_, this);
  }

  /// Refresh the engine's clock. Call once per loop iteration, with
  /// the same monotonic value handed to host.tick(), before verbs are
  /// dispatched.
  void setNow(uint32_t nowMs) { nowMs_ = nowMs; }

  /// Emit the epoch marker: ts restarts at 0 here, so a runner seeing
  /// this line knows every cumulative counter restarted with it. The
  /// anchor pair carries the image's absolute time reference (e.g.
  /// "wallClock" on the desktop, "bootMs" on the Xiao).
  void emitEpoch(const char* anchorKey, const char* anchorValue) {
    epochMs_ = nowMs_;
    emitLineAt_(nowMs_, "epoch", anchorKey, anchorValue);
  }

  /// Emit one telemetry line at the engine's current clock. `event`
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
    emitLine("error", "unknownVerb", verb);
    return VerbResult::kHandled;
  }

  /// True while polling is suspended by the quiesce verb.
  bool quiesced() const { return quiesced_; }

 private:
  // Inputs hex (2 chars per byte) + ~400 chars of fixed fields and
  // counters + one extra field. Truncation is guarded, not expected.
  static constexpr size_t kLineCapacity =
      2 * CMRINET_HOST_MAX_INPUT_BYTES + 512;

  /// CMRIHost event listener: one telemetry line per engine event,
  /// stamped with the event's own tick time.
  static void onHostEvent_(void* context, const CMRIHostEvent& event) {
    CMRITracerEngine& self = *static_cast<CMRITracerEngine*>(context);
    if (event.type == CMRIHostEventType::kNodeStateChanged) {
      self.emitLineAt_(event.nowMs, eventName(event.type), "previousState",
                       stateName(event.previousState));
      return;
    }
    self.emitLineAt_(event.nowMs, eventName(event.type));
  }

  void emitLineAt_(uint32_t nowMs, const char* event,
                   const char* extraKey = nullptr,
                   const char* extraValue = nullptr) {
    char inputsHex[2 * RemoteNodeHandle::kMaxInputBytes + 1] = "";
    const size_t inputLength = node_->inputLength();
    for (size_t i = 0; i < inputLength; ++i) {
      snprintf(&inputsHex[2 * i], 3, "%02X", node_->inputByte(i));
    }

    const CMRIHostStatistics& host = host_->statistics();
    const RemoteNodeStatistics& node = node_->statistics();
    const LinkStatistics& link = transport_->stats();

    int written = snprintf(
        line_, sizeof(line_),
        "{\"seq\":%u,\"ts\":%u,\"event\":\"%s\","
        "\"role\":\"host\",\"image\":\"%s\",\"version\":\"%s\","
        "\"address\":%u,\"ua\":%u,\"state\":\"%s\",\"quiesced\":%s,"
        "\"polls\":%u,\"pollRetries\":%u,\"replies\":%u,"
        "\"misses\":%u,\"rejected\":%u,\"unsolicited\":%u,"
        "\"exchanges\":%u,\"errors\":%u,\"recoveries\":%u,"
        "\"consecutiveMisses\":%u,\"lastTurnaroundMs\":%u,"
        "\"decodeErrors\":%u,\"inputs\":\"%s\"",
        ++seq_, nowMs - epochMs_, event, image_, version_, node_->address(),
        node_->ua(), stateName(node_->state()), quiesced_ ? "true" : "false",
        host.pollsSent, host.pollSendRetries, host.repliesAccepted,
        node.noReplies, host.repliesRejected, host.unsolicitedPackets,
        node.exchanges, node.errors, node.recoveries, node.consecutiveMisses,
        node.lastTurnaroundMs, link.decodeErrors, inputsHex);
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
    snprintf(line_ + written, sizeof(line_) - written, "}");
    writeLine_(writeContext_, line_);
  }

  CMRIHost* host_ = nullptr;
  SerialCMRITransport* transport_ = nullptr;
  RemoteNodeHandle* node_ = nullptr;
  const char* image_ = "";
  const char* version_ = "";
  LineWriter writeLine_ = nullptr;
  void* writeContext_ = nullptr;

  uint32_t nowMs_ = 0;    ///< the main loop's injected clock
  uint32_t epochMs_ = 0;  ///< clock value at the epoch line; ts base
  uint32_t seq_ = 0;      ///< monotonic line sequence number
  bool quiesced_ = false; ///< bus handed off; polling suspended
  char line_[kLineCapacity] = {0};
};

}  // namespace testbed
}  // namespace CMRInet
