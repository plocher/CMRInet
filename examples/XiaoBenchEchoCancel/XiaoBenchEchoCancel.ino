// XiaoBenchEchoCancel.ino — Phase B 2-wire echo-cancel characterization.
//
// Purpose: drive the real CMRInet stack (CMRIHost + SerialCMRITransport +
// TracerShell) against a PHANTOM node (UA 32, no board) so the only RX
// traffic is the host's own self-echo on the 2-wire jumpered bus. The
// echo-cancel mitigation (issue #104) is toggleable at runtime over CDC
// (`echocancel on|off`) so B1 (cancel OFF, defect exposed) and B2 (cancel
// ON, mitigation active) are one variable, no reflash.
//
// This is the first Phase to use the real library code. Phase A (the
// library-free XiaoBenchEcho probe) measured the echo's raw shape; this
// probe runs that echo through the transport, decoder, and host's
// drainReceive_ reply-match layer, and characterizes where the echo is
// actually caught (byte-level kWriting discard vs frame-level kDraining
// cancel) and whether the trailing 0x00 (measured in Phase A) is harmless
// through the real stack.
//
// Reuses the XiaoHostTracer C&C substrate (readVerb, run/dump/ring,
// lazyBegin, XiaoCdcConsole) — no generators, one phantom UA, plus the
// sketch-local echocancel verb. See docs/two-wire-echo-bench-findings.md
// and docs/adr/0003-*.md for the observed-facts baseline and the
// semantic boundary this probe validates.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS485 transmit enable
//
// STATUS: bench probe (Phase B/C). Uses the shipped library's
// three-state echo-cancel mode (setEchoCancelMode: Auto | AlwaysOn |
// AlwaysOff), toggled at runtime over CDC. The mode survives begin().

#include <Arduino.h>

#include "CMRIHost.h"
#include "transport/serial.h"
#include "transport/serialESP32.h"
#include "testbed/TracerShell.h"
#include "testbed/CdcLineWriter.h"  // #99: shared CDC line writer

// OLED diagnostic display (same panel as the other bench sketches).
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"  // shared HostStatusPanel

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;

// #64: OLED custom annotations (harness-injected).
char displayLine1[22] = {0};
char displayLine2[22] = {0};

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;
CMRInet::examples::HostStatusPanel panel;

// Non-blocking incremental display update (#11): display.display() blocks
// ~25ms (1024-byte I2C push), so the framebuffer is sent in 32-byte chunks
// across loop iterations instead. Keeps the poll cadence honest.
bool displayDirty = false;
size_t displayChunkOffset = 0;
const size_t kDisplayChunkSize = 32;
const size_t kDisplayTotalBytes = (128 * 64) / 8;  // 1024 bytes

void updateDisplayIncremental() {
  if (!displayDirty || !oledOk) return;
  if (displayChunkOffset == 0) {
    Wire.beginTransmission(kScreenAddr);
    Wire.write((uint8_t)0x00);
    Wire.write(0x21); Wire.write(0); Wire.write(127);
    Wire.endTransmission();
    Wire.beginTransmission(kScreenAddr);
    Wire.write((uint8_t)0x00);
    Wire.write(0x22); Wire.write(0); Wire.write(0xFF);
    Wire.endTransmission();
  }
  Wire.beginTransmission(kScreenAddr);
  Wire.write((uint8_t)0x40);
  size_t end = displayChunkOffset + kDisplayChunkSize;
  if (end > kDisplayTotalBytes) end = kDisplayTotalBytes;
  for (size_t i = displayChunkOffset; i < end; ++i) {
    Wire.write(display.getBuffer()[i]);
  }
  Wire.endTransmission();
  displayChunkOffset = end;
  if (displayChunkOffset >= kDisplayTotalBytes) {
    displayChunkOffset = 0;
    displayDirty = false;
  }
}

// Build-time knobs.
#ifndef ECHOCALC_UA
#define ECHOCALC_UA 32     // phantom node (no board) -> only RX is self-echo
#endif
#ifndef ECHOCALC_INPUT_BYTES
#define ECHOCALC_INPUT_BYTES 2
#endif
#ifndef ECHOCALC_OUTPUT_BYTES
#define ECHOCALC_OUTPUT_BYTES 2
#endif
#ifndef ECHOCALC_BAUD
#define ECHOCALC_BAUD 28800
#endif
#ifndef ECHOCALC_INTER_BYTE_TIMEOUT_MS
#define ECHOCALC_INTER_BYTE_TIMEOUT_MS 50  // 0 disables (interop 2.2.6)
#endif

constexpr const char* kImage = "xiao_bench_echo_cancel";
constexpr const char* kVersion = "0.1.0";
constexpr int kTxenPin = D3;  // specific to the cpNode-Xiao board

CMRInet::Esp32SerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                      ECHOCALC_BAUD);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::testbed::TracerShell engine;

bool finished = false;  // quit latched: "final" emitted, polling parked

// ---- RAM Ring Buffer (capture mode) -------------------------------------
struct RingRecord {
  uint32_t t_ms;
  uint8_t UA;
  uint8_t mt;
  uint8_t flags;  // bit 0 = TX, bit 1 = invalid wire-UA at capture
  uint8_t len;
} __attribute__((packed));

constexpr size_t kRingCap = 12000;
RingRecord ring[kRingCap];
size_t ring_used = 0;
bool run_active = false;
uint32_t run_end_ms = 0;
uint32_t run_start_ms = 0;
uint32_t run_polls = 0;
uint32_t run_loop_its = 0;
uint32_t run_it_frames = 0;
uint32_t run_invalid_ua_records = 0;
constexpr uint8_t kRingFlagTx = 0x01u;
constexpr uint8_t kRingFlagInvalidUa = 0x02u;
constexpr uint8_t kMaxWireUA = static_cast<uint8_t>(CMRInet::kWireUAOffset + 127u);

bool isLegalWireUA(uint8_t wireUA) {
  return wireUA >= CMRInet::kWireUAOffset && wireUA <= kMaxWireUA;
}

uint8_t toSemanticUA(uint8_t wireUA) {
  return static_cast<uint8_t>(wireUA - CMRInet::kWireUAOffset);
}

void ourOnTrace(void* context, bool transmit, const CMRInet::CMRIPacket& packet) {
  if (run_active) {
    if (packet.mt == 'I' || packet.mt == 'T') {
      run_it_frames++;
    }
    if (ring_used < kRingCap) {
      RingRecord& r = ring[ring_used++];
      const bool legalUa = isLegalWireUA(packet.wireUA);
      r.t_ms = millis();
      r.UA = legalUa ? toSemanticUA(packet.wireUA) : packet.wireUA;
      r.mt = packet.mt;
      r.flags = transmit ? kRingFlagTx : 0u;
      if (!legalUa) {
        r.flags |= kRingFlagInvalidUa;
        run_invalid_ua_records++;
      }
      r.len = packet.length;
    }
  } else {
    engine.emitPacket(transmit, packet);
  }
}

// ---- deferred begin (engine starts on first node/traffic verb) -----------
CMRInet::CMRIHost::ConfigStatus setupStatus = CMRInet::CMRIHost::ConfigStatus::kOk;
bool host_begun = false;
void lazyBegin() {
  if (!host_begun) {
    if (setupStatus != CMRInet::CMRIHost::ConfigStatus::kOk) {
      Serial.print("{\"event\":\"fatal\",\"error\":\"begin rejected configuration: ");
      Serial.print(CMRInet::configStatusString(setupStatus));
      Serial.println("\"}");
      for (;;) delay(1000);
    }
    host.begin();
    host_begun = true;
  }
}

// ---- CDC console seam (shared CdcLineWriter) -----------------------------
class XiaoCdcConsole : public CMRInet::testbed::CdcConsole {
 public:
  bool open() const override { return static_cast<bool>(Serial); }
  size_t availableForWrite() override { return Serial.availableForWrite(); }
  size_t write(const uint8_t* data, size_t n) override {
    return Serial.write(data, n);
  }
  uint32_t nowMs() override { return millis(); }
  void yieldMs() override { delay(1); }
};

XiaoCdcConsole cdc;

bool readVerb(char* out, size_t len) {
  static char buffer[128];
  static size_t used = 0;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
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

// ---- OLED status panel ---------------------------------------------------
// Shows the echo-cancel state + the phantom node's counters, so the
// operator sees the defect/mitigation at a glance. Reflects quiesce:
// after `quit` (finished), the panel shows "QUIESCED" not live data.
void drawHostStatus() {
  if (!oledOk) return;
  const uint32_t now = millis();
  CMRInet::RemoteNodeHandle* node = host.node(ECHOCALC_UA);
  const uint32_t pollsSent = host.statistics().pollsSent;
  uint32_t nodeErrs[1] = {node ? node->statistics().errors : 0};
  panel.sample(now, pollsSent, nodeErrs, 1);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(finished ? F("ECq") : F("EC"));
  display.setTextSize(1);
  char header[16];
  panel.headerText(header, sizeof(header), now);
  display.setCursor(60, 4);
  display.print(header);

  // echo-cancel state line
  display.setCursor(0, 20);
  display.print(F("echo "));
  using M = CMRInet::SerialCMRITransport::EchoCancelMode;
  const char* modeTag =
      (transport.echoCancelMode() == M::kAuto) ? "auto" :
      (transport.echoCancelMode() == M::kAlwaysOn) ? "ON" : "off";
  display.print(modeTag);

  const char* tag =
      (node != nullptr) ? CMRInet::remoteNodeStateTag(node->state()) : "---";
  const uint32_t latMs = (node != nullptr) ? node->statistics().lastTurnaroundMs : 0;
  char row[24];
  panel.nodeRowText(row, sizeof(row), now, 0, ECHOCALC_UA, tag, latMs);
  display.setCursor(0, 32);
  display.print(row);

  // reject/unsolicited counters — the defect signal on the panel
  display.setCursor(0, 44);
  display.print(F("rej="));
  display.print(host.statistics().repliesRejected);
  display.print(F(" uns="));
  display.print(host.statistics().unsolicitedPackets);

  if (displayLine1[0] != '\0') {
    display.setCursor(0, 48);  // overlaps row area on small panel; kept for harness
    display.print(displayLine1);
  }
  if (displayLine2[0] != '\0') {
    display.setCursor(0, 56);
    display.print(displayLine2);
  }
  displayDirty = true;
}

// ---- sketch-local verb: echocancel on|off -------------------------------
// Intercepted before the shell so no library change is needed. Toggles
// SerialCMRITransport::setEchoCancelMode (the serial-transport-scoped
// mitigation, per ADR-0003 — not hoisted to the host or base seam).
// The mode survives begin() (lazyBegin fires on the first node verb).
void emitEchoCancelEvent(const char* state) {
  Serial.print("{\"event\":\"echocancel\",\"state\":\"");
  Serial.print(state);
  Serial.print("\",\"rxDuringTx\":");
  Serial.print(transport.rxDuringTx());
  Serial.println("}");
}

bool handleEchoCancelVerb(char* cmd) {
  char* saveptr = nullptr;
  char* action = strtok_r(cmd, " ", &saveptr);  // "echocancel"
  char* arg = strtok_r(nullptr, " ", &saveptr);  // on|off|auto
  if (!arg) return false;
  using M = CMRInet::SerialCMRITransport::EchoCancelMode;
  M mode;
  const char* tag;
  if (strcmp(arg, "on") == 0) { mode = M::kAlwaysOn; tag = "on"; }
  else if (strcmp(arg, "off") == 0) { mode = M::kAlwaysOff; tag = "off"; }
  else if (strcmp(arg, "auto") == 0) { mode = M::kAuto; tag = "auto"; }
  else { return false; }
  lazyBegin();
  transport.setEchoCancelMode(mode);
  emitEchoCancelEvent(tag);
  return true;
}

// ---- setup / loop --------------------------------------------------------
void setup() {
  Serial.begin(115200);
  const uint32_t kSerialWaitMs = 3000;
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < kSerialWaitMs) delay(10);
  Serial.setTxTimeoutMs(0);
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(1024);
#endif

  Serial1.begin(ECHOCALC_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  transport.setInterByteTimeoutMs(ECHOCALC_INTER_BYTE_TIMEOUT_MS);

  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) display.dim(true);

  // One phantom node: no board, so the only RX traffic is the host's
  // own echo on the 2-wire jumpered bus.
  CMRInet::RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = ECHOCALC_INPUT_BYTES;
  nodeConfig.outputBytes = ECHOCALC_OUTPUT_BYTES;
  setupStatus = host.addRemoteNode(ECHOCALC_UA, nodeConfig);

  engine.bind(host, transport, kImage, kVersion,
              &CMRInet::testbed::writeCdcLineCb, &cdc);
  host.onTrace(ourOnTrace, nullptr);

  engine.setNow(millis());
  char bootMs[16];
  snprintf(bootMs, sizeof(bootMs), "%lu",
           static_cast<unsigned long>(millis()));
  engine.emitEpoch("bootMs", bootMs);
}

void loop() {
  const uint32_t nowMs = millis();
  engine.setNow(nowMs);
  if (!finished) {
    host.tick(nowMs);
  }

  if (run_active) {
    run_loop_its++;
    if (nowMs >= run_end_ms) {
      run_active = false;
      engine.setBackoffTraceOnly(false);
      uint32_t end_ms = millis();
      uint32_t total_polls = host.statistics().pollsSent - run_polls;
      Serial.print("END CAPTURE t="); Serial.print(end_ms);
      Serial.print(" polls="); Serial.print(total_polls);
      Serial.print(" its="); Serial.print(run_it_frames);
      Serial.print(" loops="); Serial.print(run_loop_its);
      Serial.print(" invalid_ua="); Serial.print(run_invalid_ua_records);
      Serial.print(" ring_used="); Serial.print(ring_used);
      Serial.print("/"); Serial.println(kRingCap);
    }
  }

  char verb[128];
  if (readVerb(verb, sizeof(verb))) {
    char verbCopy[128];
    strncpy(verbCopy, verb, sizeof(verbCopy));

    bool handled = false;
    if (strncmp(verb, "echocancel ", 11) == 0) {
      handled = handleEchoCancelVerb(verbCopy);
    } else if (strncmp(verb, "node ", 5) == 0) {
      lazyBegin();
      handled = false;  // fall through to the shell
    } else if (strncmp(verb, "display ", 8) == 0) {
      if (oledOk) {
        char* saveptr = nullptr;
        strtok_r(verbCopy, " ", &saveptr);
        char* line_num_str = strtok_r(nullptr, " ", &saveptr);
        char* text = strtok_r(nullptr, "\n", &saveptr);
        if (line_num_str && text) {
          int line_num = atoi(line_num_str);
          if (line_num == 1) {
            strncpy(displayLine1, text, sizeof(displayLine1) - 1);
            displayLine1[sizeof(displayLine1) - 1] = '\0';
          } else if (line_num == 2) {
            strncpy(displayLine2, text, sizeof(displayLine2) - 1);
            displayLine2[sizeof(displayLine2) - 1] = '\0';
          }
          drawHostStatus();
        }
      }
      handled = true;
    } else if (strcmp(verb, "reboot") == 0) {
      Serial.println("{\"event\":\"reboot\"}");
      Serial.flush();
      ESP.restart();
      handled = true;
    } else if (strncmp(verb, "run ", 4) == 0) {
      char* saveptr = nullptr;
      strtok_r(verbCopy, " ", &saveptr);
      char* secs_s = strtok_r(nullptr, " ", &saveptr);
      if (secs_s) {
        lazyBegin();
        uint32_t secs = strtoul(secs_s, nullptr, 10);
        run_active = true;
        ring_used = 0;
        run_polls = host.statistics().pollsSent;
        run_loop_its = 0;
        run_it_frames = 0;
        run_invalid_ua_records = 0;
        run_start_ms = millis();
        run_end_ms = run_start_ms + secs * 1000;
        engine.setBackoffTraceOnly(true);
        Serial.print("BEGIN CAPTURE t="); Serial.println(run_start_ms);
      }
      handled = true;
    } else if (strcmp(verb, "dump") == 0) {
      Serial.print("BEGIN DUMP records="); Serial.println(ring_used);
      for (size_t i = 0; i < ring_used; ++i) {
        const RingRecord& r = ring[i];
        Serial.print("PKT t="); Serial.print(r.t_ms);
        Serial.print((r.flags & kRingFlagTx) != 0u ? " TX " : " RX ");
        Serial.print("UA="); Serial.print(r.UA);
        if ((r.flags & kRingFlagInvalidUa) != 0u) {
          Serial.print(" wireUA_invalid=1");
        }
        Serial.print(" mt="); Serial.print((char)r.mt);
        Serial.print(" len="); Serial.print(r.len);
        Serial.print(" n="); Serial.println(i);
      }
      Serial.println("END DUMP");
      handled = true;
    } else if (strcmp(verb, "reset") == 0) {
      run_active = false;
      run_end_ms = 0;
      engine.setBackoffTraceOnly(false);
      ring_used = 0;
      run_polls = 0;
      run_loop_its = 0;
      run_it_frames = 0;
      Serial.println("{\"event\":\"reset\"}");
      handled = true;
    } else if (strcmp(verb, "status") == 0) {
      engine.handleVerb(verb);
      handled = true;
    }

    if (!handled) {
      using VerbResult = CMRInet::testbed::TracerShell::VerbResult;
      if (engine.handleVerb(verb) == VerbResult::kQuit && !finished) {
        engine.emitLine("final");
        finished = true;  // quiesced: panel reflects it
      }
    }
  }

  if (nowMs - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = nowMs;
  }
  updateDisplayIncremental();
}
