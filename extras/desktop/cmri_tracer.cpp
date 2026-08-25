// cmri_tracer.cpp — the desktop Host tracer: CMRIHost over
// SerialCMRITransport over PosixCMRISerialPort, driven from a
// command-and-control loop on stdin/stdout.
//
// The C&C engine (verbs, JSON-lines telemetry, listener wiring) is
// the shared testbed engine in src/testbed/TracerShell.h — the
// same engine the Xiao Host R&D image wraps (map issue #21). This
// file is only the desktop main(): option parsing, termios port,
// POSIX clocks, and stdin/stdout plumbing.
//
// The scenario this binary runs is the stage-1/stage-2 testbed
// invariant: a scenario that passes here must pass unchanged against
// the Xiao Host R&D image.

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
#include "testbed/TracerShell.h"

namespace {

constexpr const char* kImage = "cmri_tracer";
// 0.3.0: I/T bench slice (map issue #30) — onTrace packet telemetry,
// output verbs (setbit/writeoutputs/forcetx), --output-bytes so T is
// exercisable, and the outputs hex field in every telemetry line.
// 0.2.0: ts is now integer ms since the epoch line (uniform across
// images, map issue #21); the epoch line carries the wall clock.
constexpr const char* kVersion = "0.3.0";

// ------------------------------------------------------------ CLI options

struct Options {
  const char* device = nullptr;
  uint32_t baud = 28800;
  uint8_t address = 30;        // wire UA = address + 65
  uint16_t inputBytes = 7;     // bench node: 2 phantom + 5 IOX IN bytes
  uint16_t outputBytes = 7;    // bench node: 2 phantom + 5 IOX OUT bytes
  uint32_t replyTimeoutMs = 0; // 0 = engine default (250 ms)
  uint32_t exchanges = 0;      // stop after N completed exchanges (0 = run on)
  uint32_t durationS = 0;      // stop after N seconds (0 = run on)

  // Conformance-strict mode: opt into the rate-derived inter-byte
  // abort (transport.rateDerivedInterByteTimeoutMs()) and the
  // rate-derived slow-gap thresholds, instead of the USB-deployment
  // defaults below. A scenario/tracer verb, not a deployment default
  // (Design v1.1 D13). When set, --inter-byte-timeout-ms and
  // --slow-gap-* are ignored.
  bool conformanceStrict = false;

  // Receive inter-byte timeout. The transport's rate-derived default
  // (3 char times, ~2 ms at 28800) measures WIRE gaps, but a USB
  // serial adapter delivers RX bytes in latency-timer chunks (FTDI
  // default: 16 ms), so arrival gaps are not wire gaps and the
  // decoder aborts healthy frames. Bench finding, 2026-08-15. 25 ms
  // rides above the chunking while still expiring genuine truncation
  // within the 250 ms reply gate. 0 disables the timeout entirely
  // (interop 2.2.6 conformance exception).
  uint32_t interByteTimeoutMs = 25;

  // Receive gap-observability thresholds (interop 2.2.6 grace band).
  // slowGapLoMs: the observation floor — gaps at/above this stamp the
  // maxGapMs watermark. slowGapHiMs: the slowGaps trigger — gaps
  // at/above this and below the abort limit increment slowGaps (the
  // "took longer than expected" annotation). USB serial adapters
  // deliver RX bytes in latency-timer chunks (FTDI default 16 ms), so
  // the char-derived default (lo=1, hi=2) would annotate every
  // between-chunk gap as slow. These USB-appropriate defaults keep the
  // grace band above the chunk size; a genuine stall still stamps
  // maxGapMs and aborts past the 25 ms limit. lo=0 disables
  // observability; hi<=lo leaves watermark-only mode.
  uint32_t slowGapLoMs = 1;
  uint32_t slowGapHiMs = 20;
};

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s --device <path> [--baud N] [--address N]\n"
          "          [--input-bytes N] [--output-bytes N] [--reply-timeout-ms N]\n"
          "          [--inter-byte-timeout-ms N]\n"
          "          [--slow-gap-lo-ms N] [--slow-gap-hi-ms N]\n"
          "          [--conformance-strict]\n"
          "          [--exchanges N] [--duration-s N]\n"
          "verbs on stdin (every node verb names its UA):\n"
          "  status | status <ua>\n"
          "  quiesce <ua> | resume <ua> | forcetx <ua>\n"
          "  setbit <ua> <bit> <0|1> | writeoutputs <ua> <hex>\n"
          "  node add <ua> <in> <out> | node delete <ua>\n"
          "  node geometry <ua> <in> <out>\n"
          "  node enable <ua> | node disable <ua>\n"
          "  quit\n",
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
    } else if (strcmp(arg, "--output-bytes") == 0 && hasValue) {
      opt.outputBytes = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--reply-timeout-ms") == 0 && hasValue) {
      opt.replyTimeoutMs =
          static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--inter-byte-timeout-ms") == 0 && hasValue) {
      opt.interByteTimeoutMs =
          static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--slow-gap-lo-ms") == 0 && hasValue) {
      opt.slowGapLoMs =
          static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--slow-gap-hi-ms") == 0 && hasValue) {
      opt.slowGapHiMs =
          static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(arg, "--conformance-strict") == 0) {
      opt.conformanceStrict = true;
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

/// LineWriter for the shared engine: one line to stdout, flushed so a
/// piped runner sees it immediately.
void writeStdoutLine(void* /*context*/, const char* line) {
  puts(line);
  fflush(stdout);
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
  if (opt.conformanceStrict) {
    // Conformance-strict: the rate-derived inter-byte abort (three
    // character times, interop 2.2.6) and rate-derived slow-gap
    // thresholds. This is the conformance instrument, not the
    // deployment mode (Design v1.1 D13); the USB-appropriate defaults
    // below do not apply. Skip setSlowGapThresholdsMs so begin()
    // derives lo (1 char time) and hi (3 char times) from the port.
    transport.setInterByteTimeoutMs(transport.rateDerivedInterByteTimeoutMs());
  } else {
    // Deployment mode: overrides survive begin() (see Options — USB
    // chunking is not wire silence).
    transport.setInterByteTimeoutMs(opt.interByteTimeoutMs);
    transport.setSlowGapThresholdsMs(opt.slowGapLoMs, opt.slowGapHiMs);
  }

  CMRInet::CMRIHostConfig config;
  if (opt.replyTimeoutMs != 0) {
    config.replyTimeoutMs = opt.replyTimeoutMs;
  }
  CMRInet::CMRIHost host(transport, config);

  CMRInet::RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = opt.inputBytes;
  nodeConfig.outputBytes = opt.outputBytes;
  // The add reports its own outcome (Design v1.2 D5), so bail before
  // binding rather than inspecting a deferred host-wide status.
  const CMRInet::CMRIHost::ConfigStatus configStatus =
      host.addRemoteNode(opt.address, nodeConfig);
  if (configStatus != CMRInet::CMRIHost::ConfigStatus::kOk) {
    fprintf(stderr, "error: addRemoteNode rejected the configuration: %s\n",
            CMRInet::configStatusString(configStatus));
    return 2;
  }
  CMRInet::testbed::TracerShell engine;
  engine.bind(host, transport, kImage, kVersion, writeStdoutLine, nullptr);

  host.begin();
  if (!port.isOpen()) {
    fprintf(stderr, "error: %s: %s\n", opt.device, port.lastError());
    return 1;
  }

  // The epoch marker: seq and ts restart here, so a runner seeing
  // this line knows every cumulative counter restarted with it. The
  // desktop's absolute anchor is the wall clock.
  engine.setNow(monotonicMs());
  char wallClock[40];
  isoTimestamp(wallClock, sizeof(wallClock));
  engine.emitEpoch("wallClock", wallClock);

  const uint32_t startMs = monotonicMs();
  char verb[128];
  while (!gStop) {
    const uint32_t nowMs = monotonicMs();
    engine.setNow(nowMs);
    host.tick(nowMs);

    if (readVerb(verb, sizeof(verb))) {
      using VerbResult = CMRInet::testbed::TracerShell::VerbResult;
      if (engine.handleVerb(verb) == VerbResult::kQuit) {
        break;
      }
    }

    // Resolved every pass rather than cached: `node delete <ua>` is a
    // verb now, so the handle can legitimately go away underneath us
    // (Design v1.2 D5). A deleted node ends the run -- there is nothing
    // left to count exchanges against.
    const CMRInet::RemoteNodeHandle* node = host.node(opt.address);
    if (node == nullptr) {
      break;
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

  engine.setNow(monotonicMs());
  engine.emitLine("final");
  port.close();
  return 0;
}
