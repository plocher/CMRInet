// TracerHost.ino — the stage-2 Xiao Host R&D image (map issue
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
//   status | status <UA>
//   quiesce <UA> | resume <UA> | forcetx <UA>
//   setbit <UA> <bit> <0|1> | writeoutputs <UA> <hex>
//   node add <UA> <type> ...     type C|M|N|X + type-specific INIT
//                                C: <in> <out> [opts1 [opts2]]
//                                M: [ns [ct0..ct5]]
//                                N|X: <ns> <in> <out> [ct x ns]
//   node delete <UA>
//   node geometry <UA> <in> <out>   (CPNODE NI/NO reshape)
//   node enable <UA> | node disable <UA>
//   enable|disable|configure <generator> [UA <n>] ...
// Membership is only via node add/delete — this sketch seeds none.
//   run <secs> | dump | reset | reboot | display <1|2> <text> | quit
// After quit the image emits "final" and parks; reset the board to run
// again.

#include <Arduino.h>

#include "CMRIHost.h"
#include "transport/serial.h"
#include "transport/serialESP32.h"
#include "testbed/TracerShell.h"
#include "testbed/CdcLineWriter.h"  // #99: shared, testable CDC line writer

// ---- OLED diagnostic display (#11)
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"          // shared HostStatusPanel
#include "Ssd1306SegmentedFlush.h"      // non-blocking OLED push

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;

// #64: OLED custom annotations
char displayLine1[22] = {0};
char displayLine2[22] = {0};

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
CMRInet::examples::Ssd1306SegmentedFlush oledFlush(display, kScreenAddr);
bool oledOk = false;
uint32_t lastDisplayMs = 0;
CMRInet::examples::HostStatusPanel panel;

// The OLED state tag comes from CMRInet::remoteNodeStateTag(), beside
// the enum in RemoteNodeHandle.h -- one rendering, inside the library's
// -Werror=switch gate, instead of a copy per sketch that rots the next
// time the enum grows (#85, #93).

// Build-time knobs for the *transport only* (not membership).
// Override with build.defines, NOT build.extra_flags (esp32 core owns
// build.extra_flags; overriding it can drop -DARDUINO_USB_CDC_ON_BOOT=1).
#ifndef TRACER_BAUD
#define TRACER_BAUD 28800
#endif
#ifndef TRACER_INTER_BYTE_TIMEOUT_MS
#define TRACER_INTER_BYTE_TIMEOUT_MS 50  // 0 disables (interop 2.2.6 exception)
#endif

namespace {

constexpr const char* kImage = "tracer_host";
// 0.1.1: hardware transmit-drain truth (Esp32SerialPort) — the
// ~2 s C6 runtime stall made the estimate-based drain drop TXEN mid-ETX.
// 0.1.2: 50 ms inter-byte tolerance — the same stall splits intact
// replies at the tick level; the rate-derived timeout misread the gap.
// 0.1.3 (#27): Esp32SerialPort promoted from this sketch into
// the library (src/); the library's inter-byte abort doctrine now
// ships a tolerant default (Design D13). This image keeps its explicit
// 50 ms override, so runtime behavior is unchanged from 0.1.2.
// 0.2.0: I/T bench slice (map issue #30) — onTrace packet telemetry and
// output verbs (setbit/writeoutputs/forcetx) so T is exercisable from C&C.
// 0.3.0: Add generator-control verbs (enable, disable, configure) for
// fastwalker, slowwalker, toggleoutfrominput, and stall stimulus (#55).
// 0.4.0: Capture-mode ring + run/dump/reset + runtime node topology (#47).
// 0.7.0 (#86): every node verb names its UA, and the node verbs moved
// into the shared shell so both tracer images speak one vocabulary. New
// `node delete` / `node geometry`; `node add` works after begin() now
// that D5 unlocked the table. `status` reports host scope plus a roster;
// `status <UA>` reports one node. Telemetry carries the UA and never the
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
// 0.9.1: bare `status` is a multi-line bundle (status/roster/generators)
// so CDC no longer truncates the host ledger under backpressure.
// 0.9.2: Esp32SerialPort ctor is (stream, txen, baud, rx, tx) — the
// leftover UART_NUM_1 arg made baud=D3 and pins=28800, which the ESP
// UART driver rejected as "baud rate unachievable" and left polls=0.
// 0.10.0 (#112): dense full-T Host belief timeline — live miss/reject/
// xchg/unsolicited during `run`, gate/kind on those lines, transport
// snapshot on miss/reject, T body `fp` on packet traces.
// 0.10.1 (#112): echo-cancel discards only own-frame wire length;
// post-deassert RX (incl. prompt R after ETX, interop 2.3.15 / E10)
// is not treated as endless self-echo. One-char drain hold unchanged.
// 0.10.2 (#112): accept matching R while P is still kAwaitSendComplete;
// Esp32 hardwareTransmitDrain ends TXEN without estimate veto.
constexpr const char* kVersion = "0.11.0"; // typed C&C membership; no seed topology
constexpr int kTxenPin = D3;  // specific to the cpNode-Xiao board

CMRInet::Esp32SerialPort port(Serial1, kTxenPin, TRACER_BAUD,
                                     RX /* D7 */, TX /* D6 */);
CMRInet::SerialCMRITransport transport(port);
CMRInet::CMRIHost host(transport);
CMRInet::testbed::TracerShell engine;

bool finished = false;  // quit latched: "final" emitted, polling parked


// ---- RAM Ring Buffer ----------------------------------------------------
struct RingRecord {
  uint32_t t_ms;
  uint8_t UA;
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

void ourOnTrace(void* context, bool transmit, const CMRInet::CMRIPacket& packet) {
  if (run_active) {
    if (packet.mt == 'I' || packet.mt == 'T') {
      run_it_frames++;
    }
    if (ring_used < kRingCap) {
      RingRecord& r = ring[ring_used++];
      const bool legalUa = CMRInet::isLegalWireUA(packet.wireUA);
      r.t_ms = millis();
      r.UA = legalUa ? CMRInet::toSemanticUA(packet.wireUA) : packet.wireUA;
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
// ---- Services (HostServices + Orchestrator) -------------------------------
// One walker type (BitWalkerService). fastwalker/slowwalker are C&C aliases
// that only set different defaults (invert/period/byte).
// Pool of walker and toggle slots. Orchestrator ticks the pool.
#include "GeneratorParser.h"
#include "HostServices.h"

using CMRInet::app::BitWalkerConfig;
using CMRInet::app::BitWalkerService;
using CMRInet::app::InputToggleConfig;
using CMRInet::app::InputToggleMode;
using CMRInet::app::InputToggleService;
using CMRInet::app::Orchestrator;
using CMRInet::app::StallConfig;
using CMRInet::app::StallMode;
using CMRInet::app::StallService;

constexpr size_t kMaxWalkers = 16;
constexpr size_t kMaxToggles = 8;

BitWalkerService walkers[kMaxWalkers];
bool walkerLive[kMaxWalkers] = {false};
InputToggleService toggles[kMaxToggles];
bool toggleLive[kMaxToggles] = {false};
StallService stallService;
Orchestrator g_orchestrator;
bool servicesReady = false;

void setupServices() {
  if (servicesReady) return;
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    walkers[i].setEnabled(false);
    g_orchestrator.add(&walkers[i]);
  }
  for (size_t i = 0; i < kMaxToggles; ++i) {
    toggles[i].setEnabled(false);
    g_orchestrator.add(&toggles[i]);
  }
  stallService.setEnabled(false);
  g_orchestrator.add(&stallService);
  servicesReady = true;
}

int findWalker(uint8_t ua, uint8_t byteIdx) {
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    if (!walkerLive[i]) continue;
    const BitWalkerConfig& c = walkers[i].config();
    if (c.nodeUA == ua && c.byte == byteIdx) return static_cast<int>(i);
  }
  return -1;
}

int allocWalker() {
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    if (!walkerLive[i]) return static_cast<int>(i);
  }
  return -1;
}

int findToggle(uint8_t ua) {
  for (size_t i = 0; i < kMaxToggles; ++i) {
    if (!toggleLive[i]) continue;
    if (toggles[i].config().inNodeUA == ua) return static_cast<int>(i);
  }
  return -1;
}

int allocToggle() {
  for (size_t i = 0; i < kMaxToggles; ++i) {
    if (!toggleLive[i]) return static_cast<int>(i);
  }
  return -1;
}

void disableAllNodeScopedServices() {
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    walkers[i].setEnabled(false);
    walkerLive[i] = false;
  }
  for (size_t i = 0; i < kMaxToggles; ++i) {
    toggles[i].setEnabled(false);
    toggleLive[i] = false;
  }
}

size_t countEnabledWalkers(bool inverted) {
  size_t n = 0;
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    if (walkerLive[i] && walkers[i].enabled() &&
        walkers[i].config().inverted == inverted)
      ++n;
  }
  return n;
}

size_t countEnabledToggles() {
  size_t n = 0;
  for (size_t i = 0; i < kMaxToggles; ++i)
    if (toggleLive[i] && toggles[i].enabled()) ++n;
  return n;
}

void emitGeneratorEvent(const char* event, const char* generator,
                        bool include_ua, uint8_t UA) {
  Serial.print("{\"event\":\"");
  Serial.print(event);
  Serial.print("\",\"generator\":\"");
  Serial.print(generator);
  if (include_ua) {
    Serial.print("\",\"ua\":");
    Serial.print(UA);
    Serial.println("}");
  } else {
    Serial.println("\"}");
  }
}

bool handleGeneratorControl(char* cmd) {
  setupServices();

  char* saveptr = nullptr;
  char* action = strtok_r(cmd, " ", &saveptr);
  if (!action) return false;

  const bool is_enable = (strcmp(action, "enable") == 0);
  const bool is_disable = (strcmp(action, "disable") == 0);
  const bool is_configure = (strcmp(action, "configure") == 0);
  if (!is_enable && !is_disable && !is_configure) return false;

  char* gen_name = strtok_r(nullptr, " ", &saveptr);
  if (!gen_name) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"");
    Serial.print(action);
    Serial.println(": missing generator\"}");
    return true;
  }

  // walker is the real service. fastwalker/slowwalker are aliases.
  const bool is_fast = (strcmp(gen_name, "fastwalker") == 0);
  const bool is_slow = (strcmp(gen_name, "slowwalker") == 0);
  const bool is_walker = (strcmp(gen_name, "walker") == 0) || is_fast || is_slow;
  const bool is_toggle = (strcmp(gen_name, "toggleoutfrominput") == 0);
  const bool is_stall = (strcmp(gen_name, "stall") == 0);
  if (!is_walker && !is_toggle && !is_stall) {
    Serial.print("{\"event\":\"error\",\"error\":\"badVerb\",\"message\":\"unknown generator '");
    Serial.print(gen_name);
    Serial.println("'\"}");
    return true;
  }

  const bool node_scoped = is_walker || is_toggle;
  const char* report_name = gen_name;
  if (is_walker && strcmp(gen_name, "walker") == 0) report_name = "walker";

  ParsedGeneratorParams p;
  char* args = strtok_r(nullptr, "", &saveptr);
  if (args != nullptr) {
    // Parser still keys on fastwalker/slowwalker/toggle/stall names.
    const char* parse_as = gen_name;
    if (strcmp(gen_name, "walker") == 0) parse_as = "fastwalker";
    p = parseGeneratorParams(args, parse_as);
    if (p.error_code) {
      Serial.print("{\"event\":\"error\",\"error\":\"");
      Serial.print(p.error_code);
      Serial.print("\",\"message\":\"Invalid parameter '");
      Serial.print(p.error_val);
      Serial.println("'\"}");
      return true;
    }
  }

  uint8_t target_ua = 0;
  if (node_scoped) {
    if (!p.has_UA) {
      Serial.println(
          "{\"event\":\"error\",\"error\":\"badVerb\","
          "\"message\":\"node-scoped service needs UA <n>\"}");
      return true;
    }
    target_ua = p.UA;
  }

  if (is_stall) {
    if (is_disable) {
      stallService.setEnabled(false);
      emitGeneratorEvent("disable", "stall", false, 0);
      return true;
    }
    StallConfig cfg = stallService.config();
    if (p.has_stall_ms) cfg.stallMs = p.stall_ms;
    if (p.has_period) cfg.periodMs = p.period_ms;
    if (p.has_mode)
      cfg.mode = p.mode_busy ? StallMode::kBusy : StallMode::kYield;
    stallService.setConfig(cfg);
    if (is_enable)
      stallService.setEnabled(cfg.stallMs != 0);
    emitGeneratorEvent(action, "stall", false, 0);
    return true;
  }

  if (is_walker) {
    bool invert = is_fast ? true : false;
    if (is_slow) invert = false;
    // bare "walker" defaults to non-inverted unless invert set later
    if (strcmp(gen_name, "walker") == 0) invert = false;

    uint8_t byteIdx = is_fast ? 3 : (is_slow ? 5 : 3);
    uint32_t period = is_fast ? 250 : (is_slow ? 1000 : 250);
    if (p.has_byte) byteIdx = p.byte_idx;
    if (p.has_period) period = p.period_ms;

    if (is_disable) {
      if (p.has_byte) {
        const int idx = findWalker(target_ua, byteIdx);
        if (idx >= 0) {
          walkers[idx].setEnabled(false);
          walkerLive[idx] = false;
        }
      } else {
        for (size_t i = 0; i < kMaxWalkers; ++i) {
          if (!walkerLive[i]) continue;
          if (walkers[i].config().nodeUA != target_ua) continue;
          if (is_fast && !walkers[i].config().inverted) continue;
          if (is_slow && walkers[i].config().inverted) continue;
          walkers[i].setEnabled(false);
          walkerLive[i] = false;
        }
      }
      emitGeneratorEvent("disable", report_name, true, target_ua);
      return true;
    }

    int idx = findWalker(target_ua, byteIdx);
    if (idx < 0) idx = allocWalker();
    if (idx < 0) {
      Serial.println(
          "{\"event\":\"error\",\"error\":\"noSlot\","
          "\"message\":\"walker pool full\"}");
      return true;
    }

    BitWalkerConfig cfg;
    cfg.nodeUA = target_ua;
    cfg.byte = byteIdx;
    cfg.startBit = 0;
    cfg.bitsCount = 8;
    cfg.periodMs = period;
    cfg.inverted = invert;
    walkers[idx].setConfig(cfg);
    walkerLive[idx] = true;
    if (is_enable) {
      lazyBegin();
      walkers[idx].setEnabled(true);
    }
    emitGeneratorEvent(action, report_name, true, target_ua);
    return true;
  }

  if (is_toggle) {
    if (is_disable) {
      const int idx = findToggle(target_ua);
      if (idx >= 0) {
        toggles[idx].setEnabled(false);
        toggleLive[idx] = false;
      }
      emitGeneratorEvent("disable", gen_name, true, target_ua);
      return true;
    }

    int idx = findToggle(target_ua);
    if (idx < 0) idx = allocToggle();
    if (idx < 0) {
      Serial.println(
          "{\"event\":\"error\",\"error\":\"noSlot\","
          "\"message\":\"toggle pool full\"}");
      return true;
    }

    InputToggleConfig cfg = toggles[idx].config();
    cfg.inNodeUA = target_ua;
    cfg.outNodeUA = target_ua;
    if (p.has_in) {
      cfg.inByte = static_cast<uint8_t>(p.in_bit / 8u);
      cfg.inBit = static_cast<uint8_t>(p.in_bit % 8u);
    }
    if (p.has_out) {
      cfg.outByte = static_cast<uint8_t>(p.out_bit / 8u);
      cfg.outBit = static_cast<uint8_t>(p.out_bit % 8u);
    }
    if (p.has_src_byte && p.has_src_bit) {
      cfg.inByte = p.src_byte;
      cfg.inBit = p.src_bit;
    }
    if (p.has_dst_byte && p.has_dst_bit) {
      cfg.outByte = p.dst_byte;
      cfg.outBit = p.dst_bit;
    }
    if (p.has_loopback_mode) {
      cfg.mode = p.loopback_mode_write_read
          ? InputToggleMode::kLevelFollow
          : InputToggleMode::kToggleOnRise;
    }
    toggles[idx].setConfig(cfg);
    toggleLive[idx] = true;
    if (is_enable) {
      lazyBegin();
      toggles[idx].setEnabled(true);
    }
    emitGeneratorEvent(action, gen_name, true, target_ua);
    return true;
  }

  return true;
}

void emitGeneratorsStatus(void* /*context*/, char* buffer,
                          size_t remaining_capacity) {
  // Report first enabled inverted walker as "fastwalker", first non-inverted
  // as "slowwalker", first toggle as toggleoutfrominput.
  const BitWalkerService* fast = nullptr;
  const BitWalkerService* slow = nullptr;
  const InputToggleService* tog = nullptr;
  uint8_t fastUa = 0, slowUa = 0, togUa = 0;
  for (size_t i = 0; i < kMaxWalkers; ++i) {
    if (!walkerLive[i] || !walkers[i].enabled()) continue;
    if (walkers[i].config().inverted && !fast) {
      fast = &walkers[i];
      fastUa = walkers[i].config().nodeUA;
    } else if (!walkers[i].config().inverted && !slow) {
      slow = &walkers[i];
      slowUa = walkers[i].config().nodeUA;
    }
  }
  for (size_t i = 0; i < kMaxToggles; ++i) {
    if (!toggleLive[i] || !toggles[i].enabled()) continue;
    tog = &toggles[i];
    togUa = toggles[i].config().inNodeUA;
    break;
  }

  BitWalkerConfig z{};
  const BitWalkerConfig& fc = fast ? fast->config() : z;
  const BitWalkerConfig& sc = slow ? slow->config() : z;
  const char* loopMode = "toggle";
  uint16_t inBit = 0, outBit = 0;
  if (tog) {
    const InputToggleConfig& c = tog->config();
    inBit = static_cast<uint16_t>(c.inByte) * 8u + c.inBit;
    outBit = static_cast<uint16_t>(c.outByte) * 8u + c.outBit;
    loopMode = (c.mode == InputToggleMode::kLevelFollow) ? "write_read"
                                                         : "toggle";
  }
  const StallConfig& stc = stallService.config();

  snprintf(
      buffer, remaining_capacity,
      ",\"generators\":{"
      "\"fastwalker\":{\"ua\":%u,\"enabled\":%s,\"period_ms\":%lu,\"byte\":%u,"
      "\"enabled_count\":%u},"
      "\"slowwalker\":{\"ua\":%u,\"enabled\":%s,\"period_ms\":%lu,\"byte\":%u,"
      "\"enabled_count\":%u},"
      "\"toggleoutfrominput\":{\"ua\":%u,\"enabled\":%s,\"in\":%u,\"out\":%u,"
      "\"mode\":\"%s\",\"enabled_count\":%u},"
      "\"stall\":{\"enabled\":%s,\"ms\":%lu,\"period_ms\":%lu,\"mode\":\"%s\"}"
      "}",
      static_cast<unsigned>(fastUa), fast ? "true" : "false",
      (unsigned long)fc.periodMs, fc.byte,
      static_cast<unsigned>(countEnabledWalkers(true)),
      static_cast<unsigned>(slowUa), slow ? "true" : "false",
      (unsigned long)sc.periodMs, sc.byte,
      static_cast<unsigned>(countEnabledWalkers(false)),
      static_cast<unsigned>(togUa), tog ? "true" : "false",
      inBit, outBit, loopMode,
      static_cast<unsigned>(countEnabledToggles()),
      stallService.enabled() ? "true" : "false",
      (unsigned long)stc.stallMs, (unsigned long)stc.periodMs,
      stc.mode == StallMode::kYield ? "yield" : "busy");
}

void drawHostStatus() {
  if (!oledOk) return;
  const uint32_t now = millis();

  constexpr size_t kMaxRows = 4;
  uint8_t uas[kMaxRows] = {};
  size_t nRows = 0;
  for (unsigned ua = 0; ua <= 127u && nRows < kMaxRows; ++ua) {
    if (host.node(static_cast<uint8_t>(ua)) != nullptr)
      uas[nRows++] = static_cast<uint8_t>(ua);
  }

  const auto& hs = host.statistics();
  uint32_t nodeErrs[kMaxRows] = {};
  uint32_t nodeMisses[kMaxRows] = {};
  for (size_t i = 0; i < nRows; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(uas[i]);
    if (n != nullptr) {
      nodeErrs[i] = n->statistics().errors;
      nodeMisses[i] = n->statistics().noReplies;
    }
  }
  panel.sample(now, hs.pollsSent, hs.repliesAccepted, nodeErrs, nodeMisses,
               nRows > 0 ? nRows : 1);

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

  char totals[24];
  panel.hostTotalsText(totals, sizeof(totals), now);
  display.setCursor(0, 18);
  display.print(totals);

  for (size_t i = 0; i < nRows; ++i) {
    CMRInet::RemoteNodeHandle* node = host.node(uas[i]);
    const bool online =
        (node != nullptr) &&
        (node->state() == CMRInet::RemoteNodeState::kOnline);
    const char* tag =
        (node != nullptr) ? CMRInet::remoteNodeStateTag(node->state()) : "---";
    const uint32_t latMs =
        (node != nullptr) ? node->statistics().lastTurnaroundMs : 0;
    char row[28];
    panel.nodeRowText(row, sizeof(row), now, i, uas[i], online, tag, latMs);
    display.setTextSize(1);
    display.setCursor(0, 30 + static_cast<int>(i) * 10);
    display.print(row);
  }

  if (displayLine1[0] != '\0') {
    display.setCursor(0, 48);
    display.print(displayLine1);
  }
  if (displayLine2[0] != '\0') {
    display.setCursor(0, 56);
    display.print(displayLine2);
  }

  oledFlush.markDirty();
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

  // The CMRI wire is configured by Esp32SerialPort::begin() (baud,
  // 8N2, RX/TX pins) when host.begin()/lazyBegin() runs. Do not call
  // Serial1.begin() here — a second begin with different args races the
  // port and can leave the UART unusable.
  // Tick-gap tolerance; see the header comment. Survives begin().
  transport.setInterByteTimeoutMs(TRACER_INTER_BYTE_TIMEOUT_MS);

  // OLED diagnostic display (#11).
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) {
    display.dim(true);
  }

  // Membership is C&C-driven (probes send `node add`). No permanent
  // compiled-in topology: a probe owns the bench view and can delete/re-add.
  // Optional empty boot — host.begin is still deferred via lazyBegin().
  setupStatus = CMRInet::CMRIHost::ConfigStatus::kOk;
  setupServices();

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
    g_orchestrator.tick(host, nowMs);
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
        // Keep miss/reject/xchg/unsolicited live during capture (#112);
        // packet traces still go only to the ring via ourOnTrace.
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
      disableAllNodeScopedServices();
      stallService.setEnabled(false);
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

// Paint on a timer; drain one I2C chunk every loop.
  if (oledOk && (nowMs - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0)) {
    drawHostStatus();
    lastDisplayMs = nowMs;
  }
  if (oledOk) {
    oledFlush.service();
  }
}
