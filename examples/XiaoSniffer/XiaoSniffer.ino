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
//   D4 - SDA  I2C for the OLED status panel (optional)
//   D5 - SCL  I2C for the OLED status panel (optional)
//
// One board = one pair. The MAX3491 receiver hears only the pair wired
// to R±, so a complete poll+reply conversation needs two sniffers (one
// per pair) or a 2-wire bench. This sniffer logs whichever pair it is
// on; it is direction-blind and reports "observed" — the MT still
// identifies the frame (I/T/P are Host->Node, R is Node->Host).
//
// OLED (SSD1306 128x64 @ 0x3C): a big "SNIFFER" header confirms at a
// glance the new firmware is actually running (not a stale node image),
// with a live packet count and per-MT tally. Degrades gracefully: if
// the display is absent, the JSON CDC stream still works.
//
// Output (one JSON line each over USB CDC):
//   epoch  {"seq":N,"ts":0,"event":"epoch","image":"xiao_sniffer",...}
//   frame  {"seq":N,"ts":N,"event":"frame",...,"ua":N,"mt":"C","body":"HEX"}
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
#include <Wire.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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
#ifndef SNIFFER_DISPLAY_INTERVAL_MS
#define SNIFFER_DISPLAY_INTERVAL_MS 150
#endif
#ifndef SNIFFER_USE_OLED
#define SNIFFER_USE_OLED 1  // set to 0 to compile out the display
#endif

namespace {

constexpr const char* kImage = "xiao_sniffer";
constexpr const char* kVersion = "0.2.1";
constexpr int kTxenPin = D3;  // held LOW: driver off, listen-only

CMRInet::CMRIFrameDecoder decoder;

uint32_t seq = 0;
uint32_t epochMs = 0;
uint32_t lastStatsMs = 0;
uint32_t lastDisplayMs = 0;

// Per-MT tally for the OLED live view (I/T/P/R + other).
struct Tally {
  uint32_t total = 0;
  uint32_t i = 0, t = 0, p = 0, r = 0, other = 0;
  uint8_t lastUa = 0;
  char lastMt = '-';
};
Tally tally;

#if SNIFFER_USE_OLED
constexpr int kScreenW = 128;
constexpr int kScreenH = 64;
constexpr int kScreenAddr = 0x3C;
Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
#endif

// body hex (2 * kMaxBody) + ~120 chars of fixed fields. Guarded, not
// expected to truncate for bench geometries.
char line[2 * CMRInet::kMaxBody + 160];

void emitLine() {
  // Skip the blocking CDC write when no host has the port open. After
  // the bounded 3 s wait in setup(), `Serial` stays false on a headless
  // board, so without this gate the first write blocks forever (the TX
  // buffer fills and nobody drains it) and loop()/drawStatus() never
  // run. Live check: if a terminal attaches later, writes resume.
  if (!Serial) return;
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

void countFrame(const CMRInet::CMRIPacket& p) {
  ++tally.total;
  tally.lastUa = p.ua;
  tally.lastMt = (p.mt >= 0x20 && p.mt <= 0x7E) ? static_cast<char>(p.mt)
                                                 : '?';
  switch (p.mt) {
    case 'I': ++tally.i; break;
    case 'T': ++tally.t; break;
    case 'P': ++tally.p; break;
    case 'R': ++tally.r; break;
    default: ++tally.other; break;
  }
}

void emitFrame(const CMRInet::CMRIPacket& p) {
  countFrame(p);
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

#if SNIFFER_USE_OLED
void drawSplash() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("SNIFFER"));
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print(kVersion);
  display.setCursor(0, 40);
  display.print(F("listening..."));
  display.display();
}

void drawStatus() {
  if (!oledOk) return;
  const CMRInet::CMRIFrameDecoder::Statistics& s = decoder.statistics();
  display.clearDisplay();
  // Header — big so it's obvious this is the sniffer, not a node.
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("SNIFFER"));
  // Counts — fixed layout: every line stays within the 21-char (128px)
  // width at size 1, so a growing counter can never wrap and garble the
  // row below. setTextWrap(false) (set in setup) makes any future
  // overflow clip instead of clobber. Each counter is printed in a
  // fixed-width, right-aligned field (%Nu) so the labels and the second
  // counter on each line stay pinned as digit counts change — no layout
  // shift when a value grows by a digit.
  //   frames: %10u      total, up to 10-digit uint32   (= 18 chars)
  //   P:%8u R:%8u       fastest MTs, 8 digits each     (= 21 chars)
  //   T:%8u I:%4u       common T (8) + rare I (4)      (= 17 chars)
  //   abort:%4u rst:%4u rare errors, 4 digits each     (= 19 chars)
  //   last: %c ua=%3u   glance line — ua is uint8      (<=14 chars)
  display.setTextSize(1);
  display.setCursor(0, 18);
  display.printf(F("frames: %10u"), tally.total);
  display.setCursor(0, 27);
  display.printf(F("P:%8u R:%8u"), tally.p, tally.r);
  display.setCursor(0, 36);
  display.printf(F("T:%8u I:%4u"), tally.t, tally.i);
  display.setCursor(0, 45);
  display.printf(F("abort:%4u rst:%4u"),
                 static_cast<unsigned>(s.timeoutAborts),
                 static_cast<unsigned>(s.framesRestarted));
  display.setCursor(0, 54);
  if (tally.total != 0) {
    display.printf(F("last: %c ua=%3u"), tally.lastMt,
                   static_cast<unsigned>(tally.lastUa));
  } else {
    display.print(F("last: -"));
  }
  display.display();
}
#endif

}  // namespace

void setup() {
  Serial.begin(115200);  // USB CDC: the frame log stream
  // R&D image: wait for the CDC host to open the port so the epoch
  // line is captured — but bound the wait at 3 s so a headless board
  // (no terminal attached) still boots and drives the OLED without
  // hanging setup(). On the Xiao ESP32-C6, `Serial` is native USB CDC
  // and only reads true once a host asserts DTR/RTS; without a bound
  // the wait parks here forever and loop()/drawStatus() never run.
  const uint32_t kSerialWaitMs = 3000;
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < kSerialWaitMs) {
    delay(10);
  }

#if SNIFFER_USE_OLED
  // SSD1306 at 0x3C on the board I2C (D4/D5). Degrade gracefully: a
  // missing display does not stop the JSON stream.
  if (display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr)) {
    oledOk = true;
    display.dim(true);
    display.setTextWrap(false);  // dashboard: clip, never wrap-clobber a row
    drawSplash();
  } else {
    oledOk = false;
  }
#endif

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
  lastDisplayMs = millis();
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

#if SNIFFER_USE_OLED
  if (now - lastDisplayMs >= SNIFFER_DISPLAY_INTERVAL_MS) {
    drawStatus();
    lastDisplayMs = now;
  }
#endif
}
