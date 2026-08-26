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
// C&C: verbs on the CDC stream, JSON lines back. Every verb that acts
// on a node names its UA — the shell holds no bound node (Design v1.2
// D5), so there is no implicit target:
//   status | status <ua>
//   quiesce <ua> | resume <ua> | forcetx <ua>
//   setbit <ua> <bit> <0|1> | writeoutputs <ua> <hex>
//   node add <ua> <in> <out> | node delete <ua>
//   node geometry <ua> <in> <out>
//   node enable <ua> | node disable <ua>
//   enable|disable|configure <generator> [ua <n>] ...
//   run <secs> | dump | reset | reboot | display <1|2> <text> | quit
// After quit the image emits "final" and parks; reset the board to run
// again.

#include <Arduino.h>

#include "CMRIHost.h"
#include "SerialCMRITransport.h"
#include "testbed/TracerShell.h"
#include "testbed/CdcLineWriter.h"  // #99: shared, testable CDC line writer

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

// #64: OLED custom annotations
char displayLine1[22] = {0};
char displayLine2[22] = {0};

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;
CMRInet::examples::HostStatusPanel panel;

// #11: Non-blocking incremental display update
bool displayDirty = false;
size_t displayChunkOffset = 0;
const size_t kDisplayChunkSize = 32; // 32 bytes per loop iteration
const size_t kDisplayTotalBytes = (128 * 64) / 8; // 1024 bytes

void updateDisplayIncremental() {
  if (!displayDirty || !oledOk) return;
  
  // If we're starting a new update, send the commands to set the address
  if (displayChunkOffset == 0) {
    // Send commands to set the page and column address
    Wire.beginTransmission(kScreenAddr);
    Wire.write((uint8_t)0x00); // Co = 0, D/C = 0 (command)
    Wire.write(0x21); // Column start address
    Wire.write(0);    // Column start
    Wire.write(127);  // Column end
    Wire.endTransmission();
    
    Wire.beginTransmission(kScreenAddr);
    Wire.write((uint8_t)0x00); // Co = 0, D/C = 0 (command)
    Wire.write(0x22); // Page start address
    Wire.write(0);    // Page start
    Wire.write(0xFF); // Page end
    Wire.endTransmission();
  }
  
  // Send the next chunk of the framebuffer
  Wire.beginTransmission(kScreenAddr);
  Wire.write((uint8_t)0x40); // Co = 0, D/C = 1
  
  size_t end = displayChunkOffset + kDisplayChunkSize;
  if (end > kDisplayTotalBytes) {
    end = kDisplayTotalBytes;
  }
  
  for (size_t i = displayChunkOffset; i < end; i++) {
    Wire.write(display.getBuffer()[i]);
  }
  
  Wire.endTransmission();
  
  displayChunkOffset = end;
  
  // If we've sent the whole buffer, we're done
  if (displayChunkOffset >= kDisplayTotalBytes) {
    displayChunkOffset = 0;
    displayDirty = false;
  }
}

// The OLED state tag comes from CMRInet::remoteNodeStateTag(), beside
// the enum in RemoteNodeHandle.h -- one rendering, inside the library's
// -Werror=switch gate, instead of a copy per sketch that rots the next
// time the enum grows (#85, #93).

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
#ifndef TRACER_PHANTOM_ADDRESS
#define TRACER_PHANTOM_ADDRESS 32
#endif
#ifndef TRACER_PHANTOM_INPUT_BYTES
#define TRACER_PHANTOM_INPUT_BYTES 4
#endif
#ifndef TRACER_PHANTOM_OUTPUT_BYTES
#define TRACER_PHANTOM_OUTPUT_BYTES 4
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
// 0.7.0 (#86): every node verb names its UA, and the node verbs moved
// into the shared shell so both tracer images speak one vocabulary. New
// `node delete` / `node geometry`; `node add` works after begin() now
// that D5 unlocked the table. `status` reports host scope plus a roster;
// `status <ua>` reports one node. Telemetry carries the UA and never the
// wire byte, so the roster is keyed the way its readers key it (#90).
// 0.8.0 (#90): ring-dump PKT lines now emit semantic UA (0..127), never
// the CMRI wire byte. This keeps decoded telemetry uniform: framing and
// escaping are already removed when these lines are produced.
// 0.8.1 (#90): semantic-UA dump path now validates wire UA at the call
// site and marks invalid records explicitly instead of silently
// normalizing them into plausible semantic addresses.
// 0.9.0 (#87): D17 telemetry. Host lines carry the degraded-lane ledger
// (grants, per-gate denials, clamp bypasses); node lines carry the
// derived service class, the conformance breaker's position, and the
// conformance run. Two new events, breaker_open and breaker_close. An
// analyzer keying on 0.8.x will not find these fields, which is why the
// minor bumps rather than the patch.
constexpr const char* kVersion = "0.9.0"; // #87: degraded lane + breaker telemetry
constexpr int kTxenPin = D3;  // specific to the cpNode-Xiao board

CMRInet::Esp32UartCMRISerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                     TRACER_BAUD);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::testbed::TracerShell engine;

bool finished = false;  // quit latched: "final" emitted, polling parked


// ---- RAM Ring Buffer ----------------------------------------------------
struct RingRecord {
  uint32_t t_ms;
  uint8_t ua;
  uint8_t mt;
  uint8_t flags; // bit 0 = TX, bit 1 = invalid wire-UA at capture
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
constexpr uint8_t kMaxWireUa = static_cast<uint8_t>(CMRInet::kUaOffset + 127u);
bool isLegalWireUa(uint8_t wireUa) {
  return wireUa >= CMRInet::kUaOffset && wireUa <= kMaxWireUa;
}

/// Convert a legal CMRI wire-UA byte to semantic UA (0..127).
/// PRECONDITION: `wireUa` must satisfy isLegalWireUa(wireUa).

uint8_t toSemanticUa(uint8_t wireUa) {
  return static_cast<uint8_t>(wireUa - CMRInet::kUaOffset);
}

void ourOnTrace(void* context, bool transmit, const CMRInet::CMRIPacket& packet) {
  if (run_active) {
    if (packet.mt == 'I' || packet.mt == 'T') {
      run_it_frames++;
    }
    if (ring_used < kRingCap) {
      RingRecord& r = ring[ring_used++];
      const bool legalUa = isLegalWireUa(packet.ua);
      r.t_ms = millis();
      r.ua = legalUa ? toSemanticUa(packet.ua) : packet.ua;
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

// Set at setup() if either compiled-in node was rejected. Reported once
// at begin() rather than latched in the engine: each add already told us
// its own outcome (Design v1.2 D5).
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
// --------------------------------------------------------------------------

/// The CDC console seam for writeCdcLine (#99): binds the shared
/// src/testbed/CdcLineWriter.h logic to this sketch's Serial, millis,
/// and delay. The chunked write, the room check, and the reserved
/// terminator budget live in the header, under the desktop -Werror gate
/// and tests/test_cdc_line.cpp. The sketch keeps only this adapter.
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
constexpr uint8_t kGeneratorDefaultUa = TRACER_ADDRESS;
constexpr uint8_t kBenchNodeUa = 31;
constexpr uint8_t kBenchLoopbackByte = 2;
constexpr uint8_t kBenchLoopbackBit = 1;
constexpr uint16_t kBenchLoopbackBitIndex =
    static_cast<uint16_t>(kBenchLoopbackByte) * 8u + kBenchLoopbackBit;
constexpr size_t kGeneratorUaCount = 128;

struct FastWalkerGenerator {
  bool enabled = false;
  uint32_t period_ms = 250;
  uint8_t byte = 3;
  uint32_t last_ms = 0;
  uint8_t step = 0;
  void tick(uint32_t now_ms, CMRInet::RemoteNodeHandle* target) {
    if (!enabled || target == nullptr || byte >= target->outputLength()) return;
    if (now_ms - last_ms >= period_ms) {
      target->setOutputBit(byte * 8 + step, false);
      if (step > 0) {
        target->setOutputBit(byte * 8 + (step - 1), true);
      } else if (last_ms != 0) {
        target->setOutputBit(byte * 8 + 7, true);
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
  void tick(uint32_t now_ms, CMRInet::RemoteNodeHandle* target) {
    if (!enabled || target == nullptr || byte >= target->outputLength()) return;
    if (now_ms - last_ms >= period_ms) {
      target->setOutputBit(byte * 8 + step, true);
      if (step > 0) {
        target->setOutputBit(byte * 8 + (step - 1), false);
      } else if (last_ms != 0) {
        target->setOutputBit(byte * 8 + 7, false);
      }
      step = (step + 1) % 8;
      last_ms = now_ms;
    }
  }
};

struct ToggleOutFromInputGenerator {
  enum class Mode { kToggleOnRise, kWriteRead };
  bool enabled = false;
  uint16_t in_bit = 48;
  uint16_t out_bit = 32;
  Mode mode = Mode::kToggleOnRise;
  bool last_in = false;
  void tick(uint32_t now_ms, CMRInet::RemoteNodeHandle* target) {
    if (!enabled || target == nullptr) return;
    const size_t in_bits = target->inputLength() * 8u;
    const size_t out_bits = target->outputLength() * 8u;
    if (in_bit >= in_bits || out_bit >= out_bits) return;
    bool current_in = target->inputBit(in_bit);
    if (mode == Mode::kWriteRead) {
      if (target->outputBit(out_bit) != current_in) {
        target->setOutputBit(out_bit, current_in);
      }
    } else if (current_in && !last_in) {
      target->setOutputBit(out_bit, !target->outputBit(out_bit));
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

FastWalkerGenerator fastwalkerByUa[kGeneratorUaCount];
SlowWalkerGenerator slowwalkerByUa[kGeneratorUaCount];
ToggleOutFromInputGenerator toggleoutfrominputByUa[kGeneratorUaCount];
bool generatorDefaultsInitialized[kGeneratorUaCount] = {false};
StallGenerator stall;

void initializeGeneratorDefaults(uint8_t ua) {
  if (ua >= kGeneratorUaCount || generatorDefaultsInitialized[ua]) return;
  fastwalkerByUa[ua] = FastWalkerGenerator{};
  slowwalkerByUa[ua] = SlowWalkerGenerator{};
  toggleoutfrominputByUa[ua] = ToggleOutFromInputGenerator{};
  if (ua == kBenchNodeUa) {
    slowwalkerByUa[ua].byte = kBenchLoopbackByte;
    toggleoutfrominputByUa[ua].in_bit = kBenchLoopbackBitIndex;
    toggleoutfrominputByUa[ua].out_bit = kBenchLoopbackBitIndex;
    toggleoutfrominputByUa[ua].mode = ToggleOutFromInputGenerator::Mode::kWriteRead;
  }
  generatorDefaultsInitialized[ua] = true;
}

void disableAllNodeScopedGenerators() {
  for (size_t ua = 0; ua < kGeneratorUaCount; ++ua) {
    if (!generatorDefaultsInitialized[ua]) continue;
    fastwalkerByUa[ua].enabled = false;
    slowwalkerByUa[ua].enabled = false;
    toggleoutfrominputByUa[ua].enabled = false;
  }
}

size_t countEnabledFastwalker() {
  size_t count = 0;
  for (size_t ua = 0; ua < kGeneratorUaCount; ++ua) {
    if (generatorDefaultsInitialized[ua] && fastwalkerByUa[ua].enabled) ++count;
  }
  return count;
}

size_t countEnabledSlowwalker() {
  size_t count = 0;
  for (size_t ua = 0; ua < kGeneratorUaCount; ++ua) {
    if (generatorDefaultsInitialized[ua] && slowwalkerByUa[ua].enabled) ++count;
  }
  return count;
}

size_t countEnabledLoopback() {
  size_t count = 0;
  for (size_t ua = 0; ua < kGeneratorUaCount; ++ua) {
    if (generatorDefaultsInitialized[ua] && toggleoutfrominputByUa[ua].enabled) ++count;
  }
  return count;
}

void tickNodeScopedGenerators(uint32_t now_ms) {
  for (size_t ua = 0; ua < kGeneratorUaCount; ++ua) {
    if (!generatorDefaultsInitialized[ua]) continue;
    FastWalkerGenerator& fast = fastwalkerByUa[ua];
    SlowWalkerGenerator& slow = slowwalkerByUa[ua];
    ToggleOutFromInputGenerator& loopback = toggleoutfrominputByUa[ua];
    if (!fast.enabled && !slow.enabled && !loopback.enabled) continue;
    CMRInet::RemoteNodeHandle* target = host.node(static_cast<uint8_t>(ua));
    fast.tick(now_ms, target);
    slow.tick(now_ms, target);
    loopback.tick(now_ms, target);
  }
}

void emitGeneratorEvent(const char* event, const char* generator,
                        bool include_ua, uint8_t ua) {
  Serial.print("{\"event\":\"");
  Serial.print(event);
  Serial.print("\",\"generator\":\"");
  Serial.print(generator);
  if (include_ua) {
    Serial.print("\",\"ua\":");
    Serial.print(ua);
    Serial.println("}");
  } else {
    Serial.println("\"}");
  }
}

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

  const bool valid_gen = (strcmp(gen_name, "fastwalker") == 0 ||
                          strcmp(gen_name, "slowwalker") == 0 ||
                          strcmp(gen_name, "toggleoutfrominput") == 0 ||
                          strcmp(gen_name, "stall") == 0);
  if (!valid_gen) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"unknown generator '");
    Serial.print(gen_name);
    Serial.println("'\"}");
    return true;
  }

  const bool node_scoped_gen =
      (strcmp(gen_name, "fastwalker") == 0 ||
       strcmp(gen_name, "slowwalker") == 0 ||
       strcmp(gen_name, "toggleoutfrominput") == 0);
  ParsedGeneratorParams p;
  char* args = strtok_r(nullptr, "", &saveptr);
  if (args != nullptr) {
    p = parseGeneratorParams(args, gen_name);
    if (p.error_code) {
      Serial.print("{\"event\":\"error\",\"error\":\"");
      Serial.print(p.error_code);
      Serial.print("\",\"message\":\"Invalid parameter '");
      Serial.print(p.error_val);
      Serial.println("'\"}");
      return true;
    }
  }

  uint8_t target_ua = kGeneratorDefaultUa;
  if (node_scoped_gen && p.has_ua) target_ua = p.ua;
  if (node_scoped_gen) initializeGeneratorDefaults(target_ua);

  if (is_disable) {
    if (strcmp(gen_name, "fastwalker") == 0) fastwalkerByUa[target_ua].enabled = false;
    else if (strcmp(gen_name, "slowwalker") == 0) slowwalkerByUa[target_ua].enabled = false;
    else if (strcmp(gen_name, "toggleoutfrominput") == 0) toggleoutfrominputByUa[target_ua].enabled = false;
    else if (strcmp(gen_name, "stall") == 0) stall.enabled = false;
    emitGeneratorEvent("disable", gen_name, node_scoped_gen, target_ua);
    return true;
  }

  if (strcmp(gen_name, "fastwalker") == 0 || strcmp(gen_name, "slowwalker") == 0) {
    if (args != nullptr) {
      if (p.has_period) {
        if (strcmp(gen_name, "fastwalker") == 0) fastwalkerByUa[target_ua].period_ms = p.period_ms;
        else slowwalkerByUa[target_ua].period_ms = p.period_ms;
      }
      if (p.has_byte) {
        if (strcmp(gen_name, "fastwalker") == 0) fastwalkerByUa[target_ua].byte = p.byte_idx;
        else slowwalkerByUa[target_ua].byte = p.byte_idx;
      }
    }
  } else if (strcmp(gen_name, "toggleoutfrominput") == 0) {
    if (args != nullptr) {
      ToggleOutFromInputGenerator& loopback = toggleoutfrominputByUa[target_ua];
      if (p.has_in) loopback.in_bit = p.in_bit;
      if (p.has_out) loopback.out_bit = p.out_bit;
      if (p.has_src_byte && p.has_src_bit) {
        loopback.in_bit = static_cast<uint16_t>(p.src_byte) * 8u + p.src_bit;
      }
      if (p.has_dst_byte && p.has_dst_bit) {
        loopback.out_bit = static_cast<uint16_t>(p.dst_byte) * 8u + p.dst_bit;
      }
      if (p.has_loopback_mode) {
        loopback.mode = p.loopback_mode_write_read
            ? ToggleOutFromInputGenerator::Mode::kWriteRead
            : ToggleOutFromInputGenerator::Mode::kToggleOnRise;
      }
    }
  } else if (strcmp(gen_name, "stall") == 0) {
    if (args != nullptr) {
      if (p.has_stall_ms) stall.ms = p.stall_ms;
      if (p.has_period) stall.period_ms = p.period_ms;
      if (p.has_mode) {
        stall.mode = p.mode_busy
            ? StallGenerator::Mode::kBusy
            : StallGenerator::Mode::kYield;
      }
    }
  }

  if (is_enable) {
    lazyBegin();
    if (strcmp(gen_name, "fastwalker") == 0) {
      FastWalkerGenerator& fast = fastwalkerByUa[target_ua];
      fast.enabled = true;
      fast.last_ms = 0;
      fast.step = 0;
    } else if (strcmp(gen_name, "slowwalker") == 0) {
      SlowWalkerGenerator& slow = slowwalkerByUa[target_ua];
      slow.enabled = true;
      slow.last_ms = 0;
      slow.step = 0;
    } else if (strcmp(gen_name, "toggleoutfrominput") == 0) {
      ToggleOutFromInputGenerator& loopback = toggleoutfrominputByUa[target_ua];
      loopback.enabled = true;
      CMRInet::RemoteNodeHandle* target = host.node(target_ua);
      const size_t in_bits = target ? (target->inputLength() * 8u) : 0u;
      loopback.last_in = (target != nullptr && loopback.in_bit < in_bits)
          ? target->inputBit(loopback.in_bit)
          : false;
    } else if (strcmp(gen_name, "stall") == 0) {
      if (stall.ms == 0) stall.enabled = false;
      else {
        stall.enabled = true;
        stall.last_ms = 0;
      }
    }
  }

  emitGeneratorEvent(action, gen_name, node_scoped_gen, target_ua);
  return true;
}

/// Append the generator block to a status line.
///
/// This used to also emit a `nodes` array, which the shell now owns as
/// `roster`. Two reasons it had to move: the shell can keep it in step
/// with runtime membership changes, and the copy here keyed each entry by
/// `n->ua()` -- the *wire byte* (95), not the UA (30) -- so
/// analyze_bench_validation.py built its map on 95 and looked it up on
/// 30, missed every time, and left its UNINITIALIZED/OFFLINE check dead.
/// See #90.
void emitGeneratorsStatus(void* context, char* buffer, size_t remaining_capacity) {
  initializeGeneratorDefaults(kGeneratorDefaultUa);
  const FastWalkerGenerator& defaultFastwalker = fastwalkerByUa[kGeneratorDefaultUa];
  const SlowWalkerGenerator& defaultSlowwalker = slowwalkerByUa[kGeneratorDefaultUa];
  const ToggleOutFromInputGenerator& defaultLoopback = toggleoutfrominputByUa[kGeneratorDefaultUa];
  const char* loopbackMode =
      (defaultLoopback.mode == ToggleOutFromInputGenerator::Mode::kWriteRead)
          ? "write_read"
          : "toggle";

  snprintf(buffer, remaining_capacity, 
    ",\"generators\":{"
    "\"fastwalker\":{\"ua\":%u,\"enabled\":%s,\"period_ms\":%lu,\"byte\":%u,\"enabled_count\":%u},"
    "\"slowwalker\":{\"ua\":%u,\"enabled\":%s,\"period_ms\":%lu,\"byte\":%u,\"enabled_count\":%u},"
    "\"toggleoutfrominput\":{\"ua\":%u,\"enabled\":%s,\"in\":%u,\"out\":%u,\"mode\":\"%s\",\"enabled_count\":%u},"
    "\"stall\":{\"enabled\":%s,\"ms\":%lu,\"period_ms\":%lu,\"mode\":\"%s\"}"
    "}",
    static_cast<unsigned>(kGeneratorDefaultUa),
    defaultFastwalker.enabled ? "true" : "false",
    (unsigned long)defaultFastwalker.period_ms,
    defaultFastwalker.byte,
    static_cast<unsigned>(countEnabledFastwalker()),
    static_cast<unsigned>(kGeneratorDefaultUa),
    defaultSlowwalker.enabled ? "true" : "false",
    (unsigned long)defaultSlowwalker.period_ms,
    defaultSlowwalker.byte,
    static_cast<unsigned>(countEnabledSlowwalker()),
    static_cast<unsigned>(kGeneratorDefaultUa),
    defaultLoopback.enabled ? "true" : "false",
    defaultLoopback.in_bit,
    defaultLoopback.out_bit,
    loopbackMode,
    static_cast<unsigned>(countEnabledLoopback()),
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
  // Resolved at the point of use, not cached: the operator can delete
  // this node at runtime now, and a cached handle would keep drawing a
  // tombstone -- or a different device that reused its slot (D5).
  CMRInet::RemoteNodeHandle* node = host.node(TRACER_ADDRESS);
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

  const char* tag =
      (node != nullptr) ? CMRInet::remoteNodeStateTag(node->state()) : "---";
  const uint32_t latMs = (node != nullptr)
      ? node->statistics().lastTurnaroundMs : 0;
  char row[24];
  panel.nodeRowText(row, sizeof(row), now, 0,
                    TRACER_ADDRESS, tag, latMs);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print(row);
  
  // #64: print the bottom two lines if set
  if (displayLine1[0] != '\0') {
    display.setCursor(0, 48);
    display.print(displayLine1);
  }
  if (displayLine2[0] != '\0') {
    display.setCursor(0, 56);
    display.print(displayLine2);
  }
  
  displayDirty = true;
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
  const CMRInet::CMRIHost::ConfigStatus realStatus =
      host.addRemoteNode(TRACER_ADDRESS, nodeConfig);
  CMRInet::RemoteNodeConfig phantomNodeConfig;
  phantomNodeConfig.inputBytes = TRACER_PHANTOM_INPUT_BYTES;
  phantomNodeConfig.outputBytes = TRACER_PHANTOM_OUTPUT_BYTES;
  const CMRInet::CMRIHost::ConfigStatus phantomStatus =
      host.addRemoteNode(TRACER_PHANTOM_ADDRESS, phantomNodeConfig);
  setupStatus = (realStatus != CMRInet::CMRIHost::ConfigStatus::kOk)
      ? realStatus
      : phantomStatus;

  // Bind unconditionally: the shell holds no node (Design v1.2 D5), so
  // there is nothing for a rejected compiled-in add to invalidate -- and
  // a shell that failed to bind could not report the rejection.
  engine.bind(host, transport, kImage, kVersion,
              &CMRInet::testbed::writeCdcLineCb, &cdc);
  host.onTrace(ourOnTrace, nullptr);
  engine.setStatusExtender(emitGeneratorsStatus, nullptr);

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
    tickNodeScopedGenerators(nowMs);
    stall.tick(nowMs);
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
    if (strncmp(verb, "enable", 6) == 0 || 
        strncmp(verb, "disable", 7) == 0 || 
        strncmp(verb, "configure", 9) == 0) {
      handled = handleGeneratorControl(verbCopy);
    } else if (strncmp(verb, "node ", 5) == 0) {
      // The node verbs live in the shared shell now. Design v1.2 D5 made
      // add/delete/geometry engine operations, and the shell is where
      // both tracer images get them identically (issue #21) -- this
      // sketch's private copy could only ever drift from the desktop's.
      //
      // It also fixes two things the private copy got wrong: it refused
      // `node add` after begin() with a "locked" error that D5 retired,
      // and `node enable|disable` on an unknown UA printed success while
      // doing nothing.
      //
      // All this sketch still owns is the deferred begin(): a node verb
      // means the operator wants traffic, so the engine must be running
      // before the shell acts on it.
      lazyBegin();
      handled = false;  // fall through to the shell
    } else if (strncmp(verb, "display ", 8) == 0) {
      // #64: Allow the harness to inject custom annotations on the OLED
      if (oledOk) {
        char* saveptr = nullptr;
        strtok_r(verbCopy, " ", &saveptr); // "display"
        char* line_num_str = strtok_r(nullptr, " ", &saveptr); // line number
        char* text = strtok_r(nullptr, "\n", &saveptr); // rest of the line
        
        if (line_num_str && text) {
          int line_num = atoi(line_num_str);
          if (line_num == 1) {
            strncpy(displayLine1, text, sizeof(displayLine1) - 1);
            displayLine1[sizeof(displayLine1) - 1] = '\0';
          } else if (line_num == 2) {
            strncpy(displayLine2, text, sizeof(displayLine2) - 1);
            displayLine2[sizeof(displayLine2) - 1] = '\0';
          }
          // Trigger an immediate update instead of waiting for the next timer tick
          drawHostStatus();
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
        Serial.print("ua="); Serial.print(r.ua);
        if ((r.flags & kRingFlagInvalidUa) != 0u) {
          Serial.print(" ua_invalid=1");
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
      disableAllNodeScopedGenerators();
      stall.enabled = false;
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
        finished = true;  // reset the board to run again
      }
    }
  }

  // Redraw the OLED on a timer.
  if (nowMs - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawHostStatus();
    lastDisplayMs = nowMs;
  }
  
  // Process the display update incrementally
  updateDisplayIncremental();
}
