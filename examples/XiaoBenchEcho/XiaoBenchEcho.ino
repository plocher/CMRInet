// XiaoBenchEcho.ino — 2-wire self-echo timing probe (library-free).
//
// Purpose: measure the RS485 self-echo on a 2-wire (single-pair,
// full-duplex-jumpered) bus WITHOUT the CMRInet transport, codec, or
// host engine, so the echo's real latency, duration, and placement
// relative to the TXEN assert/deassert edges are OBSERVED FACTS — not
// predictions of the echo-cancel code under test in issue #104 / PR #106.
//
// Method (one burst per loop() iteration):
//   1. assert TXEN (D3)            -> stamp "assert"
//   2. gapless-write the marker    -> stamp "tx" (UART queue time)
//   3. tight-poll RX bytes AND poll uart_wait_tx_done(0) for the
//      shift-register drain edge, non-blockingly, so RX arrival stamps
//      stay tight. When the drain edge fires: stamp "drained",
//      deassert TXEN, stamp "deassert".
//   4. keep polling RX for a fixed window -> catches any echo tail
//      or stray/garbage byte that arrives AFTER TXEN deassert.
//   5. read the UART break/framing/parity raw flags and stamp "err".
//   6. print the collected timeline to USB CDC AFTER the burst, so
//      CDC writes do not perturb the timing under measurement.
//
// TXEN-deassertion correlation test: send a SINGLE byte
// (marker = one byte) so the deassert edge is the only event under
// study, isolating it from multi-byte traffic. A stray NULL (0x00) or a
// latched UART break/framing flag after deassert is the artifact.
//
// Runtime marker (CDC): the default 4-byte marker can be changed over
// USB CDC without reflashing:
//   marker 5A                 -> single-byte deassertion test
//   marker AA 55 AA 55        -> restore the 4-byte default
//   ?                         -> emit epoch (identity + current marker)
// Each command emits a fresh epoch and skips that burst, so the gather
// script can request identity or change the marker without a race.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS485 transmit enable
//
// STATUS: bench probe. No CMRInet library code is used. The marker and
// timing windows are build-overridable below. All timestamps are
// micros() — at 28800 8N2 one char time is ~347us, well above the
// tight-poll resolution of this core, so RX arrivals are byte-accurate.

#include <Arduino.h>
#include <driver/uart.h>

// OLED: identifies this sketch on the bench display as the echo probe
// ("ECHO", distinct from the tracer "TRC", sniffer "SNIFFER", and cal
// host "CAL"), and shows the operator live per-burst echo progress.
// Adafruit only — no CMRInet library code is pulled in, so the timing
// measurement stays isolated from the code under test (#104 / PR #106).
//
// TIMING NOTE: display.display() blocks ~25ms pumping 1024 bytes over
// I2C. It is NEVER called inside the timed burst window (the tight RX/
// drain poll). All OLED draws happen AFTER the burst timeline is emitted
// to CDC, in the trailing summary block, throttled to kDisplayRefreshMs.
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// UART hardware error flags (break / framing / parity). The raw
// interrupt bits latch when the condition is detected, independent of
// interrupt-enable. Read after each burst to correlate a stray byte
// with a hardware BREAK/framing error rather than guessing.
#if defined(ARDUINO_ARCH_ESP32)
#include <soc/uart_reg.h>
#endif

constexpr int        kTxenPin = D3;
constexpr uint32_t   kBaud    = 28800;
constexpr uart_port_t kUart   = UART_NUM_1;

// Default marker; overridable at runtime via the `marker` CDC command.
#ifndef ECHO_DEFAULT_MARKER
#define ECHO_DEFAULT_MARKER 0xAA, 0x55, 0xAA, 0x55
#endif
constexpr uint8_t  kDefaultMarker[] = { ECHO_DEFAULT_MARKER };
constexpr size_t   kMaxMarkerLen = 16;
uint8_t  gMarker[kMaxMarkerLen];
size_t   gMarkerLen = 0;

#ifndef ECHO_CAPTURE_US
#define ECHO_CAPTURE_US 5000   // RX capture window from assert (us)
#endif
#ifndef ECHO_GAP_MS
#define ECHO_GAP_MS 500        // idle gap between bursts (ms)
#endif

constexpr const char* kImage   = "xiao_bench_echo";
constexpr const char* kVersion = "0.3.0";

// Burst mode: normal = write the marker; deonly = assert/deassert TXEN
// with NO bytes written to the UART, so the transceiver's DE transition
// is the only event on the pair. If a stray 0x00 still appears in
// deonly mode, the artifact is purely the DE/receiver-enable path; if it
// vanishes, the stray needs real TX content to manifest.
enum class Mode : uint8_t { kNormal, kDeOnly };
Mode gMode = Mode::kNormal;

// OLED (cpNode-Xiao SSD1306 on D4/D5 I2C). Degrades to headless on failure.
constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;
Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;

// Last burst's echo summary, shown on the status panel. Computed AFTER
// the timeline is emitted, so the OLED draw (which blocks ~25ms) never
// lands in the timed window. Not the measurement path.
size_t   lastEchoBytes_ = 0;
bool     lastEchoFull_  = false;
uint32_t lastLatUs_     = 0;
uint32_t lastDurUs_     = 0;
size_t   lastStray_     = 0;
bool     lastBrk_       = false;
bool     lastFrm_       = false;
bool     lastPar_       = false;

void drawSplash();
void drawStatus();

// Per-burst event buffer. Printed after the burst so CDC writes never
// land inside the timed window. Capacity covers TX/RX/err events;
// overflow drops silently (the analyzer works on what remains).
struct Ev {
  const char* tag;
  uint32_t us;
  int v;            // byte value, or -1
};
constexpr size_t kEvCap = 48;
Ev evs_[kEvCap];
size_t evCount_ = 0;

static void rec(const char* tag, int v = -1) {
  if (evCount_ < kEvCap) {
    evs_[evCount_].tag = tag;
    evs_[evCount_].us  = micros();
    evs_[evCount_].v   = v;
    ++evCount_;
  }
}

static void clearUartErrFlags() {
#if defined(ARDUINO_ARCH_ESP32)
  REG_WRITE(UART_INT_CLR_REG(kUart),
            UART_BRK_DET_INT_RAW | UART_FRM_ERR_INT_RAW | UART_PARITY_ERR_INT_RAW);
#endif
}

// Return a packed error mask: bit0=brk, bit1=frm, bit2=par.
static uint8_t readUartErrMask() {
#if defined(ARDUINO_ARCH_ESP32)
  const uint32_t raw = REG_READ(UART_INT_RAW_REG(kUart));
  uint8_t m = 0;
  if (raw & UART_BRK_DET_INT_RAW)    m |= 0x1;
  if (raw & UART_FRM_ERR_INT_RAW)    m |= 0x2;
  if (raw & UART_PARITY_ERR_INT_RAW) m |= 0x4;
  return m;
#else
  return 0;
#endif
}

static void emitEpoch() {
  Serial.print("{\"e\":\"epoch\",\"image\":\"");
  Serial.print(kImage);
  Serial.print("\",\"version\":\"");
  Serial.print(kVersion);
  Serial.print("\",\"mode\":\"");
  Serial.print(gMode == Mode::kDeOnly ? "deonly" : "normal");
  Serial.print("\",\"marker\":[");
  for (size_t i = 0; i < gMarkerLen; ++i) {
    if (i) Serial.print(',');
    Serial.print(gMarker[i]);
  }
  Serial.println("]}");
}

static void flushRx() {
  while (Serial1.available()) (void)Serial1.read();
}

uint32_t burstN_ = 0;

void drawSplash() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("ECHO"));
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.print(kVersion);
  display.setCursor(0, 40);
  display.print(F("2-wire probe"));
  display.display();
}

// drawStatus ends with display.display(), which blocks ~25ms (1024-byte
// I2C push). Only ever called from the post-burst summary block — never
// from inside the timed tight-poll window.
void drawStatus() {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("ECHO"));
  display.setTextSize(1);
  display.setCursor(60, 4);
  display.print(F("n="));
  display.print(burstN_);
  if (gMode == Mode::kDeOnly) {
    // deonly: no marker is sent, so the marker/echo rows are meaningless.
    // Show the mode and the stray/error result instead.
    display.setCursor(0, 20);
    display.print(F("DE-ONLY"));
    display.setCursor(0, 32);
    display.print(F("no TX bytes"));
    display.setCursor(0, 44);
    display.print(F("rx "));
    display.print(lastEchoBytes_ + lastStray_);
    if (lastBrk_ || lastFrm_ || lastPar_) {
      display.print(' ');
      if (lastBrk_) display.print('B');
      if (lastFrm_) display.print('F');
      if (lastPar_) display.print('P');
    }
    display.display();
    return;
  }
  // marker hex (so the operator can confirm what the probe is sending)
  display.setCursor(0, 20);
  display.print(F("mk "));
  for (size_t i = 0; i < gMarkerLen; ++i) {
    if (i) display.print(' ');
    if (gMarker[i] < 0x10) display.print('0');
    display.print(gMarker[i], HEX);
  }
  // last burst echo outcome: FULL = all marker bytes in order,
  // PART = some marker bytes, NONE = no marker bytes seen on RX.
  display.setCursor(0, 32);
  display.print(F("echo "));
  display.print(lastEchoFull_ ? F("FULL") : (lastEchoBytes_ ? F("PART") : F("NONE")));
  display.print(F(" "));
  display.print(lastEchoBytes_);
  display.print('/');
  display.print(gMarkerLen);
  display.setCursor(0, 44);
  display.print(F("stray "));
  display.print(lastStray_);
  if (lastBrk_ || lastFrm_ || lastPar_) {
    display.print(' ');
    if (lastBrk_) display.print('B');
    if (lastFrm_) display.print('F');
    if (lastPar_) display.print('P');
  }
  display.display();
}

// --- CDC command parsing (line-buffered) ---------------------------
//   "?"            -> emit epoch, skip burst   (also fires immediately
//                    on a lone '?' with no newline, for simple clients)
//   "marker HH HH" -> set runtime marker (1..kMaxMarkerLen hex bytes),
//                     emit epoch, skip burst
//   empty line     -> ignored
char cmdBuf_[40];
size_t cmdLen_ = 0;

static bool parseHexNybble(char c, uint8_t& out) {
  if (c >= '0' && c <= '9') { out = c - '0'; return true; }
  if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
  if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
  return false;
}

static void handleCommand(const char* line) {
  // "?" -> epoch
  if (line[0] == '?' && line[1] == '\0') {
    emitEpoch();
    return;
  }
  // "marker HH HH ..." -> set runtime marker
  if (strncmp(line, "marker", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    uint8_t tmp[kMaxMarkerLen];
    size_t n = 0;
    size_t i = (line[6] == ' ') ? 7 : 6;
    while (line[i] != '\0' && n < kMaxMarkerLen) {
      while (line[i] == ' ') ++i;
      if (line[i] == '\0') break;
      uint8_t hi, lo;
      if (!parseHexNybble(line[i], hi)) break;
      ++i;
      if (line[i] == '\0') { tmp[n++] = hi; break; }  // lone nybble
      if (!parseHexNybble(line[i], lo)) { tmp[n++] = hi; break; }
      tmp[n++] = (hi << 4) | lo;
      ++i;
    }
    if (n > 0) {
      memcpy(gMarker, tmp, n);
      gMarkerLen = n;
    }
    emitEpoch();  // confirm the new marker back to the host
  }
  // "mode deonly" / "mode normal" -> set burst mode
  if (strncmp(line, "mode", 4) == 0 && line[4] == ' ') {
    const char* m = line + 5;
    if (strcmp(m, "deonly") == 0) {
      gMode = Mode::kDeOnly;
      emitEpoch();
    } else if (strcmp(m, "normal") == 0) {
      gMode = Mode::kNormal;
      emitEpoch();
    }
  }
  // unknown / empty -> ignored
}

// Returns true if a command was processed this call (so loop() skips
// the burst). A lone '?' with no newline fires immediately.
static bool serviceCdc() {
  bool fired = false;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '?' && cmdLen_ == 0) {
      emitEpoch();
      fired = true;
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf_[cmdLen_] = '\0';
      if (cmdLen_ > 0) {
        handleCommand(cmdBuf_);
        fired = true;
      }
      cmdLen_ = 0;
      continue;
    }
    if (cmdLen_ < sizeof(cmdBuf_) - 1) {
      cmdBuf_[cmdLen_++] = c;
    }
  }
  return fired;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  const uint32_t kWaitMs = 3000;
  uint32_t start = millis();
  while (!Serial && (millis() - start) < kWaitMs) delay(10);

  // Runtime marker from the compile default.
  gMarkerLen = sizeof(kDefaultMarker);
  if (gMarkerLen > kMaxMarkerLen) gMarkerLen = kMaxMarkerLen;
  memcpy(gMarker, kDefaultMarker, gMarkerLen);

  Serial1.begin(kBaud, SERIAL_8N2, RX /*D7*/, TX /*D6*/);
  pinMode(kTxenPin, OUTPUT);
  digitalWrite(kTxenPin, LOW);  // never come up holding the bus
  clearUartErrFlags();  // drop any boot-time latched errors

  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  if (display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr)) {
    oledOk = true;
    display.dim(true);
    display.setTextWrap(false);
    drawSplash();
  } else {
    oledOk = false;
  }

  emitEpoch();
}

void loop() {
  if (serviceCdc()) return;  // a CDC command was handled; skip this burst

  evCount_ = 0;
  flushRx();
  clearUartErrFlags();  // baseline: no latched errors going into the burst

  // 1. assert TXEN
  digitalWrite(kTxenPin, HIGH);
  rec("assert");

  // 2. gapless write of the marker (queue time stamped; the real wire
  //    edge is the "drained" stamp below). In deonly mode, NO bytes are
  //    written — the DE transition is the only event on the pair.
  if (gMode == Mode::kNormal) {
    Serial1.write(gMarker, gMarkerLen);
  }
  rec("tx");

  // 3-4. tight poll: capture RX arrivals AND detect the shift-register
  //      drain edge non-blockingly. TXEN deasserts the instant the drain
  //      edge is seen (same discipline as the library), then RX keeps
  //      being polled for the rest of the window to catch any tail.
  //      In deonly mode there is nothing to drain; we still poll for the
  //      same window so any stray byte is comparable to normal mode.
  bool drained = false;
  uint32_t windowStart = micros();
  while ((micros() - windowStart) < ECHO_CAPTURE_US) {
    if (Serial1.available()) {
      rec("rx", Serial1.read());
    }
    if (gMode == Mode::kNormal && !drained &&
        uart_wait_tx_done(kUart, 0) == ESP_OK) {
      rec("drained");
      digitalWrite(kTxenPin, LOW);
      rec("deassert");
      drained = true;
    }
  }
  // deonly mode: deassert at the end of the window (no drain edge to
  // wait on). Normal mode: safety deassert if the drain edge was missed.
  if (gMode == Mode::kDeOnly) {
    rec("deassert");
    digitalWrite(kTxenPin, LOW);
  } else if (!drained) {
    digitalWrite(kTxenPin, LOW);  // safety: never carry TXEN across bursts
    rec("deassert");
  }
  rec("end");

  // 5. UART error flags after the capture window. A stray 0x00 paired
  //    with a latched brk/frm bit is a BREAK/framing artifact of the
  //    deassert edge, not a transmitted byte.
  uint8_t errm = readUartErrMask();
  {
    char tag[8];
    snprintf(tag, sizeof(tag), "err%1u", (unsigned)errm);
    rec(tag);  // pack the mask into the event tag (0..7)
  }

  // 6. emit the timeline to CDC now that the burst is over
  for (size_t i = 0; i < evCount_; ++i) {
    Serial.print("{\"e\":\"");
    Serial.print(evs_[i].tag);
    Serial.print("\",\"n\":");
    Serial.print(burstN_);
    Serial.print(",\"us\":");
    Serial.print(evs_[i].us);
    if (evs_[i].v >= 0) {
      Serial.print(",\"v\":");
      Serial.print(evs_[i].v);
    }
    Serial.println("}");
  }

  // Post-burst echo summary for the OLED status panel. Computed from the
  // already-collected events (rx events carry v >= 0; all others are -1),
  // so this never touches the timed burst. The drawStatus() call below
  // ends in display.display(), which BLOCKS ~25ms (1024-byte I2C push) —
  // it stays here, after the burst and CDC emission, and is throttled to
  // kDisplayRefreshMs so it never lands in the tight-poll window.
  size_t matched = 0;
  size_t rxIdx = 0;
  bool firstRx = true;
  uint32_t firstRxUs = 0, lastRxUs = 0;
  size_t stray = 0;
  for (size_t i = 0; i < evCount_; ++i) {
    if (evs_[i].v >= 0) {
      if (rxIdx < gMarkerLen && evs_[i].v == gMarker[rxIdx]) {
        ++matched; ++rxIdx;
      } else {
        ++stray;
      }
      if (firstRx) { firstRxUs = evs_[i].us; firstRx = false; }
      lastRxUs = evs_[i].us;
    }
  }
  lastEchoBytes_ = matched;
  lastEchoFull_ = (matched == gMarkerLen);
  lastLatUs_ = firstRx ? 0 : (firstRxUs - evs_[0].us);
  lastDurUs_ = firstRx ? 0 : (lastRxUs - firstRxUs);
  lastStray_ = stray;
  // deonly mode never sends the marker, so FULL/PART is misleading.
  if (gMode == Mode::kDeOnly) {
    lastEchoBytes_ = 0;
    lastEchoFull_ = false;
  }
  lastBrk_ = (errm & 0x1) != 0;
  lastFrm_ = (errm & 0x2) != 0;
  lastPar_ = (errm & 0x4) != 0;

  if (oledOk && (millis() - lastDisplayMs) >= kDisplayRefreshMs) {
    drawStatus();
    lastDisplayMs = millis();
  }

  ++burstN_;
  delay(ECHO_GAP_MS);
}
