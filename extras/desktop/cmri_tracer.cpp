// cmri_tracer.cpp — the desktop Host tracer: CMRIHost over
// SerialCMRITransport over PosixCMRISerialPort, driven from a
// command-and-control loop on stdin/stdout.
//
// Command-and-control shape per docs/testbed-software-notes.md:
// plain verbs in (quiesce / resume / status / quit), JSON lines out.
// Stream rules: cumulative counters only, a monotonic sequence
// number on every line, and an explicit epoch marker at start, so a
// runner detects process restarts.
//
// The scenario this binary runs is the stage-1/stage-2 testbed
// invariant: a scenario that passes here must pass unchanged against
// the Xiao Host R&D image (map issue #7).

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "CMRIHost.h"
#include "PosixCMRISerialPort.h"
#include "SerialCMRITransport.h"

namespace {

constexpr const char* kImage = "cmri_tracer";
constexpr const char* kVersion = "0.1.0";

// ------------------------------------------------------------ CLI options

struct Options {
  const char* device = nullptr;
  uint32_t baud = 28800;
  uint8_t address = 30;        // wire UA = address + 65
  uint16_t inputBytes = 7;     // bench node: 2 phantom + 5 IOX IN bytes
  uint32_t replyTimeoutMs = 0; // 0 = engine default (250 ms)
  uint32_t exchanges = 0;      // stop after N completed exchanges (0 = run on)
  uint32_t durationS = 0;      // stop after N seconds (0 = run on)

  // Receive inter-byte timeout. The transport's rate-derived default
  // (3 char times, ~2 ms at 28800) measures WIRE gaps, but a USB
  // serial adapter delivers RX bytes in latency-timer chunks (FTDI
  // default: 16 ms), so arrival gaps are not wire gaps and the
  // decoder aborts healthy frames. Bench finding, 2026-08-15. 25 ms
  // rides above the chunking while still expiring genuine truncation
  // within the 250 ms reply gate. 0 disables the timeout entirely
  // (interop 2.2.6 conformance exception).
  uint32_t interByteTimeoutMs = 25;
};

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s --device <path> [--baud N] [--address N]\n"
          "          [--input-bytes N] [--reply-timeout-ms N]\n"
          "          [--inter-byte-timeout-ms N] [--exchanges N]\n"
          "          [--duration-s N]\n"
          "verbs on stdin: quiesce | resume | status | quit\n",
          argv0);
}

bool parseOptions(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    const bool hasValue = (i + 1 < argc);
    if (strcmp(arg, "--device") == 0 && hasValue) {
      opt.device = argv[++i];
    } else if (strcmp(arg, "--baud") == 0 && hasValue) {
      opt.baud = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--address") == 0 && hasValue) {
      const unsigned long v = strtoul(argv[++i], nullptr, 10);
      if (v > 127) {
        fprintf(stderr, "error: --address must be 0..127\n");
        return false;
      }
      opt.address = static_cast<uint8_t>(v);
    } else if (strcmp(arg, "--input-bytes") == 0 && hasValue) {
      opt.inputBytes = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--reply-timeout-ms") == 0 && hasValue) {
      opt.replyTimeoutMs =
          static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--inter-byte-timeout-ms") == 0 && hasValue) {
      opt.interByteTimeoutMs =
          static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--exchanges") == 0 && hasValue) {
      opt.exchanges = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--duration-s") == 0 && hasValue) {
      opt.durationS = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else {
      fprintf(stderr, "error: unknown or incomplete argument '%s'\n", arg);
      return false;
    }
  }
  if (opt.device == nullptr) {
    fprintf(stderr, "error: --device is required\n");
    return false;
  }
  return true;
}

// ------------------------------------------------------------ time helpers

/// Milliseconds on the monotonic clock (the engine's injected time).
uint32_t monotonicMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint32_t>(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/// Wall-clock timestamp, ISO 8601 UTC, for the human reading the log.
void isoTimestamp(char* out, size_t len) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmUtc;
  gmtime_r(&ts.tv_sec, &tmUtc);
  const long millis = ts.tv_nsec / 1000000L;
  snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
           tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
           tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec, millis);
}

// ------------------------------------------------------------ telemetry

const char* stateName(CMRInet::RemoteNodeState state) {
  switch (state) {
    case CMRInet::RemoteNodeState::kUninitialized: return "UNINITIALIZED";
    case CMRInet::RemoteNodeState::kOnline: return "ONLINE";
    case CMRInet::RemoteNodeState::kStale: return "STALE";
    case CMRInet::RemoteNodeState::kOffline: return "OFFLINE";
  }
  return "UNKNOWN";
}

const char* eventName(CMRInet::CMRIHostEventType type) {
  switch (type) {
    case CMRInet::CMRIHostEventType::kReplyAccepted: return "reply";
    case CMRInet::CMRIHostEventType::kReplyRejected: return "reject";
    case CMRInet::CMRIHostEventType::kReplyTimeout: return "miss";
    case CMRInet::CMRIHostEventType::kNodeStateChanged: return "state";
  }
  return "unknown";
}

/// Everything one telemetry line needs, bundled as the listener
/// context cookie.
struct Tracer {
  CMRInet::CMRIHost* host = nullptr;
  CMRInet::SerialCMRITransport* transport = nullptr;
  const CMRInet::RemoteNodeHandle* node = nullptr;
  uint32_t seq = 0;       // monotonic line sequence number
  bool quiesced = false;  // bus handed off; polling suspended

  /// Emit one JSON telemetry line. `event` names why the line exists;
  /// extraKey/extraValue append one event-specific string field.
  void emitLine(const char* event, const char* extraKey = nullptr,
                const char* extraValue = nullptr) {
    char ts[40];
    isoTimestamp(ts, sizeof(ts));

    char inputsHex[2 * CMRINET_HOST_MAX_INPUT_BYTES + 1] = "";
    const size_t inputLength = node->inputLength();
    for (size_t i = 0; i < inputLength; ++i) {
      snprintf(&inputsHex[2 * i], 3, "%02X", node->inputByte(i));
    }

    const CMRInet::CMRIHostStatistics& host_ = host->statistics();
    const CMRInet::RemoteNodeStatistics& node_ = node->statistics();
    const CMRInet::LinkStatistics& link = transport->stats();

    printf("{\"seq\":%u,\"ts\":\"%s\",\"event\":\"%s\","
           "\"role\":\"host\",\"image\":\"%s\",\"version\":\"%s\","
           "\"address\":%u,\"ua\":%u,\"state\":\"%s\",\"quiesced\":%s,"
           "\"polls\":%u,\"pollRetries\":%u,\"replies\":%u,"
           "\"misses\":%u,\"rejected\":%u,\"unsolicited\":%u,"
           "\"exchanges\":%u,\"errors\":%u,\"recoveries\":%u,"
           "\"consecutiveMisses\":%u,\"lastTurnaroundMs\":%u,"
           "\"decodeErrors\":%u,\"inputs\":\"%s\"",
           ++seq, ts, event, kImage, kVersion, node->address(), node->ua(),
           stateName(node->state()), quiesced ? "true" : "false",
           host_.pollsSent, host_.pollSendRetries, host_.repliesAccepted,
           node_.noReplies, host_.repliesRejected, host_.unsolicitedPackets,
           node_.exchanges, node_.errors, node_.recoveries,
           node_.consecutiveMisses, node_.lastTurnaroundMs, link.decodeErrors,
           inputsHex);
    if (extraKey != nullptr && extraValue != nullptr) {
      printf(",\"%s\":\"%s\"", extraKey, extraValue);
    }
    printf("}\n");
    fflush(stdout);
  }
};

/// CMRIHost event listener: one telemetry line per engine event.
void onHostEvent(void* context, const CMRInet::CMRIHostEvent& event) {
  Tracer& tracer = *static_cast<Tracer*>(context);
  if (event.type == CMRInet::CMRIHostEventType::kNodeStateChanged) {
    tracer.emitLine(eventName(event.type), "previousState",
                    stateName(event.previousState));
    return;
  }
  tracer.emitLine(eventName(event.type));
}

// ------------------------------------------------------------ verbs

volatile sig_atomic_t gStop = 0;
void handleSignal(int) { gStop = 1; }

/// Read one newline-terminated verb from non-blocking stdin.
/// Returns false when no complete line is waiting.
bool readVerb(char* out, size_t len) {
  static char buffer[128];
  static size_t used = 0;
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1) {
    if (c == '\n') {
      buffer[used] = '\0';
      snprintf(out, len, "%s", buffer);
      used = 0;
      return true;
    }
    if (used < sizeof(buffer) - 1) {
      buffer[used++] = c;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseOptions(argc, argv, opt)) {
    usage(argv[0]);
    return 2;
  }

  signal(SIGINT, handleSignal);
  signal(SIGTERM, handleSignal);
  fcntl(STDIN_FILENO, F_SETFL,
        fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);

  CMRInet::PosixCMRISerialPort port(opt.device, opt.baud, /*stopBits2=*/true);
  CMRInet::SerialCMRITransport transport(port);
  // Override survives begin() (see Options: USB chunking is not wire
  // silence).
  transport.setInterByteTimeoutMs(opt.interByteTimeoutMs);

  CMRInet::CMRIHostConfig config;
  if (opt.replyTimeoutMs != 0) {
    config.replyTimeoutMs = opt.replyTimeoutMs;
  }
  CMRInet::CMRIHost host(transport, config);

  CMRInet::RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = opt.inputBytes;
  CMRInet::RemoteNodeHandle* node = host.addRemoteNode(opt.address, nodeConfig);
  if (node == nullptr) {
    fprintf(stderr, "error: addRemoteNode rejected the configuration\n");
    return 2;
  }

  Tracer tracer;
  tracer.host = &host;
  tracer.transport = &transport;
  tracer.node = node;
  host.onEvent(onHostEvent, &tracer);

  host.begin();
  if (!port.isOpen()) {
    fprintf(stderr, "error: %s: %s\n", opt.device, port.lastError());
    return 1;
  }

  // The epoch marker: seq restarts here, so a runner seeing this line
  // knows every cumulative counter restarted with it.
  tracer.emitLine("epoch");

  const uint32_t startMs = monotonicMs();
  char verb[128];
  while (!gStop) {
    host.tick(monotonicMs());

    if (readVerb(verb, sizeof(verb))) {
      if (strcmp(verb, "quit") == 0) {
        break;
      } else if (strcmp(verb, "status") == 0) {
        tracer.emitLine("status");
      } else if (strcmp(verb, "quiesce") == 0) {
        node->setEnabled(false);
        tracer.quiesced = true;
        tracer.emitLine("quiesce");
      } else if (strcmp(verb, "resume") == 0) {
        node->setEnabled(true);
        tracer.quiesced = false;
        tracer.emitLine("resume");
      } else if (verb[0] != '\0') {
        tracer.emitLine("error", "unknownVerb", verb);
      }
    }

    const CMRInet::RemoteNodeStatistics& s = node->statistics();
    if (opt.exchanges != 0 &&
        s.exchanges + s.noReplies + s.errors >= opt.exchanges) {
      break;
    }
    if (opt.durationS != 0 &&
        monotonicMs() - startMs >= opt.durationS * 1000u) {
      break;
    }

    usleep(500);  // 0.5 ms: fine-grained against the 5 ms pacing gate
  }

  tracer.emitLine("final");
  port.close();
  return 0;
}
