// XiaoHostTracer.ino — the stage-2 Xiao Host R&D image (map issue
// #21): CMRIHost on the cpNode-Xiao RS-485 block, command-and-control
// over USB CDC. Same engine, same listeners as the desktop tracer
// (extras/desktop/cmri_tracer.cpp) via testbed/TracerShell.h;
// only this main() differs.
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
// Host-seat wiring per docs/testbed-physical-notes.md: T± on the poll
// pair, R± on the reply pair. The Host/Node inversion is entirely in
// the crossover cable — this pinout is identical to a node's.
//
// No OTA, no WiFi in this image. The OLED diagnostic display was
// added by #11: the same shared HostStatusPanel the SimpleHost example
// uses, showing polling cadence (alternating c/s ↔ ms/cycle), per-node
// state, last-turnaround latency, and a rolling recent-error count.
//
// Inter-byte timeout: 50 ms tolerant override (stage-2 bench finding,
// wire-tap verified). The decoder measures gaps at tick granularity,
// and the ESP32-C6 runtime stalls up to ~32 ms every ~2 s — arrival
// gaps are not wire gaps, the same artifact class as stage 1's USB
// chunking. The wire itself measured 100% gapless. The 250 ms reply
// gate remains the genuine truncation guard. Be strict in what you
// send, forgiving in what you accept.
//
// C&C: verbs on the CDC stream (quiesce | resume | status |
// setbit <n> <0|1> | writeoutputs <hex> | forcetx | quit),
// JSON lines back. After quit the image emits "final" and parks;
// reset the board to run again.

#include <Arduino.h>

#include "CMRIHost.h"
#include "SerialCMRITransport.h"
#include "testbed/TracerShell.h"

#include "Esp32UartCMRISerialPort.h"

// ---- OLED diagnostic display (#11)
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"          // shared HostStatusPanel

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;
CMRInet::examples::HostStatusPanel panel;

/// Two-letter state tag for the OLED line.
const char* stateTag(CMRInet::RemoteNodeState s) {
  switch (s) {
    case CMRInet::RemoteNodeState::kOnline:        return "ON ";
    case CMRInet::RemoteNodeState::kStale:         return "OLD";
    case CMRInet::RemoteNodeState::kOffline:       return "OFF";
    case CMRInet::RemoteNodeState::kUninitialized: return "---";
  }
  return "??";
}

// Build-time knobs, overridable from a CLI build — e.g. the wrong-UA
// negative test:
//   --build-property "build.defines=-DTRACER_ADDRESS=31"
// (build.defines, NOT build.extra_flags: the esp32 core composes
// build.extra_flags itself, and overriding it clobbers the board's
// -DARDUINO_USB_CDC_ON_BOOT=1 — which would silently move Serial off
// the USB CDC console this image depends on.)
#ifndef TRACER_ADDRESS
#define TRACER_ADDRESS 30     // node address; wire UA = address + 65
#endif
#ifndef TRACER_INPUT_BYTES
#define TRACER_INPUT_BYTES 7  // bench node: 2 phantom CPNODE + 5 IOX IN
#endif
#ifndef TRACER_OUTPUT_BYTES
#define TRACER_OUTPUT_BYTES 7  // bench node: 2 phantom CPNODE + 5 IOX OUT
#endif
#ifndef TRACER_BAUD
#define TRACER_BAUD 28800
#endif
#ifndef TRACER_INTER_BYTE_TIMEOUT_MS
#define TRACER_INTER_BYTE_TIMEOUT_MS 50  // 0 disables (interop 2.2.6 exception)
#endif

namespace {

constexpr const char* kImage = "xiao_host_tracer";
// 0.1.1: hardware transmit-drain truth (Esp32UartCMRISerialPort) — the
// ~2 s C6 runtime stall made the estimate-based drain drop TXEN mid-ETX.
// 0.1.2: 50 ms inter-byte tolerance — the same stall splits intact
// replies at the tick level; the rate-derived timeout misread the gap.
// 0.1.3 (#27): Esp32UartCMRISerialPort promoted from this sketch into
// the library (src/); the library's inter-byte abort doctrine now
// ships a tolerant default (Design D13). This image keeps its explicit
// 50 ms override, so runtime behavior is unchanged from 0.1.2.
// 0.2.0: I/T bench slice (map issue #30) — output image via
// TRACER_OUTPUT_BYTES, onTrace packet telemetry, and output verbs
// (setbit/writeoutputs/forcetx) so T is exercisable from the bench.
// 0.3.0: Add generator-control verbs (enable, disable, configure) for
// fastwalker, slowwalker, toggleoutfrominput, and stall stimulus (#55).
// 0.4.0: Capture-mode ring + run/dump/reset + runtime node topology (#47).
constexpr const char* kVersion = "0.4.0";
constexpr int kTxenPin = D3;  // specific to the cpNode-Xiao board

CMRInet::Esp32UartCMRISerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                     TRACER_BAUD);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::RemoteNodeHandle* node = nullptr;
CMRInet::testbed::TracerShell engine;

bool finished = false;  // quit latched: "final" emitted, polling parked


// ---- RAM Ring Buffer ----------------------------------------------------
struct RingRecord {
  uint32_t t_ms;
  uint8_t ua;
  uint8_t mt;
  uint8_t flags; // bit 0 = 1 for TX, 0 for RX
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

void ourOnTrace(void* context, bool transmit, const CMRInet::CMRIPacket& packet) {
  if (run_active) {
    if (packet.mt == 'I' || packet.mt == 'T') {
      run_it_frames++;
    }
    if (ring_used < kRingCap) {
      RingRecord& r = ring[ring_used++];
      r.t_ms = millis();
      r.ua = packet.ua;
      r.mt = packet.mt;
      r.flags = transmit ? 1 : 0;
      r.len = packet.length;
    }
  } else {
    engine.emitPacket(transmit, packet);
  }
}

bool host_begun = false;
void lazyBegin() {
  if (!host_begun) {
    if (host.begin() != CMRInet::CMRIHost::ConfigStatus::kOk) {
      Serial.print("{\"event\":\"fatal\",\"error\":\"begin rejected configuration: ");
      Serial.print(CMRInet::configStatusString(host.configStatus()));
      Serial.println("\"}");
      for (;;) delay(1000);
    }
    host_begun = true;
  }
}
// --------------------------------------------------------------------------

void writeCdcLine(void* /*context*/, const char* line) {
  Serial.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  Serial.write('\n');
}

/// One newline-terminated verb from non-blocking CDC input. CRs are
/// dropped so a terminal sending CRLF works. Returns false when no
/// complete line is waiting.
bool readVerb(char* out, size_t len) {
  static char buffer[128];
  static size_t used = 0;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
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
// ---- Generator definitions (Issue #55) -----------------------------------
#include "GeneratorParser.h"

struct FastWalkerGenerator {
  bool enabled = false;
  uint32_t period_ms = 250;
  uint8_t byte = 3;
  uint32_t last_ms = 0;
  uint8_t step = 0;

  void tick(uint32_t now_ms) {
    if (!enabled || node == nullptr) return;
    if (now_ms - last_ms >= period_ms) {
      node->setOutputBit(byte * 8 + step, false);
      if (step > 0) {
        node->setOutputBit(byte * 8 + (step - 1), true);
      } else if (last_ms != 0) {
        node->setOutputBit(byte * 8 + 7, true);
      }
      step = (step + 1) % 8;
      last_ms = now_ms;
    }
  }
};

struct SlowWalkerGenerator {
  bool enabled = false;
  uint32_t period_ms = 1000;
  uint8_t byte = 5;
  uint32_t last_ms = 0;
  uint8_t step = 0;

  void tick(uint32_t now_ms) {
    if (!enabled || node == nullptr) return;
    if (now_ms - last_ms >= period_ms) {
      node->setOutputBit(byte * 8 + step, true);
      if (step > 0) {
        node->setOutputBit(byte * 8 + (step - 1), false);
      } else if (last_ms != 0) {
        node->setOutputBit(byte * 8 + 7, false);
      }
      step = (step + 1) % 8;
      last_ms = now_ms;
    }
  }
};

struct ToggleOutFromInputGenerator {
  bool enabled = false;
  uint16_t in_bit = 48;
  uint16_t out_bit = 32;
  bool last_in = false;

  void tick(uint32_t now_ms) {
    if (!enabled || node == nullptr) return;
    bool current_in = node->inputBit(in_bit);
    if (current_in && !last_in) {
      node->setOutputBit(out_bit, !node->outputBit(out_bit));
    }
    last_in = current_in;
  }
};

struct StallGenerator {
  bool enabled = false;
  uint32_t ms = 0;
  uint32_t period_ms = 150;
  enum class Mode { kYield, kBusy } mode = Mode::kYield;
  uint32_t last_ms = 0;

  void tick(uint32_t now_ms) {
    if (!enabled || ms == 0) return;
    if (now_ms - last_ms >= period_ms || last_ms == 0) {
      if (mode == Mode::kYield) {
        delay(ms);
      } else {
        uint32_t start = millis();
        while (millis() - start < ms) {
          // busy spin
        }
      }
      last_ms = millis();
    }
  }
};

FastWalkerGenerator fastwalker;
SlowWalkerGenerator slowwalker;
ToggleOutFromInputGenerator toggleoutfrominput;
StallGenerator stall;

bool handleGeneratorControl(char* cmd) {
  char* saveptr = nullptr;
  char* action = strtok_r(cmd, " ", &saveptr);
  if (!action) return false;

  bool is_enable = (strcmp(action, "enable") == 0);
  bool is_disable = (strcmp(action, "disable") == 0);
  bool is_configure = (strcmp(action, "configure") == 0);

  if (!is_enable && !is_disable && !is_configure) return false;

  char* gen_name = strtok_r(nullptr, " ", &saveptr);
  if (!gen_name) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"");
    Serial.print(action);
    Serial.println(": missing generator\"}");
    return true;
  }

  bool valid_gen = (strcmp(gen_name, "fastwalker") == 0 ||
                    strcmp(gen_name, "slowwalker") == 0 ||
                    strcmp(gen_name, "toggleoutfrominput") == 0 ||
                    strcmp(gen_name, "stall") == 0);
  if (!valid_gen) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"unknown generator '");
    Serial.print(gen_name);
    Serial.println("'\"}");
    return true;
  }

  if (is_disable) {
    if (strcmp(gen_name, "fastwalker") == 0) fastwalker.enabled = false;
    else if (strcmp(gen_name, "slowwalker") == 0) slowwalker.enabled = false;
    else if (strcmp(gen_name, "toggleoutfrominput") == 0) toggleoutfrominput.enabled = false;
    else if (strcmp(gen_name, "stall") == 0) stall.enabled = false;
    
    Serial.print("{\"event\":\"disable\",\"generator\":\"");
    Serial.print(gen_name);
    Serial.println("\"}");
    return true;
  }

  char* args = strtok_r(nullptr, "", &saveptr);
  if (args) {
    ParsedGeneratorParams p = parseGeneratorParams(args, gen_name);
    if (p.error_code) {
      Serial.print("{\"event\":\"error\",\"error\":\"");
      Serial.print(p.error_code);
      Serial.print("\",\"message\":\"Invalid parameter '");
      Serial.print(p.error_val);
      Serial.println("'\"}");
      return true;
    }
    
    if (strcmp(gen_name, "fastwalker") == 0 || strcmp(gen_name, "slowwalker") == 0) {
      if (p.has_period) {
        if (strcmp(gen_name, "fastwalker") == 0) fastwalker.period_ms = p.period_ms;
        else slowwalker.period_ms = p.period_ms;
      }
      if (p.has_byte) {
        if (strcmp(gen_name, "fastwalker") == 0) fastwalker.byte = p.byte_idx;
        else slowwalker.byte = p.byte_idx;
      }
    } else if (strcmp(gen_name, "toggleoutfrominput") == 0) {
      if (p.has_in) toggleoutfrominput.in_bit = p.in_bit;
      if (p.has_out) toggleoutfrominput.out_bit = p.out_bit;
    } else if (strcmp(gen_name, "stall") == 0) {
      if (p.has_stall_ms) stall.ms = p.stall_ms;
      if (p.has_period) stall.period_ms = p.period_ms;
      if (p.has_mode) {
        stall.mode = p.mode_busy ? StallGenerator::Mode::kBusy : StallGenerator::Mode::kYield;
      }
    }
  }

  if (is_enable) {
    lazyBegin();
    if (strcmp(gen_name, "fastwalker") == 0) { fastwalker.enabled = true; fastwalker.last_ms = 0; }
    else if (strcmp(gen_name, "slowwalker") == 0) { slowwalker.enabled = true; slowwalker.last_ms = 0; }
    else if (strcmp(gen_name, "toggleoutfrominput") == 0) { 
      toggleoutfrominput.enabled = true; 
      toggleoutfrominput.last_in = node ? node->inputBit(toggleoutfrominput.in_bit) : false; 
    }
    else if (strcmp(gen_name, "stall") == 0) { 
      if (stall.ms == 0) stall.enabled = false;
      else { stall.enabled = true; stall.last_ms = 0; }
    }
  }

  Serial.print("{\"event\":\"");
  Serial.print(action);
  Serial.print("\",\"generator\":\"");
  Serial.print(gen_name);
  Serial.println("\"}");
  return true;
}

void emitGeneratorsAndNodesStatus(void* context, char* buffer, size_t remaining_capacity) {
  char nodesBuf[512] = ",\"nodes\":[";
  size_t offset = strlen(nodesBuf);
  bool first = true;
  for (uint8_t addr = 0; addr <= 127; ++addr) {
    CMRInet::RemoteNodeHandle* n = host.node(addr);
    if (n) {
      if (!first) {
         offset += snprintf(nodesBuf + offset, sizeof(nodesBuf) - offset, ",");
      }
      offset += snprintf(nodesBuf + offset, sizeof(nodesBuf) - offset,
          "{\"ua\":%u,\"in\":%zu,\"out\":%zu,\"state\":\"%s\"}",
          n->ua(), n->inputLength(), n->outputLength(), CMRInet::testbed::stateName(n->state()));
      first = false;
    }
  }
  snprintf(nodesBuf + offset, sizeof(nodesBuf) - offset, "]");

  snprintf(buffer, remaining_capacity, 
    "%s,\"generators\":{"
    "\"fastwalker\":{\"enabled\":%s,\"period_ms\":%lu,\"byte\":%u},"
    "\"slowwalker\":{\"enabled\":%s,\"period_ms\":%lu,\"byte\":%u},"
    "\"toggleoutfrominput\":{\"enabled\":%s,\"in\":%u,\"out\":%u},"
    "\"stall\":{\"enabled\":%s,\"ms\":%lu,\"period_ms\":%lu,\"mode\":\"%s\"}"
    "}",
    nodesBuf,
    fastwalker.enabled ? "true" : "false", (unsigned long)fastwalker.period_ms, fastwalker.byte,
    slowwalker.enabled ? "true" : "false", (unsigned long)slowwalker.period_ms, slowwalker.byte,
    toggleoutfrominput.enabled ? "true" : "false", toggleoutfrominput.in_bit, toggleoutfrominput.out_bit,
    stall.enabled ? "true" : "false", (unsigned long)stall.ms, (unsigned long)stall.period_ms, stall.mode == StallGenerator::Mode::kYield ? "yield" : "busy"
  );
}
// --------------------------------------------------------------------------

/// Draw the host status panel (shared HostStatusPanel, same layout as
/// SimpleHost): header with alternating cadence, one per-node row with
/// state / latency / recent errors. Defined after the anonymous
/// namespace so it can see host, node, and TRACER_ADDRESS.
void drawHostStatus() {
  if (!oledOk) return;
  const uint32_t now = millis();
  const uint32_t pollsSent = host.statistics().pollsSent;
  uint32_t nodeErrs[1] = {node ? node->statistics().errors : 0};
  panel.sample(now, pollsSent, nodeErrs, 1);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("TRC"));
  display.setTextSize(1);
  char header[16];
  panel.headerText(header, sizeof(header), now);
  display.setCursor(60, 4);
  display.print(header);

  const char* tag = (node != nullptr) ? stateTag(node->state()) : "---";
  const uint32_t latMs = (node != nullptr)
      ? node->statistics().lastTurnaroundMs : 0;
  char row[24];
  panel.nodeRowText(row, sizeof(row), now, 0,
                    TRACER_ADDRESS, tag, latMs);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print(row);
  display.display();
}

void setup() {
  Serial.begin(115200);  // USB CDC: the command-and-control stream
  // R&D image: wait for the C&C stream so the epoch line — and every
  // line after it — is captured. Bench order: open the port first,
  // then (re)power the board. But bound the wait at 3 s so a headless
  // board (no terminal attached, as during flash_and_probe.sh which
  // does not open the tracer's CDC port) still boots and drives the
  // OLED / polls the bus without hanging setup(). On the Xiao ESP32-C6,
  // `Serial` is native USB CDC and only reads true once a host asserts
  // DTR/RTS; without a bound the wait parks here forever and loop()
  // never runs (issue #46).
  const uint32_t kSerialWaitMs = 3000;
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < kSerialWaitMs) {
    delay(10);
  }
  // Non-blocking CDC writes: with the default TX timeout, each
  // Serial.write() stalls when a cable is plugged in but no host has
  // the port open (DTR not asserted) — the ring buffer fills and nobody
  // drains it. At many packets/s that starves loop() and the OLED
  // freezes. setTxTimeoutMs(0) makes writes discard-and-return when no
  // host is reading instead of blocking (Espressif's HWCDC workaround;
  // same pattern as the sniffer sketch). A larger TX buffer reduces
  // drop rate when a host IS reading but slowly (issue #46).
  Serial.setTxTimeoutMs(0);
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(1024);
#endif

  // The CMRI wire: 28800 8N2 on the MAX3491 UART pins.
  Serial1.begin(TRACER_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  // Tick-gap tolerance; see the header comment. Survives begin().
  transport.setInterByteTimeoutMs(TRACER_INTER_BYTE_TIMEOUT_MS);

  // OLED diagnostic display (#11).
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
  }

  CMRInet::RemoteNodeConfig nodeConfig;
  nodeConfig.inputBytes = TRACER_INPUT_BYTES;
  nodeConfig.outputBytes = TRACER_OUTPUT_BYTES;
  host.addRemoteNode(TRACER_ADDRESS, nodeConfig);

  if (host.configStatus() == CMRInet::CMRIHost::ConfigStatus::kOk) {
    node = host.node(TRACER_ADDRESS);
    engine.bind(host, transport, *node, kImage, kVersion,
                writeCdcLine, nullptr);
    host.onTrace(ourOnTrace, nullptr);
    engine.setStatusExtender(emitGeneratorsAndNodesStatus, nullptr);
  }

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
    
    fastwalker.tick(nowMs);
    slowwalker.tick(nowMs);
    toggleoutfrominput.tick(nowMs);
    stall.tick(nowMs);
  }

  if (run_active) {
    run_loop_its++;
    if (nowMs >= run_end_ms) {
      run_active = false;
      uint32_t end_ms = millis();
      uint32_t total_polls = host.statistics().pollsSent - run_polls;
      Serial.print("END CAPTURE t="); Serial.print(end_ms);
      Serial.print(" polls="); Serial.print(total_polls);
      Serial.print(" its="); Serial.print(run_it_frames);
      Serial.print(" loops="); Serial.print(run_loop_its);
      Serial.print(" ring_used="); Serial.print(ring_used);
      Serial.print("/"); Serial.println(kRingCap);
    }
  }

  char verb[128];
  if (readVerb(verb, sizeof(verb))) {
    char verbCopy[128];
    strncpy(verbCopy, verb, sizeof(verbCopy));
    
    bool handled = false;
    if (strncmp(verb, "enable", 6) == 0 || 
        strncmp(verb, "disable", 7) == 0 || 
        strncmp(verb, "configure", 9) == 0) {
      handled = handleGeneratorControl(verbCopy);
    } else if (strncmp(verb, "node ", 5) == 0) {
      char* saveptr = nullptr;
      strtok_r(verbCopy, " ", &saveptr); // "node"
      char* action = strtok_r(nullptr, " ", &saveptr);
      if (action) {
        if (strcmp(action, "add") == 0) {
          char* ua_s = strtok_r(nullptr, " ", &saveptr);
          char* in_s = strtok_r(nullptr, " ", &saveptr);
          char* out_s = strtok_r(nullptr, " ", &saveptr);
          if (ua_s && in_s && out_s) {
            if (host_begun) {
              Serial.println("{\"event\":\"error\",\"error\":\"locked\",\"message\":\"node add: configuration locked by begin\"}");
            } else {
              uint8_t addr = atoi(ua_s);
              uint8_t in_b = atoi(in_s);
              uint8_t out_b = atoi(out_s);
              CMRInet::RemoteNodeHandle* existing = host.node(addr);
              if (existing) {
                  if (existing->inputLength() == in_b && existing->outputLength() == out_b) {
                      Serial.print("{\"event\":\"node_add\",\"ua\":"); Serial.print(addr); Serial.println("}");
                  } else {
                      Serial.println("{\"event\":\"error\",\"error\":\"inUse\",\"message\":\"address already in use with different size\"}");
                  }
              } else {
                  CMRInet::RemoteNodeConfig cfg;
                  cfg.inputBytes = in_b;
                  cfg.outputBytes = out_b;
                  host.addRemoteNode(addr, cfg);
                  if (host.configStatus() != CMRInet::CMRIHost::ConfigStatus::kOk) {
                      Serial.println("{\"event\":\"error\",\"error\":\"addFailed\"}");
                  } else {
                      Serial.print("{\"event\":\"node_add\",\"ua\":"); Serial.print(addr); Serial.println("}");
                  }
              }
            }
          }
        } else if (strcmp(action, "enable") == 0) {
          char* ua_s = strtok_r(nullptr, " ", &saveptr);
          if (ua_s) {
            lazyBegin();
            uint8_t addr = atoi(ua_s);
            CMRInet::RemoteNodeHandle* n = host.node(addr);
            if (n) n->setEnabled(true);
            Serial.print("{\"event\":\"node_enable\",\"ua\":"); Serial.print(addr); Serial.println("}");
          }
        } else if (strcmp(action, "disable") == 0) {
          char* ua_s = strtok_r(nullptr, " ", &saveptr);
          if (ua_s) {
            lazyBegin();
            uint8_t addr = atoi(ua_s);
            CMRInet::RemoteNodeHandle* n = host.node(addr);
            if (n) n->setEnabled(false);
            Serial.print("{\"event\":\"node_disable\",\"ua\":"); Serial.print(addr); Serial.println("}");
          }
        }
      }
      handled = true;
    } else if (strcmp(verb, "reboot") == 0) {
      Serial.println("{\"event\":\"reboot\"}");
      Serial.flush();  // ensure the response goes out before we drop CDC
      ESP.restart();
      handled = true;
    } else if (strncmp(verb, "run ", 4) == 0) {
      char* saveptr = nullptr;
      strtok_r(verbCopy, " ", &saveptr); // "run"
      char* secs_s = strtok_r(nullptr, " ", &saveptr);
      if (secs_s) {
        lazyBegin();
        uint32_t secs = strtoul(secs_s, nullptr, 10);
        run_active = true;
        ring_used = 0;
        run_polls = host.statistics().pollsSent;
        run_loop_its = 0;
        run_it_frames = 0;
        run_start_ms = millis();
        run_end_ms = run_start_ms + secs * 1000;
        Serial.print("BEGIN CAPTURE t="); Serial.println(run_start_ms);
      }
      handled = true;
    } else if (strcmp(verb, "dump") == 0) {
      Serial.print("BEGIN DUMP records="); Serial.println(ring_used);
      for (size_t i = 0; i < ring_used; ++i) {
        const RingRecord& r = ring[i];
        Serial.print("PKT t="); Serial.print(r.t_ms);
        Serial.print(r.flags == 1 ? " TX " : " RX ");
        Serial.print("ua="); Serial.print(r.ua);
        Serial.print(" mt="); Serial.print((char)r.mt);
        Serial.print(" len="); Serial.print(r.len);
        Serial.print(" n="); Serial.println(i);
      }
      Serial.println("END DUMP");
      handled = true;
    } else if (strcmp(verb, "reset") == 0) {
      run_active = false;
      run_end_ms = 0;
      ring_used = 0;
      run_polls = 0;
      run_loop_its = 0;
      run_it_frames = 0;
      fastwalker.enabled = false;
      slowwalker.enabled = false;
      toggleoutfrominput.enabled = false;
      stall.enabled = false;
      Serial.println("{\"event\":\"reset\"}");
      handled = true;
    } else if (strcmp(verb, "status") == 0) {
      using VerbResult = CMRInet::testbed::TracerShell::VerbResult;
      engine.handleVerb(verb);
      handled = true;
    }

    if (!handled) {
      using VerbResult = CMRInet::testbed::TracerShell::VerbResult;
      if (engine.handleVerb(verb) == VerbResult::kQuit && !finished) {
        engine.emitLine("final");
        finished = true;  // reset the board to run again
      }
    }
  }

  // Redraw the OLED on a timer.
  if (nowMs - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = nowMs;
  }
}
