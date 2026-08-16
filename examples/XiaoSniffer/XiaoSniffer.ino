// XiaoSniffer.ino — passive RS-485 bus sniffer for the CMRInet testbed.
//
// A spare cpNode-Xiao board wires its R± to one bus pair (T± parked) and
// logs every decoded CMRInet frame over USB CDC as a JSON line. It never
// drives the bus: no Host, no transport, no TXEN assertion — just the
// standalone CMRIFrameDecoder fed byte-at-a-time from Serial1. The
// decoder is the exact code the Host transport uses, so a frame this
// sniffer decodes is a frame the wire actually carried.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491), same pinout as a
// node or Host — the inversion is entirely in what you wire:
//   D7 - RX   CMRI RS485 receive (the pair under observation)
//   D6 - TX   UART TX, but the driver is off (TXEN low) so it goes nowhere
//   D3 - TXEN held LOW — driver output high-Z, listen-only
//
// One board = one pair. The MAX3491 receiver hears only the pair wired
// to R±, so a complete poll+reply conversation needs two sniffers (one
// per pair) or a 2-wire bench. This sniffer logs whichever pair it is
// on; it is direction-blind and reports "observed" — the MT still
// identifies the frame (I/T/P are Host->Node, R is Node->Host).
//
// Output (one JSON line each):
//   epoch  {"seq":N,"ts":0,"event":"epoch","image":"xiao_sniffer",
//          "version":"...","anchor":"bootMs","value":N}
//   frame  {"seq":N,"ts":N,"event":"frame","image":"xiao_sniffer",
//          "version":"...","ua":N,"mt":"C","body":"HEX"}
//   stats  {"seq":N,"ts":N,"event":"stats",...decoder counters...}
// ts is integer ms since boot (the epoch anchor), matching the tracer
// engine's relative-clock convention so a runner can diff the two
// streams without special-casing the clock. stats lines emit every
// SNIFFER_STATS_INTERVAL_MS so decoder health (restarts, aborts,
// slowGaps) is visible even on a quiet bus.
//
// VALIDATION: map issue #30 — the passive tap witness for I/T/P frames
// (acceptance #2/#3). Born with the I/T bench slice; a reusable testbed
// asset the software notes point at for the adversarial and
// host-conformance use cases too.

#include <Arduino.h>

#include "CMRIFrameCodec.h"
#include "CMRIPacket.h"

#ifndef SNIFFER_BAUD
#define SNIFFER_BAUD 28800
#endif
#ifndef SNIFFER_INTER_BYTE_TIMEOUT_MS
// Tolerate the ESP32-C6 ~2 s runtime stall (arrival gaps != wire gaps);
// the decoder measures gaps at tick granularity. Same doctrine as the
// Xiao Host tracer (#21 finding 2).
#define SNIFFER_INTER_BYTE_TIMEOUT_MS 50
#endif
#ifndef SNIFFER_STATS_INTERVAL_MS
#define SNIFFER_STATS_INTERVAL_MS 5000
#endif

namespace {

constexpr const char* kImage = "xiao_sniffer";
constexpr const char* kVersion = "0.1.0";
constexpr int kTxenPin = D3;  // held LOW: driver off, listen-only

CMRInet::CMRIFrameDecoder decoder;

uint32_t seq = 0;
uint32_t epochMs = 0;
uint32_t lastStatsMs = 0;

// body hex (2 * kMaxBody) + ~120 chars of fixed fields. Guarded, not
// expected to truncate for bench geometries.
char line[2 * CMRInet::kMaxBody + 160];

void emitLine() {
  Serial.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  Serial.write('\n');
}

void emitEpoch() {
  epochMs = millis();
  char bootMs[16];
  snprintf(bootMs, sizeof(bootMs), "%lu",
           static_cast<unsigned long>(epochMs));
  snprintf(line, sizeof(line),
           "{\"seq\":%u,\"ts\":0,\"event\":\"epoch\",\"image\":\"%s\","
           "\"version\":\"%s\",\"anchor\":\"bootMs\",\"value\":%s}",
           ++seq, kImage, kVersion, bootMs);
  emitLine();
}

void emitFrame(const CMRInet::CMRIPacket& p) {
  char bodyHex[2 * CMRInet::kMaxBody + 1] = "";
  for (size_t i = 0; i < p.length; ++i) {
    snprintf(&bodyHex[2 * i], 3, "%02X", p.body[i]);
  }
  // Printable MT passes through (I/P/T/R and the JMRI extensions are all
  // printable); a non-printable byte becomes '.' so the JSON stays valid.
  const char mc = (p.mt >= 0x20 && p.mt <= 0x7E) ? static_cast<char>(p.mt)
                                                 : '.';
  char mtBuf[2] = {mc, '\0'};
  snprintf(line, sizeof(line),
           "{\"seq\":%u,\"ts\":%u,\"event\":\"frame\",\"image\":\"%s\","
           "\"version\":\"%s\",\"dir\":\"observed\",\"ua\":%u,"
           "\"mt\":\"%s\",\"body\":\"%s\"}",
           ++seq, static_cast<unsigned>(millis() - epochMs), kImage, kVersion,
           static_cast<unsigned>(p.ua), mtBuf, bodyHex);
  emitLine();
}

void emitStats() {
  const CMRInet::CMRIFrameDecoder::Statistics& s = decoder.statistics();
  snprintf(line, sizeof(line),
           "{\"seq\":%u,\"ts\":%u,\"event\":\"stats\",\"image\":\"%s\","
           "\"version\":\"%s\",\"framesDecoded\":%u,\"framesRestarted\":%u,"
           "\"timeoutAborts\":%u,\"danglingDle\":%u,\"overflowAborts\":%u,"
           "\"headerAborts\":%u,\"droppedPackets\":%u,\"slowGaps\":%u,"
           "\"maxGapMs\":%u}",
           ++seq, static_cast<unsigned>(millis() - epochMs), kImage, kVersion,
           s.framesDecoded, s.framesRestarted, s.timeoutAborts, s.danglingDle,
           s.overflowAborts, s.headerAborts, s.droppedPackets, s.slowGaps,
           s.maxGapMs);
  emitLine();
}

}  // namespace

void setup() {
  Serial.begin(115200);  // USB CDC: the frame log stream
  // R&D image: wait for the C&C stream so the epoch line is captured.
  while (!Serial) {
    delay(10);
  }

  // Observe the CMRI wire: 28800 8N2 on the MAX3491 UART RX pin. The TX
  // pin is configured only because Serial1.begin wants one; with TXEN
  // low the driver output is high-Z and D6 drives nothing on the bus.
  Serial1.begin(SNIFFER_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  pinMode(kTxenPin, OUTPUT);
  digitalWrite(kTxenPin, LOW);  // driver off — listen-only

  decoder.setInterByteTimeoutMs(SNIFFER_INTER_BYTE_TIMEOUT_MS);
  // Rate-derived slow-gap observability for 28800 8N2 (~385 us/char):
  // lo = 1 ms (streaming floor), hi = 2 ms (suspicion floor). The raised
  // 50 ms abort keeps the slow band open so a genuine stall is annotated
  // rather than mistaken for the C6 runtime stall.
  decoder.setSlowGapThresholdsMs(1, 2);

  emitEpoch();
  lastStatsMs = millis();
}

void loop() {
  const uint32_t now = millis();
  while (Serial1.available() > 0) {
    const uint8_t b = static_cast<uint8_t>(Serial1.read());
    if (decoder.feed(b, now)) {
      CMRInet::CMRIPacket p;
      while (decoder.take(p)) {
        emitFrame(p);
      }
    }
  }
  // Abandon a stale partial frame when the bus goes quiet (interop
  // 2.2.6) so a truncated frame cannot hold the decoder mid-frame.
  decoder.expireIdle(now);

  if (now - lastStatsMs >= SNIFFER_STATS_INTERVAL_MS) {
    emitStats();
    lastStatsMs = now;
  }
}
