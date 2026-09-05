// TracerShell.h — the shared testbed command-and-control shell:
// plain verbs in, JSON-lines telemetry out, wrapped around CMRIHost
// and its D7 listener seam.
//
// This header IS the stage-2 invariant "same shell, same listeners,
// different main()" (map issue #21): the desktop tracer
// (extras/desktop/cmri_tracer.cpp) and the Xiao Host R&D sketch
// (examples/TracerHost) both wrap this shell, so a scenario that
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
//
// ---- Two rules this file exists to keep straight ----
//
// 1. THE SHELL HOLDS NO NODE. It resolves host.node(UA) at the point of
//    use. Caching a RemoteNodeHandle is exactly the pattern Design v1.2
//    D5 deprecates: the storage survives a delete, but the slot may be
//    reused by a different logical device, so a cached handle silently
//    starts describing somebody else. Every verb therefore names its
//    target UA, and a UA with no live node is a *reported* error rather
//    than a silent no-op — a verb that misreports its own failure is the
//    #82 shape, and after runtime delete "not found" is an ordinary
//    outcome, not an exceptional one.
//
// 2. TELEMETRY SPEAKS THE UA, NEVER THE WIRE BYTE. Per the spec the UA
//    is the semantic UA 0..127; the wire payload carries
//    UA + ord('A'). The wire byte is an encoding detail of the transport
//    and no reader should see or key on it. This file used to emit both
//    ("UA":30,"UA":95), which is backwards and useless. See #90 for
//    the consumers still speaking the old vocabulary.

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CMRIHost.h"
#include "RemoteNodeHandle.h"
#include "transport/serial.h"

namespace CMRInet {
namespace testbed {

/// Telemetry spelling of a node health state.
///
/// Delegates to the library rendering rather than keeping a second
/// switch. Three copies of this mapping existed on one commit and two
/// of them silently rendered "??" for kMisconfigured and kDegraded; the
/// point of the shared helper is that there is now exactly one, sitting
/// inside -Werror=switch (#85, #93).
inline const char* stateName(RemoteNodeState state) {
  return remoteNodeStateString(state);
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
    case CMRIHostEventType::kBreakerTripped: return "breaker_open";
    case CMRIHostEventType::kBreakerClosed: return "breaker_close";
    // Runtime node-table mutation (issue #91). The spellings match the
    // shell's old verb-ack event names on purpose: the engine event is
    // now the single source, and onHostEvent_ renders it through the same
    // emitNodeLine_/emitHostLine_ the ack used, so a consumer sees one
    // stream with the same shape it already parsed.
    case CMRIHostEventType::kNodeAdded: return "node_add";
    case CMRIHostEventType::kNodeDeleted: return "node_delete";
    case CMRIHostEventType::kGeometryChanged: return "node_geometry";
    case CMRIHostEventType::kIllegalWireUA: return "illegal_wire_ua";
    // Dense full-T Host belief timeline (issue #112).
    case CMRIHostEventType::kExchangeComplete: return "xchg";
    case CMRIHostEventType::kUnsolicitedPacket: return "unsolicited";
  }
  return "unknown";
}

/// The command-and-control shell one tracer main() wraps: it owns the
/// telemetry line format, the seq counter, and the verb vocabulary. It
/// registers both D7 listeners — onEvent for exchange/health events and
/// onTrace for per-packet TX/RX visibility (I, T, P, R with MT and body)
/// — so the bench sees the full I/T exchange, not just its counters.
///
/// Verb vocabulary. Every verb acting on a node names its UA:
///
///   status                          three host-scope lines (see below)
///   status <UA>                     one node's image, health, counters
///   quiesce <UA> | resume <UA>      out of / back into the rotation
///   forcetx <UA>                    re-send a full T with no change
///   setbit <UA> <bit> <0|1>         one output bit
///   writeoutputs <UA> <hex>         whole output image
///   node add <UA> <type> ...        typed runtime add (Design v1.2 D5)
///                                   type C: <in> <out> [opts1 [opts2]]
///                                   type M: [ns [ct0..ct5]]  (geometry 3/6)
///                                   type N|X: <ns> <in> <out> [ct x ns]
///   node delete <UA>                runtime delete
///   node geometry <UA> <in> <out>   runtime geometry change (CPNODE)
///   node enable <UA>                alias of resume (bench probes)
///   node disable <UA>               alias of quiesce (bench probes)
///   quit                            the main owns its own exit
///
/// Lifecycle: bind() may run before or after host.begin(), since
/// listener registration is no longer locked at begin() (#86). Nothing
/// here allocates or blocks.
class TracerShell {
 public:
  /// Writes one completed telemetry line (no trailing newline) to the
  /// C&C stream. The writer appends the line terminator and flushes
  /// as its stream requires.
  using LineWriter = void (*)(void* context, const char* line);

/// Callback emitting a generators JSON fragment (leading comma OK).
  /// Invoked on its own line after the status/roster pair.
  using StatusExtender = void (*)(void* context, char* buffer,
                                  size_t remaining_capacity);

  /// What handleVerb() asks of the surrounding main loop.
  enum class VerbResult : uint8_t {
    kEmpty,    ///< blank input; nothing happened
    kHandled,  ///< verb consumed (including any error line)
    kQuit,     ///< end the loop; the main emits "final" on its way out
  };

  /// Wire the shell to a host. `image` and `version` identify the
  /// wrapping main on identity lines (`epoch` and `status`); both
  /// strings must outlive the shell.
  ///
  /// Takes no node: the shell resolves host.node(UA) at the point of use
  /// (Design v1.2 D5).
  void bind(CMRIHost& host, SerialCMRITransport& transport,
            const char* image, const char* version,
            LineWriter writeLine, void* writeContext) {
    host_ = &host;
    transport_ = &transport;
    image_ = image;
    version_ = version;
    writeLine_ = writeLine;
    writeContext_ = writeContext;
    host.onEvent(&TracerShell::onHostEvent_, this);
    host.onTrace(&TracerShell::onHostTrace_, this);
  }

/// Register an optional callback that emits a generators JSON fragment
  /// (e.g. `,"generators":{...}`) as its own line after `status`.
  /// Kept off the counters/roster lines so a long extender cannot
  /// truncate the host ledger under CDC backpressure.
  void setStatusExtender(StatusExtender extender, void* context = nullptr) {
    statusExtender_ = extender;
    statusExtenderContext_ = context;
  }

  /// Refresh the shell's clock. Call once per loop iteration, with the
  /// same monotonic value handed to host.tick(), before verbs are
  /// dispatched.
  void setNow(uint32_t nowMs) { nowMs_ = nowMs; }

  /// Manually emit a trace record (e.g. replaying a captured ring).
  void emitPacket(bool transmit, const CMRIPacket& packet) {
    emitTrace_(transmit, packet);
  }

  /// Emit the epoch marker: ts restarts at 0 here, so a runner seeing
  /// this line knows every cumulative counter restarted with it. The
  /// anchor pair carries the image's absolute time reference.
  void emitEpoch(const char* anchorKey, const char* anchorValue) {
    epochMs_ = nowMs_;
    emitHostLine_(nowMs_, "epoch", kNoUA, anchorKey, anchorValue,
                  /*identity=*/true, /*config=*/true, /*roster=*/true);
  }

/// Emit one host-scope line at the shell's current clock.
  ///
  /// `status` is special: it is a three-line bundle (counters, roster,
  /// optional generators). Everything else remains a single line.
  void emitLine(const char* event, const char* extraKey = nullptr,
                const char* extraValue = nullptr) {
    if (strcmp(event, "status") == 0) {
      emitStatusBundle_();
      return;
    }
    emitHostLine_(nowMs_, event, kNoUA, extraKey, extraValue,
                  /*identity=*/false, /*config=*/false, /*roster=*/false);
  }

  /// Dispatch one C&C verb. Unknown verbs emit an error line and count
  /// as handled; "quit" is not acted on here — the main owns its exit.
  VerbResult handleVerb(const char* verb) {
    if (verb == nullptr || verb[0] == '\0') {
      return VerbResult::kEmpty;
    }
    if (strcmp(verb, "quit") == 0) {
      return VerbResult::kQuit;
    }
if (strcmp(verb, "status") == 0) {
      // Three small lines, not one large one: host counters, roster, and
      // (when an extender is registered) generators. A single line with
      // all three routinely exceeded the CDC write budget and arrived
      // truncated on the gather side, which left status_snapshot null
      // and analyzers false-FAIL.
      emitStatusBundle_();
      return VerbResult::kHandled;
    }
    if (strncmp(verb, "status ", 7) == 0) {
      return withNode_(verb, verb + 7, &TracerShell::actStatus_);
    }
    if (strncmp(verb, "quiesce ", 8) == 0) {
      return withNode_(verb, verb + 8, &TracerShell::actQuiesce_);
    }
    if (strncmp(verb, "resume ", 7) == 0) {
      return withNode_(verb, verb + 7, &TracerShell::actResume_);
    }
    if (strncmp(verb, "forcetx ", 8) == 0) {
      return withNode_(verb, verb + 8, &TracerShell::actForcetx_);
    }
    if (strncmp(verb, "setbit ", 7) == 0) {
      return withNode_(verb, verb + 7, &TracerShell::actSetbit_);
    }
    if (strncmp(verb, "writeoutputs ", 13) == 0) {
      return withNode_(verb, verb + 13, &TracerShell::actWriteoutputs_);
    }
    if (strncmp(verb, "node ", 5) == 0) {
      return handleNode_(verb, verb + 5);
    }
    emitLine("error", "unknownVerb", verb);
    return VerbResult::kHandled;
  }

  /// Capture-mode filter (issue #112 / TracerHost `run`). When
  /// enabled, suppress high-rate chatter (state, reinit, node_*,
  /// breaker_*, plain reply without enrichment need) but keep the dense-T
  /// decision table live: miss, reject, xchg, unsolicited, and backoff.
  /// Full packet `trace` lines stay off during `run` (ring owns them);
  /// this flag only gates the onEvent path.
  void setBackoffTraceOnly(bool enabled) { backoffTraceOnly_ = enabled; }

 private:
  /// Sentinel for "this line is not about one node".
  static constexpr int kNoUA = -1;

  /// One roster entry: UA, geometry, state, enabled.
  static constexpr size_t kRosterEntryBytes = 80;

  // Whichever line is longest bounds the buffer. A node line carries
  // both image hex strings plus the health block (three axis names,
  // observed geometry, and the last-fault detail, ~200 bytes); a host
  // line carries the roster. No line carries both, so this is generous
  // rather than tight.
  static constexpr size_t kLineCapacity =
      2 * CMRINET_HOST_MAX_INPUT_BYTES +
      2 * CMRINET_HOST_MAX_OUTPUT_BYTES + 896 +
      CMRINET_HOST_MAX_NODES * kRosterEntryBytes;

  /// A verb action, run once its UA has resolved to a live node.
  /// `args` points just past the UA.
  using NodeAction = VerbResult (TracerShell::*)(const char* verb,
                                                 const char* args,
                                                 RemoteNodeHandle& node);

  // ------------------------------------------------ verb plumbing

  /// Parse a decimal field, skipping leading spaces.
  static bool parseUint_(const char*& p, unsigned long& out) {
    while (*p == ' ') {
      ++p;
    }
    char* end = nullptr;
    const unsigned long value = strtoul(p, &end, 10);
    if (end == p) {
      return false;
    }
    p = end;
    out = value;
    return true;
  }

  /// Resolve the leading UA argument, then run `action` against the live
  /// node it names.
  ///
  /// A missing node reports and stops. This is the one place that policy
  /// lives, so no individual verb can forget it: after runtime delete a
  /// null lookup is an ordinary outcome, and a verb that quietly did
  /// nothing would be lying about its own outcome.
  VerbResult withNode_(const char* verb, const char* args,
                       NodeAction action) {
    unsigned long parsed = 0;
    if (!parseUint_(args, parsed)) {
      emitLine("error", "badVerb", verb);
      return VerbResult::kHandled;
    }
    if (parsed > 127u) {
      emitLine("error", "uaOutOfRange", verb);
      return VerbResult::kHandled;
    }
    RemoteNodeHandle* node = host_->node(static_cast<uint8_t>(parsed));
    if (node == nullptr) {
      emitLine("error", "noSuchNode", verb);
      return VerbResult::kHandled;
    }
    return (this->*action)(verb, args, *node);
  }

  VerbResult actStatus_(const char*, const char*, RemoteNodeHandle& node) {
    emitNodeLine_(nowMs_, "status", node, /*identity=*/true);
    return VerbResult::kHandled;
  }

  VerbResult actQuiesce_(const char*, const char*, RemoteNodeHandle& node) {
    node.setEnabled(false);
    emitNodeLine_(nowMs_, "quiesce", node);
    return VerbResult::kHandled;
  }

  VerbResult actResume_(const char*, const char*, RemoteNodeHandle& node) {
    node.setEnabled(true);
    emitNodeLine_(nowMs_, "resume", node);
    return VerbResult::kHandled;
  }

  VerbResult actForcetx_(const char*, const char*, RemoteNodeHandle& node) {
    node.forceTransmit();
    emitNodeLine_(nowMs_, "forcetx", node);
    return VerbResult::kHandled;
  }

  VerbResult actNodeEnable_(const char*, const char*, RemoteNodeHandle& node) {
    node.setEnabled(true);
    emitNodeLine_(nowMs_, "node_enable", node);
    return VerbResult::kHandled;
  }

  VerbResult actNodeDisable_(const char*, const char*, RemoteNodeHandle& node) {
    node.setEnabled(false);
    emitNodeLine_(nowMs_, "node_disable", node);
    return VerbResult::kHandled;
  }

  /// "setbit <UA> <bit> <0|1>"
  VerbResult actSetbit_(const char*, const char* args,
                        RemoteNodeHandle& node) {
    unsigned long bit = 0;
    if (!parseUint_(args, bit)) {
      emitLine("error", "badVerb", "setbit: missing bit index");
      return VerbResult::kHandled;
    }
    unsigned long value = 0;
    if (!parseUint_(args, value)) {
      emitLine("error", "badVerb", "setbit: missing value");
      return VerbResult::kHandled;
    }
    if (value > 1u) {
      emitLine("error", "badValue", "setbit: value must be 0 or 1");
      return VerbResult::kHandled;
    }
    if (bit >= node.outputLength() * 8u) {
      emitLine("error", "outOfRange", "setbit: bit beyond output image");
      return VerbResult::kHandled;
    }
    node.setOutputBit(static_cast<size_t>(bit / 8), static_cast<size_t>(bit % 8), value != 0u);
    emitNodeLine_(nowMs_, "setbit", node);
    return VerbResult::kHandled;
  }

  /// "writeoutputs <UA> <hex>"
  VerbResult actWriteoutputs_(const char*, const char* args,
                              RemoteNodeHandle& node) {
    while (*args == ' ') {
      ++args;
    }
    uint8_t buf[RemoteNodeHandle::kMaxOutputBytes];
    size_t len = 0;
    const char* p = args;
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
    if (!node.setOutputs(buf, len)) {
      emitLine("error", "outOfRange", "writeoutputs: length beyond image");
      return VerbResult::kHandled;
    }
    emitNodeLine_(nowMs_, "writeoutputs", node);
    return VerbResult::kHandled;
  }

  /// The D5 mutation verbs. These exist so the runtime-mutation paths
  /// can be driven against real wire timing: a mock transport cannot
  /// reproduce an orphan across an actual TXEN drain, which is where the
  /// interaction hazards live.
  VerbResult handleNode_(const char* verb, const char* args) {
    while (*args == ' ') {
      ++args;
    }
    if (strncmp(args, "add ", 4) == 0) {
      return handleNodeAdd_(args + 4);
    }
    if (strncmp(args, "delete ", 7) == 0) {
      return handleNodeDelete_(args + 7);
    }
    if (strncmp(args, "geometry ", 9) == 0) {
      return handleNodeGeometry_(args + 9);
    }
    if (strncmp(args, "enable ", 7) == 0) {
      return withNode_(verb, args + 7, &TracerShell::actNodeEnable_);
    }
    if (strncmp(args, "disable ", 8) == 0) {
      return withNode_(verb, args + 8, &TracerShell::actNodeDisable_);
    }
    emitLine("error", "unknownVerb", verb);
    return VerbResult::kHandled;
  }

  // Thin wrapper (issue #91): parse, call the mutator, emit only the
  // error line on rejection. On success the engine fires kNodeAdded,
  // which onHostEvent_ renders through the same emitNodeLine_ this used
  // to call, so the line shape is unchanged. The mutator validates the
  // UA range; the shell's old UA > 127 pre-check was redundant with it.
  VerbResult handleNodeAdd_(const char* args) {
    // Breaking grammar: node add <UA> <type> ...
    // C: <in> <out> [opts1 [opts2]]
    // M: [ns [ct0..ct5]]
    // N|X: <ns> <in> <out> [ct x ns]
    unsigned long UA = 0;
    if (!parseUint_(args, UA)) {
      emitLine("error", "badVerb",
               "node add: want <UA> <type> ...");
      return VerbResult::kHandled;
    }
    while (*args == ' ') {
      ++args;
    }
    if (*args == '\0') {
      emitLine("error", "badVerb",
               "node add: want <UA> <type> ...");
      return VerbResult::kHandled;
    }
    // Type token: single letter C/M/N/X (case-insensitive).
    char typeCh = *args;
    if (typeCh >= 'a' && typeCh <= 'z') {
      typeCh = static_cast<char>(typeCh - 'a' + 'A');
    }
    ++args;
    if (*args != ' ' && *args != '\0') {
      emitLine("error", "badVerb",
               "node add: type must be C, M, N, or X");
      return VerbResult::kHandled;
    }
    NodeType type;
    if (!nodeTypeFromNdp(typeCh, type)) {
      emitLine("error", "badVerb",
               "node add: type must be C, M, N, or X");
      return VerbResult::kHandled;
    }

    CMRIHost::ConfigStatus status = CMRIHost::ConfigStatus::kBadInit;
    if (type == NodeType::kCpnode) {
      unsigned long in = 0, out = 0, opts1 = 0, opts2 = 0;
      if (!parseUint_(args, in) || !parseUint_(args, out)) {
        emitLine("error", "badVerb",
                 "node add C: want <UA> C <in> <out> [opts1 [opts2]]");
        return VerbResult::kHandled;
      }
      // opts optional
      (void)parseUint_(args, opts1);
      (void)parseUint_(args, opts2);
      CpnodeInit init;
      init.inputBytes = static_cast<uint16_t>(in);
      init.outputBytes = static_cast<uint16_t>(out);
      init.opts1 = static_cast<uint8_t>(opts1);
      init.opts2 = static_cast<uint8_t>(opts2);
      status = host_->addRemoteNode(static_cast<uint8_t>(UA), init);
    } else if (type == NodeType::kSmini) {
      SminiInit init;
      unsigned long ns = 0;
      if (parseUint_(args, ns)) {
        if (ns > SminiInit::kMaxNs) {
          emitLine("error", "badVerb", "node add M: ns out of range");
          return VerbResult::kHandled;
        }
        init.ns = static_cast<uint8_t>(ns);
        if (init.ns > 0) {
          for (uint8_t i = 0; i < SminiInit::kCtCount; ++i) {
            unsigned long ct = 0;
            if (!parseUint_(args, ct) || ct > 255ul) {
              emitLine("error", "badVerb",
                       "node add M: want 6 CT bytes when ns>0");
              return VerbResult::kHandled;
            }
            init.ct[i] = static_cast<uint8_t>(ct);
          }
        }
      }
      status = host_->addRemoteNode(static_cast<uint8_t>(UA), init);
    } else {
      // USIC / SUSIC
      unsigned long ns = 0, in = 0, out = 0;
      if (!parseUint_(args, ns) || !parseUint_(args, in) ||
          !parseUint_(args, out)) {
        emitLine("error", "badVerb",
                 "node add N|X: want <UA> N|X <ns> <in> <out> [ct...]");
        return VerbResult::kHandled;
      }
      if (ns < 1 || ns > UsicFamilyInit::kMaxNs) {
        emitLine("error", "badVerb", "node add N|X: ns out of range");
        return VerbResult::kHandled;
      }
      UsicFamilyInit init;
      init.ns = static_cast<uint8_t>(ns);
      init.inputBytes = static_cast<uint16_t>(in);
      init.outputBytes = static_cast<uint16_t>(out);
      for (uint8_t i = 0; i < init.ns; ++i) {
        unsigned long ct = 0;
        if (!parseUint_(args, ct) || ct > 255ul) {
          // CT optional fill with 0 if omitted? Prefer explicit.
          emitLine("error", "badVerb",
                   "node add N|X: want ns CT bytes");
          return VerbResult::kHandled;
        }
        init.ct[i] = static_cast<uint8_t>(ct);
      }
      status = host_->addRemoteNode(static_cast<uint8_t>(UA), type, init);
    }

    if (status != CMRIHost::ConfigStatus::kOk) {
      emitLine("error", "addFailed", configStatusString(status));
      return VerbResult::kHandled;
    }
    // Success: the engine's kNodeAdded event rendered the line.
    return VerbResult::kHandled;
  }

  // Thin wrapper (issue #91): on success the engine fires kNodeDeleted,
  // which onHostEvent_ renders host-scoped with the departed UA and the
  // post-delete roster -- the same line this used to emit. The mutator
  // validates the UA range.
  VerbResult handleNodeDelete_(const char* args) {
    unsigned long UA = 0;
    if (!parseUint_(args, UA)) {
      emitLine("error", "badVerb", "node delete: want <UA>");
      return VerbResult::kHandled;
    }
    const CMRIHost::ConfigStatus status =
        host_->deleteRemoteNode(static_cast<uint8_t>(UA));
    if (status != CMRIHost::ConfigStatus::kOk) {
      emitLine("error", "deleteFailed", configStatusString(status));
      return VerbResult::kHandled;
    }
    // Success: the engine's kNodeDeleted event rendered the line.
    return VerbResult::kHandled;
  }

  // Thin wrapper (issue #91): on success the engine fires
  // kGeometryChanged, which onHostEvent_ renders node-scoped with the
  // previous and new NI/NO -- a strict superset of the line this used to
  // emit. The mutator validates the UA range and byte ceilings.
  VerbResult handleNodeGeometry_(const char* args) {
    unsigned long UA = 0, in = 0, out = 0;
    if (!parseUint_(args, UA) || !parseUint_(args, in) ||
        !parseUint_(args, out)) {
      emitLine("error", "badVerb", "node geometry: want <UA> <in> <out>");
      return VerbResult::kHandled;
    }
    const CMRIHost::ConfigStatus status = host_->setRemoteNodeGeometry(
        static_cast<uint8_t>(UA), static_cast<uint16_t>(in),
        static_cast<uint16_t>(out));
    if (status != CMRIHost::ConfigStatus::kOk) {
      emitLine("error", "geometryFailed", configStatusString(status));
      return VerbResult::kHandled;
    }
    // Success: the engine's kGeometryChanged event rendered the line.
    return VerbResult::kHandled;
  }

  // ------------------------------------------------ listeners

  /// One line per engine event, stamped with the event's own tick time
  /// and attributed to the event's own node — not to whatever node the
  /// shell happened to be bound to, which is how every line on a
  /// multi-node bench used to claim the same UA.
  static void onHostEvent_(void* context, const CMRIHostEvent& event) {
    TracerShell& self = *static_cast<TracerShell*>(context);
    if (event.type == CMRIHostEventType::kPollBackoffChanged) {
      self.emitBackoffTrace_(event);
      return;
    }
    // kNodeDeleted is host-scoped: the slot is already cleaned, so
    // event.node is null by design (D5: delete cleans before the event
    // fires). Render it before the null-node early-return below, or the
    // event would be swallowed. The roster reflects the post-delete table
    // (issue #91).
    if (event.type == CMRIHostEventType::kNodeDeleted) {
      if (self.backoffTraceOnly_) {
        return;
      }
      self.emitHostLine_(event.nowMs, "node_delete",
                         static_cast<int>(event.departedUA), nullptr,
                         nullptr, /*identity=*/false, /*config=*/false,
                         /*roster=*/true);
      return;
    }
    // Issue #112: keep miss/reject/xchg/unsolicited live during capture.
    // Everything else still drops under backoffTraceOnly_.
    if (self.backoffTraceOnly_ &&
        event.type != CMRIHostEventType::kReplyTimeout &&
        event.type != CMRIHostEventType::kReplyRejected &&
        event.type != CMRIHostEventType::kExchangeComplete &&
        event.type != CMRIHostEventType::kUnsolicitedPacket) {
      return;
    }
    if (event.type == CMRIHostEventType::kIllegalWireUA) {
      // Host-scoped, node null (issue #96). Keep the existing swallow
      // outside capture; during normal mode still no dedicated renderer
      // beyond counters on status.
      return;
    }
    if (event.type == CMRIHostEventType::kUnsolicitedPacket ||
        event.type == CMRIHostEventType::kExchangeComplete) {
      self.emitExchangeScoped_(event);
      return;
    }
    if (event.node == nullptr) {
      return;
    }
    if (event.type == CMRIHostEventType::kNodeStateChanged) {
      self.emitNodeLine_(event.nowMs, eventName(event.type), *event.node,
                         /*identity=*/false, "previousState",
                         stateName(event.previousState));
      return;
    }
    // kGeometryChanged carries the old NI/NO alongside the new (already on
    // the node line as in/out), so a reader can tell what changed without
    // dereferencing the handle (issue #91).
    if (event.type == CMRIHostEventType::kGeometryChanged) {
      char extra[40];
      snprintf(extra, sizeof(extra),
               ",\"previousIn\":%u,\"previousOut\":%u",
               static_cast<unsigned>(event.previousInputBytes),
               static_cast<unsigned>(event.previousOutputBytes));
      self.emitNodeLine_(event.nowMs, eventName(event.type), *event.node,
                         /*identity=*/false, /*extraKey=*/nullptr,
                         /*extraValue=*/nullptr, extra);
      return;
    }
    if (event.type == CMRIHostEventType::kReplyAccepted ||
        event.type == CMRIHostEventType::kReplyRejected ||
        event.type == CMRIHostEventType::kReplyTimeout) {
      self.emitExchangeOutcome_(event);
      return;
    }
    // kNodeAdded and remaining health events render node-scoped.
    self.emitNodeLine_(event.nowMs, eventName(event.type), *event.node);
  }

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
    const char mtBuf[2] = {static_cast<char>(packet.mt), '\0'};
    // Uniform trace line: every trace line carries the same field set,
    // regardless of whether the wire-UA byte is legal. A consumer
    // reads "legal" once and branches — no field-presence check, no
    // variable stream that swaps between telemetry and raw.
    //
    // When legal, "UA" is the decoded ordinal (wireUA - kWireUAOffset).
    // When illegal, "UA" is null and "wireUA" carries the raw byte
    // for diagnosis. Per ADR-0001: raw views are not decoded telemetry.
    const bool legal = isLegalWireUA(packet.wireUA);
    int written = appendf_(
        0,
        "{\"seq\":%u,\"ts\":%u,\"event\":\"trace\",\"role\":\"host\","
        "\"wireUA\":%u,\"legal\":%s,\"UA\":",
        static_cast<unsigned>(++seq_),
        static_cast<unsigned>(nowMs_ - epochMs_),
        static_cast<unsigned>(packet.wireUA),
        legal ? "true" : "false");
    if (legal) {
      written = appendf_(written, "%u",
          static_cast<unsigned>(toSemanticUA(packet.wireUA)));
    } else {
      written = appendf_(written, "null");
    }
    written = appendf_(written,
        ",\"dir\":\"%s\",\"mt\":\"%s\",\"body\":\"%s\"",
        transmit ? "tx" : "rx", mtBuf, bodyHex);
    // T fingerprint (issue #112): short hash so dense full-T captures can
    // match identifiable stim bodies without dumping the whole image on
    // every line. FNV-1a 32-bit over the body bytes.
    if (packet.mt == MessageType::kTransmitData || packet.mt == 'T') {
      written = appendf_(written, ",\"fp\":\"%08X\"",
                         static_cast<unsigned>(fnv1a32_(packet.body, len)));
    }
    finish_(written);
  }

  /// Compact exchange-shaped lines: xchg and unsolicited (issue #112).
  /// May be node-null (orphan xchg, idle unsolicited).
  void emitExchangeScoped_(const CMRIHostEvent& event) {
    const char* name = eventName(event.type);
    int written = appendf_(
        0,
        "{\"seq\":%u,\"ts\":%u,\"event\":\"%s\",\"role\":\"host\"",
        static_cast<unsigned>(++seq_),
        static_cast<unsigned>(event.nowMs - epochMs_), name);
    if (event.node != nullptr) {
      written = appendf_(written, ",\"ua\":%u,\"present\":true",
                         event.node->UA());
    } else {
      written = appendf_(written, ",\"ua\":null,\"present\":false");
    }
    written = appendExchangeFields_(written, event);
    if (event.type == CMRIHostEventType::kUnsolicitedPacket) {
      const char mtPrint =
          (event.replyMt >= 0x20 && event.replyMt <= 0x7E)
              ? static_cast<char>(event.replyMt)
              : '.';
      written = appendf_(
          written,
          ",\"replyLen\":%u,\"replyWireUA\":%u,\"replyMt\":\"%c\"",
          static_cast<unsigned>(event.replyLength),
          static_cast<unsigned>(event.replyWireUA), mtPrint);
    }
    finish_(written);
  }

  /// reply / reject / miss with gate + transport snapshot (issue #112).
  void emitExchangeOutcome_(const CMRIHostEvent& event) {
    char extra[384];
    int n = 0;
    n += snprintf(extra + n, sizeof(extra) - static_cast<size_t>(n),
                  ",\"kind\":\"%s\",\"prevKind\":\"%s\",\"outcome\":\"%s\""
                  ",\"gateArmedMs\":%lu,\"gateMs\":%lu",
                  hostExchangeKindString(event.kind),
                  hostExchangeKindString(event.prevKind),
                  hostExchangeOutcomeString(event.outcome),
                  static_cast<unsigned long>(event.gateArmedMs),
                  static_cast<unsigned long>(event.gateMs));
    if (event.type == CMRIHostEventType::kReplyRejected) {
      const char mtPrint =
          (event.replyMt >= 0x20 && event.replyMt <= 0x7E)
              ? static_cast<char>(event.replyMt)
              : '.';
      n += snprintf(extra + n, sizeof(extra) - static_cast<size_t>(n),
                    ",\"rejectReason\":\"%s\",\"replyLen\":%u"
                    ",\"replyWireUA\":%u,\"replyMt\":\"%c\""
                    ",\"expectedLen\":%u",
                    replyRejectReasonString(event.rejectReason),
                    static_cast<unsigned>(event.replyLength),
                    static_cast<unsigned>(event.replyWireUA), mtPrint,
                    static_cast<unsigned>(event.expectedLength));
    }
    if (event.type == CMRIHostEventType::kReplyTimeout ||
        event.type == CMRIHostEventType::kReplyRejected) {
      n += appendTransportSnapshot_(extra + n,
                                    sizeof(extra) - static_cast<size_t>(n));
    }
    (void)n;
    emitNodeLine_(event.nowMs, eventName(event.type), *event.node,
                  /*identity=*/false, /*extraKey=*/nullptr,
                  /*extraValue=*/nullptr, extra);
  }

  int appendExchangeFields_(int written, const CMRIHostEvent& event) {
    return appendf_(
        written,
        ",\"kind\":\"%s\",\"prevKind\":\"%s\",\"outcome\":\"%s\""
        ",\"gateArmedMs\":%lu,\"gateMs\":%lu",
        hostExchangeKindString(event.kind),
        hostExchangeKindString(event.prevKind),
        hostExchangeOutcomeString(event.outcome),
        static_cast<unsigned long>(event.gateArmedMs),
        static_cast<unsigned long>(event.gateMs));
  }

  /// Transport + decoder counters snapshot. Appends a JSON object fragment
  /// starting with a leading comma. Used on miss/reject so a stall can be
  /// scored against echo-cancel / decoder health without a status poll.
  int appendTransportSnapshot_(char* buf, size_t cap) {
    if (transport_ == nullptr || cap == 0) {
      return 0;
    }
    const LinkStatistics& link = transport_->stats();
    const CMRIFrameDecoder::Statistics& dec =
        transport_->decoderStatistics();
    const char* echo =
        echoCancelModeName_(transport_->echoCancelMode());
    return snprintf(
        buf, cap,
        ",\"transport\":{\"rxDuringTx\":%lu,\"echo\":\"%s\""
        ",\"decodeErrors\":%lu,\"timeoutAborts\":%lu"
        ",\"danglingDle\":%lu,\"overflowAborts\":%lu"
        ",\"headerAborts\":%lu,\"droppedPackets\":%lu"
        ",\"framesRestarted\":%lu,\"slowGaps\":%lu}",
        static_cast<unsigned long>(transport_->rxDuringTx()), echo,
        static_cast<unsigned long>(link.decodeErrors),
        static_cast<unsigned long>(dec.timeoutAborts),
        static_cast<unsigned long>(dec.danglingDle),
        static_cast<unsigned long>(dec.overflowAborts),
        static_cast<unsigned long>(dec.headerAborts),
        static_cast<unsigned long>(dec.droppedPackets),
        static_cast<unsigned long>(dec.framesRestarted),
        static_cast<unsigned long>(dec.slowGaps));
  }

  static const char* echoCancelModeName_(
      SerialCMRITransport::EchoCancelMode mode) {
    switch (mode) {
      case SerialCMRITransport::EchoCancelMode::kAuto: return "auto";
      case SerialCMRITransport::EchoCancelMode::kAlwaysOn: return "on";
      case SerialCMRITransport::EchoCancelMode::kAlwaysOff: return "off";
    }
    return "unknown";
  }

  static uint32_t fnv1a32_(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
      h ^= data[i];
      h *= 16777619u;
    }
    return h;
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
    int written = appendf_(
        0,
        "{\"seq\":%u,\"ts\":%u,\"event\":\"diag_backoff_trace\","
        "\"role\":\"host\",\"ua\":%u,"
        "\"old_backoff_ms\":%lu,\"new_backoff_ms\":%lu,\"reason\":\"%s\","
        "\"deadline_action\":\"%s\",\"now_ms\":%lu",
        static_cast<unsigned>(++seq_),
        static_cast<unsigned>(event.nowMs - epochMs_),
        event.node->UA(),
        static_cast<unsigned long>(event.previousPollBackoffMs),
        static_cast<unsigned long>(event.newPollBackoffMs),
        pollBackoffChangeReasonString(event.pollBackoffReason),
        deadlineAction, static_cast<unsigned long>(event.nowMs));
    finish_(written);
  }

// ------------------------------------------------ line emission
  //
  // Two scopes, because the fields have two different owners. Host scope
  // carries what CMRIHost, the transport, and the decoder own; node
  // scope carries what a RemoteNodeHandle owns. Mixing them into one
  // flat line was only possible while the shell pretended there was
  // exactly one node.
  //
  // Host-scope `status` is further split into a three-line bundle so no
  // single CDC write carries counters + roster + generators at once:
  //   1. event="status"      host counters + degraded ledger + identity
  //   2. event="roster"      the live node table only
  //   3. event="generators"  optional StatusExtender payload (sketch)
  // Epoch still carries roster on one line: it is rare and small enough.

  /// Emit the host-scope status bundle as three short JSON lines.
  void emitStatusBundle_() {
    // 1) Counters and ledger — the fields degraded-lane scoring needs.
    emitHostLine_(nowMs_, "status", kNoUA, nullptr, nullptr,
                  /*identity=*/true, /*config=*/false, /*roster=*/false);
    // 2) Roster alone — the membership view dual-node scoring needs.
    emitHostLine_(nowMs_, "roster", kNoUA, nullptr, nullptr,
                  /*identity=*/false, /*config=*/false, /*roster=*/true);
    // 3) Generators, when the wrapping main registered an extender.
    if (statusExtender_ != nullptr) {
      emitGeneratorsLine_();
    }
  }

  /// Own line for the StatusExtender payload. The extender still returns
  /// a leading-comma fragment (historical contract with emitHostLine_);
  /// we strip the comma and wrap it as a complete object.
  void emitGeneratorsLine_() {
    char extBuf[256] = {0};
    statusExtender_(statusExtenderContext_, extBuf, sizeof(extBuf));
    const char* payload = extBuf;
    if (payload[0] == ',') {
      ++payload;
    }
    if (payload[0] == '\0') {
      return;
    }
    int written = appendf_(
        0, "{\"seq\":%u,\"ts\":%u,\"event\":\"generators\",\"role\":\"host\",%s",
        static_cast<unsigned>(++seq_),
        static_cast<unsigned>(nowMs_ - epochMs_), payload);
    finish_(written);
  }

  void emitHostLine_(uint32_t nowMs, const char* event, int UA,
                     const char* extraKey, const char* extraValue,
                     bool identity, bool config, bool roster) {
    const CMRIHostStatistics& host = host_->statistics();
    const LinkStatistics& link = transport_->stats();
    const CMRIFrameDecoder::Statistics& decoder =
        transport_->decoderStatistics();

    int written = appendf_(
        0, "{\"seq\":%u,\"ts\":%u,\"event\":\"%s\",\"role\":\"host\"",
        static_cast<unsigned>(++seq_),
        static_cast<unsigned>(nowMs - epochMs_), event);
    if (identity) {
      written = appendf_(written, ",\"image\":\"%s\",\"version\":\"%s\"",
                         image_, version_);
    }
    if (UA != kNoUA) {
      // Named, but no longer present: the distinction a reader needs
      // after a delete.
      written = appendf_(written, ",\"ua\":%d,\"present\":false", UA);
    }
    written = appendf_(
        written,
        ",\"nodes\":%u,\"polls\":%u,\"pollRetries\":%u,\"replies\":%u"
        ",\"rejected\":%u,\"unsolicited\":%u,\"orphaned\":%u"
        ",\"decodeErrors\":%u,\"slowGaps\":%u,\"maxGapMs\":%u",
        static_cast<unsigned>(host_->nodeCount()),
        static_cast<unsigned>(host.pollsSent),
        static_cast<unsigned>(host.pollSendRetries),
        static_cast<unsigned>(host.repliesAccepted),
        static_cast<unsigned>(host.repliesRejected),
        static_cast<unsigned>(host.unsolicitedPackets),
        static_cast<unsigned>(host.orphanedExchanges),
        static_cast<unsigned>(link.decodeErrors),
        static_cast<unsigned>(decoder.slowGaps),
        static_cast<unsigned>(decoder.maxGapMs));

    // The degraded-lane ledger (D17). Host scope because the bound is on
    // the aggregate: the question an operator asks is "how much of this
    // bus is going to broken nodes", not "how much is going to UA31".
    //
    // Both denial counters are carried, not just a total, because which
    // gate bound says which failure mode is costing the layout: slot
    // denials mean nonconforming-but-answering nodes, bandwidth denials
    // mean silent ones. A single number would hide exactly the asymmetry
    // the two gates exist to separate.
    // VALIDATION: Design v1.5 D17: two gates, and the ceiling clamp that
    // keeps the degraded class from being starved to zero.
    written = appendf_(
        written,
        ",\"degradedGrants\":%u,\"degradedSlotDenials\":%u"
        ",\"degradedBandwidthDenials\":%u,\"degradedClampBypasses\":%u",
        static_cast<unsigned>(host.degradedGrants),
        static_cast<unsigned>(host.degradedSlotDenials),
        static_cast<unsigned>(host.degradedBandwidthDenials),
        static_cast<unsigned>(host.degradedClampBypasses));
    if (config) {
      // The gap-observability band is config, not signal: the thresholds
      // every counter above was collected against.
      written = appendf_(written,
                         ",\"slowGapLoMs\":%u,\"slowGapHiMs\":%u,"
                         "\"interByteTimeoutMs\":%u",
                         static_cast<unsigned>(transport_->slowGapLoMs()),
                         static_cast<unsigned>(transport_->slowGapHiMs()),
                         static_cast<unsigned>(transport_->interByteTimeoutMs()));
    }
    if (extraKey != nullptr && extraValue != nullptr) {
      written = appendf_(written, ",\"%s\":\"%s\"", extraKey, extraValue);
    }
if (roster) {
      // Roster only. Generators ride their own line via
      // emitGeneratorsLine_ so a long extender cannot truncate the
      // membership view under CDC backpressure.
      written = appendRoster_(written);
    }
    finish_(written);
  }

  void emitNodeLine_(uint32_t nowMs, const char* event,
                     const RemoteNodeHandle& node, bool identity = false,
                     const char* extraKey = nullptr,
                     const char* extraValue = nullptr,
                     const char* extraJson = nullptr) {
    char inputsHex[2 * RemoteNodeHandle::kMaxInputBytes + 1] = "";
    const size_t inputLength = node.inputLength();
    for (size_t i = 0; i < inputLength; ++i) {
      snprintf(&inputsHex[2 * i], 3, "%02X", node.inputByte(i));
    }
    char outputsHex[2 * RemoteNodeHandle::kMaxOutputBytes + 1] = "";
    const size_t outputLength = node.outputLength();
    for (size_t i = 0; i < outputLength; ++i) {
      snprintf(&outputsHex[2 * i], 3, "%02X", node.outputByte(i));
    }
    const RemoteNodeStatistics& stats = node.statistics();

    // Observed geometry renders as JSON null when nothing has been
    // demonstrated yet. Its absence is meaningful, and 0 is a legal
    // geometry, so a magic number here would be misread as a byte count
    // (D14 L1).
    char observedIn[12];
    if (node.observedInputBytes() ==
        RemoteNodeHandle::kGeometryNeverObserved) {
      snprintf(observedIn, sizeof(observedIn), "null");
    } else {
      snprintf(observedIn, sizeof(observedIn), "%u",
               static_cast<unsigned>(node.observedInputBytes()));
    }
    const ConformanceFaultRecord& fault = node.lastConformanceFault();

    int written = appendf_(
        0, "{\"seq\":%u,\"ts\":%u,\"event\":\"%s\",\"role\":\"host\"",
        static_cast<unsigned>(++seq_),
        static_cast<unsigned>(nowMs - epochMs_), event);
    if (identity) {
      written = appendf_(written, ",\"image\":\"%s\",\"version\":\"%s\"",
                         image_, version_);
    }
    written = appendf_(
        written,
        ",\"ua\":%u,\"present\":true,\"state\":\"%s\",\"enabled\":%s"
        ",\"in\":%u,\"out\":%u"
        ",\"exchanges\":%u,\"misses\":%u,\"errors\":%u,\"recoveries\":%u"
        ",\"consecutiveMisses\":%u,\"lastTurnaroundMs\":%u"
        ",\"inputs\":\"%s\",\"outputs\":\"%s\"",
        node.UA(), stateName(node.state()),
        node.enabled() ? "true" : "false",
        static_cast<unsigned>(inputLength),
        static_cast<unsigned>(outputLength),
        static_cast<unsigned>(stats.exchanges),
        static_cast<unsigned>(stats.noReplies),
        static_cast<unsigned>(stats.errors),
        static_cast<unsigned>(stats.recoveries),
        static_cast<unsigned>(node.consecutiveMisses()),
        static_cast<unsigned>(stats.lastTurnaroundMs), inputsHex, outputsHex);

    // The three stored axes, not just the scalar projection. `state` is
    // lossy by construction -- it answers "what is the single worst
    // thing about this node" -- so a reader that only ever saw the
    // projection could not tell DEGRADED-because-geometry from any
    // other fault, which is half of why #80 stayed invisible for so
    // long.
    // VALIDATION: Design v1.3 D16: liveness, image validity, and
    // conformance are separately stored axes; the scalar state is a
    // derived projection over them.
    written = appendf_(
        written,
        ",\"liveness\":\"%s\",\"imageState\":\"%s\",\"conformance\":\"%s\""
        ",\"observedIn\":%s",
        remoteNodeLivenessString(node.liveness()),
        remoteNodeImageStateString(node.imageState()),
        remoteNodeConformanceString(node.conformance()), observedIn);

    // The breaker and the lane it puts this node in (D17). Not derivable
    // from `state`: a tripped node reads MISCONFIGURED, and so does one
    // that has never conformed and is still being polled at full rate.
    // The difference is exactly what an operator needs -- one is costing
    // the bus, the other has been bounded -- and only these fields carry
    // it.
    // VALIDATION: Design v1.5 D17: the breaker's three positions and the
    // derived service class.
    written = appendf_(
        written,
        ",\"serviceClass\":\"%s\",\"breaker\":\"%s\""
        ",\"consecutiveNonconforming\":%u,\"breakerReinitAttempts\":%u",
        remoteNodeServiceClassString(node.serviceClass()),
        conformanceBreakerStateString(node.breakerState()),
        static_cast<unsigned>(node.consecutiveNonconforming()),
        static_cast<unsigned>(node.breakerReinitAttempts()));

    // Last-fault detail. Layer and attribution are rendered from the
    // classifiers rather than stored, so an analyzer gets the verdict
    // ("go fix configuration" vs "go fix firmware") without duplicating
    // the taxonomy (D14).
    written = appendf_(
        written,
        ",\"fault\":{\"name\":\"%s\",\"layer\":\"%s\",\"attribution\":\"%s\""
        ",\"expected\":%u,\"observed\":%u,\"atMs\":%lu}",
        conformanceFaultString(fault.fault),
        conformanceLayerString(layerOf(fault.fault)),
        faultAttributionString(attributionOf(fault.fault)),
        static_cast<unsigned>(fault.expected),
        static_cast<unsigned>(fault.observed),
        static_cast<unsigned long>(fault.atMs));
    if (extraKey != nullptr && extraValue != nullptr) {
      written = appendf_(written, ",\"%s\":\"%s\"", extraKey, extraValue);
    }
    // Raw JSON fragment (e.g. ",\"previousIn\":N,\"previousOut\":M"),
    // inserted verbatim. Distinct from extraKey/extraValue, which quote
    // their value -- numeric fields like NI/NO must not be quoted
    // (issue #91).
    if (extraJson != nullptr) {
      written = appendf_(written, "%s", extraJson);
    }
    finish_(written);
  }

  /// The live node table, keyed by UA. Per-mutation events (issue #91)
  /// now make membership changes visible as their own event lines; the
  /// roster remains the at-a-glance view of current membership that a
  /// status or mutation line carries alongside the event.
  int appendRoster_(int written) {
    written = appendf_(written, ",\"roster\":[");
    bool first = true;
    for (unsigned UA = 0; UA <= 127u; ++UA) {
      const RemoteNodeHandle* node = host_->node(static_cast<uint8_t>(UA));
      if (node == nullptr) {
        continue;
      }
      written = appendf_(
          written,
          "%s{\"ua\":%u,\"type\":\"%c\",\"in\":%u,\"out\":%u,\"state\":\"%s\",\"enabled\":%s}",
          first ? "" : ",", node->UA(),
          nodeTypeNdp(node->nodeType()),
          static_cast<unsigned>(node->inputLength()),
          static_cast<unsigned>(node->outputLength()),
          stateName(node->state()), node->enabled() ? "true" : "false");
      first = false;
    }
    return appendf_(written, "]");
  }

  // ------------------------------------------------ buffer plumbing

  /// Append a formatted fragment, clamping to the buffer. Truncation is
  /// guarded, not expected: kLineCapacity is sized for the worst case.
  int appendf_(int written, const char* format, ...)
      __attribute__((format(printf, 3, 4))) {
    const int limit = static_cast<int>(sizeof(line_)) - 1;
    if (written < 0 || written >= limit) {
      return limit;
    }
    va_list args;
    va_start(args, format);
    const int added = vsnprintf(line_ + written, sizeof(line_) - written,
                                format, args);
    va_end(args);
    if (added < 0) {
      return written;
    }
    written += added;
    return (written > limit) ? limit : written;
  }

  /// Close the object and hand the line to the writer.
  void finish_(int written) {
    if (written < 0) {
      written = 0;
    }
    snprintf(line_ + written, sizeof(line_) - written, "}");
    writeLine_(writeContext_, line_);
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
  const char* image_ = "";
  const char* version_ = "";
  LineWriter writeLine_ = nullptr;
  void* writeContext_ = nullptr;
  StatusExtender statusExtender_ = nullptr;
  void* statusExtenderContext_ = nullptr;

  uint32_t nowMs_ = 0;    ///< the main loop's injected clock
  uint32_t epochMs_ = 0;  ///< clock value at the epoch line; ts base
  uint32_t seq_ = 0;      ///< monotonic line sequence number
  bool backoffTraceOnly_ = false;
  char line_[kLineCapacity] = {0};
};

}  // namespace testbed
}  // namespace CMRInet
