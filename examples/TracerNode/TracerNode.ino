// TracerNode.ino — the bench C&C Node test mule.
//
// The Node-side counterpart to XiaoHostTracer: a CMRINode on the
// cpNode-Xiao RS-485 block, command-and-control over USB CDC. It
// reacts to I/T/P from a Host and emits R replies, with packet trace
// telemetry so the bench can verify the full round trip.
//
// What it does:
// - Receives I (init), T (transmit/output), and P (poll) from a Host.
// - Replies with R (receive/input) on every poll, carrying a test
//   pattern the Host-side tracer can verify.
// - Emits JSON-lines packet trace over USB CDC (one line per I/T/P/R).
// - Supports capture mode (run/dump/reset) for timed captures.
// - Reports status (UA, geometry, counters) on demand.
//
// C&C verbs on the CDC stream:
//   status | run <secs> | dump | reset | reboot | quit
//
// Board: cpNode-Xiao (Seeed XIAO ESP32-C6 + MAX3491, full duplex):
//   D7 - RX   CMRI RS485 receive
//   D6 - TX   CMRI RS485 transmit
//   D3 - TXEN RS422/485 transmit enable
//   D4 - SDA  I2C (optional OLED)
//   D5 - SCL  I2C (optional OLED)
//
// No OTA, no WiFi in this image. The OLED shows the node's UA and
// activity spinners, same diagnostic panel as the other bench sketches.
//
// Build-time knobs (overridable via --build-property "build.defines=..."):
//   TRACER_NODE_UA, TRACER_NODE_INPUT_BYTES,
//   TRACER_NODE_OUTPUT_BYTES, TRACER_NODE_BAUD

#include <Arduino.h>

#include "CMRINode.h"
#include "transport/serial.h"
#include "transport/serialESP32.h"
#include "testbed/CdcLineWriter.h"  // #99: shared, testable CDC line writer

// ---- OLED diagnostic display
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SimpleHostMetrics.h"  // shared HostStatusPanel (header-only helpers)

constexpr int      kScreenW       = 128;
constexpr int      kScreenH       = 64;
constexpr int      kScreenAddr    = 0x3C;
constexpr uint32_t kDisplayRefreshMs = 150;

Adafruit_SSD1306 display(kScreenW, kScreenH, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;

// ---- Build knobs
#ifndef TRACER_NODE_UA
#define TRACER_NODE_UA 31           // bench node UA; wire UA = UA + 65
#endif
#ifndef TRACER_NODE_INPUT_BYTES
#define TRACER_NODE_INPUT_BYTES 2   // bench node: small geometry
#endif
#ifndef TRACER_NODE_OUTPUT_BYTES
#define TRACER_NODE_OUTPUT_BYTES 2
#endif
#ifndef TRACER_NODE_BAUD
#define TRACER_NODE_BAUD 28800
#endif
#ifndef TRACER_NODE_INTER_BYTE_TIMEOUT_MS
#define TRACER_NODE_INTER_BYTE_TIMEOUT_MS 50  // 0 disables
#endif

constexpr const char* kImage   = "tracer_node";
constexpr const char* kVersion = "0.1.0";
constexpr int kTxenPin = D3;

// ---- Wiring
CMRInet::Esp32SerialPort port(Serial1, UART_NUM_1, kTxenPin,
                                       TRACER_NODE_BAUD);
CMRInet::SerialCMRITransport    transport(port);

CMRInet::CMRINodeConfig makeConfig() {
  CMRInet::CMRINodeConfig cfg;
  cfg.ua          = TRACER_NODE_UA;
  cfg.nodeType    = 'C';
  cfg.inputBytes  = TRACER_NODE_INPUT_BYTES;
  cfg.outputBytes = TRACER_NODE_OUTPUT_BYTES;
  return cfg;
}

CMRInet::CMRINode node(transport, makeConfig());

// ---- Counters
uint32_t pollCount   = 0;  // P received
uint32_t initCount   = 0;  // I received
uint32_t txCount     = 0;  // T received
uint32_t replyCount  = 0;  // R sent

// ---- CDC console seam (#99)
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

// ---- RAM Ring Buffer (capture mode)
struct RingRecord {
  uint32_t t_ms;
  uint8_t  UA;
  uint8_t  mt;
  uint8_t  flags;  // bit 0 = TX (sent by us), bit 1 = invalid wire-UA
  uint8_t  len;
} __attribute__((packed));

constexpr size_t kRingCap = 12000;
RingRecord ring[kRingCap];
size_t  ring_used = 0;
bool    run_active = false;
uint32_t run_end_ms = 0;
uint32_t run_start_ms = 0;
constexpr uint8_t kRingFlagTx = 0x01u;
constexpr uint8_t kRingFlagInvalidUa = 0x02u;
constexpr uint8_t kMaxWireUA = static_cast<uint8_t>(CMRInet::kWireUAOffset + 127u);

uint32_t seq = 0;
uint32_t epochMs = 0;
bool finished = false;

// ---- onTrace: JSON-lines or ring capture
void ourOnTrace(void*, bool transmit, const CMRInet::CMRIPacket& packet) {
  const bool legalUa = CMRInet::isLegalWireUA(packet.wireUA);
  const uint8_t semUA = legalUa
      ? static_cast<uint8_t>(packet.wireUA - CMRInet::kWireUAOffset)
      : packet.wireUA;

  if (transmit) replyCount++;
  else {
    switch (packet.mt) {
      case 'I': initCount++; break;
      case 'T': txCount++;   break;
      case 'P': pollCount++; break;
      default: break;
    }
  }

  if (run_active) {
    if (ring_used < kRingCap) {
      RingRecord& r = ring[ring_used++];
      r.t_ms  = millis();
      r.UA    = semUA;
      r.mt    = packet.mt;
      r.flags = transmit ? kRingFlagTx : 0u;
      if (!legalUa) r.flags |= kRingFlagInvalidUa;
      r.len   = packet.length;
    }
  } else {
    // Stream JSON-lines trace
    char bodyHex[2 * CMRInet::kMaxBody + 1] = "";
    const size_t blen = packet.length;
    for (size_t i = 0; i < blen; ++i) {
      snprintf(&bodyHex[2 * i], 3, "%02X", packet.body[i]);
    }
    const char mtBuf[2] = {static_cast<char>(packet.mt), '\0'};
    Serial.print("{\"seq\":");
    Serial.print(static_cast<unsigned long>(++seq));
    Serial.print(",\"ts\":");
    Serial.print(static_cast<unsigned long>(millis() - epochMs));
    Serial.print(",\"event\":\"trace\",\"role\":\"node\",\"dir\":\"");
    Serial.print(transmit ? "tx" : "rx");
    Serial.print("\",\"wireUA\":");
    Serial.print(static_cast<unsigned>(packet.wireUA));
    Serial.print(",\"legal\":");
    Serial.print(legalUa ? "true" : "false");
    Serial.print(",\"UA\":");
    if (legalUa) {
      Serial.print(static_cast<unsigned>(semUA));
    } else {
      Serial.print("null");
    }
    Serial.print(",\"mt\":\"");
    Serial.print(mtBuf);
    Serial.print("\",\"body\":\"");
    Serial.print(bodyHex);
    Serial.println("\"}");
  }
}

// ---- CDC verb reader (same pattern as XiaoHostTracer)
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
    if (used < sizeof(buffer) - 1) buffer[used++] = c;
  }
  return false;
}

void emitEpoch() {
  epochMs = millis();
  Serial.print("{\"seq\":0,\"ts\":0,\"event\":\"epoch\",\"image\":\"");
  Serial.print(kImage);
  Serial.print("\",\"version\":\"");
  Serial.print(kVersion);
  Serial.print("\",\"ua\":");
  Serial.print(static_cast<unsigned>(node.UA()));
  Serial.print(",\"in\":");
  Serial.print(static_cast<unsigned>(node.inputLength()));
  Serial.print(",\"out\":");
  Serial.print(static_cast<unsigned>(node.outputLength()));
  Serial.println("}");
}

void emitStatus() {
  Serial.print("{\"seq\":");
  Serial.print(static_cast<unsigned long>(++seq));
  Serial.print(",\"ts\":");
  Serial.print(static_cast<unsigned long>(millis() - epochMs));
  Serial.print(",\"event\":\"status\",\"role\":\"node\",\"ua\":");
  Serial.print(static_cast<unsigned>(node.UA()));
  Serial.print(",\"in\":");
  Serial.print(static_cast<unsigned>(node.inputLength()));
  Serial.print(",\"out\":");
  Serial.print(static_cast<unsigned>(node.outputLength()));
  Serial.print(",\"polls\":");
  Serial.print(static_cast<unsigned long>(pollCount));
  Serial.print(",\"inits\":");
  Serial.print(static_cast<unsigned long>(initCount));
  Serial.print(",\"txs\":");
  Serial.print(static_cast<unsigned long>(txCount));
  Serial.print(",\"replies\":");
  Serial.print(static_cast<unsigned long>(replyCount));
  Serial.println("}");
}

// ---- pack: fill IB with a test pattern (walking bit)
void packInputs(void*, uint8_t* ib, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    ib[i] = static_cast<uint8_t>((millis() / 500u + i) & 0xFFu);
  }
}

// ---- unpack: store OB (count for diagnostic)
void unpackOutputs(void*, const uint8_t* ob, size_t len) {
  (void)ob;
  (void)len;
  // The output image is already stored by the engine; we can read
  // it via node.outputBit()/outputByte() anytime. This callback is
  // the place to drive pins if the node has physical outputs.
}

// ---- OLED display
CMRInet::examples::HostStatusPanel panel;

void drawStatus() {
  if (!oledOk) return;
  const uint32_t now = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("NODE"));
  display.setTextSize(1);
  display.setCursor(60, 4);
  display.print(F("UA"));
  display.print(node.UA());

  // Activity line
  display.setCursor(0, 20);
  display.print(F("P:"));
  display.print(static_cast<unsigned long>(pollCount));
  display.print(F(" R:"));
  display.print(static_cast<unsigned long>(replyCount));

  display.setCursor(0, 32);
  display.print(F("I:"));
  display.print(static_cast<unsigned long>(initCount));
  display.print(F(" T:"));
  display.print(static_cast<unsigned long>(txCount));

  // Output image hex
  display.setCursor(0, 48);
  for (size_t i = 0; i < node.outputLength(); ++i) {
    if (node.outputByte(i) < 0x10) display.print('0');
    display.print(node.outputByte(i), HEX);
    display.print(' ');
  }

  display.display();
}

// ---- Setup
void setup() {
  Serial.begin(115200);  // USB CDC: command-and-control + telemetry
  Serial.setTxTimeoutMs(0);
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setRxBufferSize(1024);
#endif

  // Wait for CDC (bounded so headless boards still boot)
  const uint32_t kSerialWaitMs = 3000;
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < kSerialWaitMs) {
    delay(10);
  }

  // CMRI wire
  Serial1.begin(TRACER_NODE_BAUD, SERIAL_8N2, RX /* D7 */, TX /* D6 */);
  transport.setInterByteTimeoutMs(TRACER_NODE_INTER_BYTE_TIMEOUT_MS);

  // OLED
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (oledOk) display.dim(true);

  // Register the trace listener and pack/unpack seam
  node.onTrace(ourOnTrace, nullptr);
  node.pack(packInputs);
  node.unpack(unpackOutputs);
  node.begin();

  emitEpoch();
}

// ---- Loop
void loop() {
  const uint32_t nowMs = millis();

  if (!finished) {
    node.tick(nowMs);
  }

  // Capture mode timer
  if (run_active && nowMs >= run_end_ms) {
    run_active = false;
    Serial.print("END CAPTURE t=");
    Serial.print(static_cast<unsigned long>(millis()));
    Serial.print(" ring_used=");
    Serial.print(ring_used);
    Serial.print("/");
    Serial.println(kRingCap);
  }

  // C&C verbs
  char verb[128];
  if (readVerb(verb, sizeof(verb))) {
    if (strcmp(verb, "status") == 0) {
      emitStatus();
    } else if (strcmp(verb, "quit") == 0) {
      Serial.println("{\"event\":\"final\"}");
      finished = true;
    } else if (strcmp(verb, "reboot") == 0) {
      Serial.println("{\"event\":\"reboot\"}");
      Serial.flush();
      ESP.restart();
    } else if (strncmp(verb, "run ", 4) == 0) {
      uint32_t secs = strtoul(verb + 4, nullptr, 10);
      run_active = true;
      ring_used = 0;
      run_start_ms = millis();
      run_end_ms = run_start_ms + secs * 1000u;
      Serial.print("BEGIN CAPTURE t=");
      Serial.println(static_cast<unsigned long>(run_start_ms));
    } else if (strcmp(verb, "dump") == 0) {
      Serial.print("BEGIN DUMP records=");
      Serial.println(ring_used);
      for (size_t i = 0; i < ring_used; ++i) {
        const RingRecord& r = ring[i];
        Serial.print("PKT t=");
        Serial.print(r.t_ms);
        Serial.print((r.flags & kRingFlagTx) != 0u ? " TX " : " RX ");
        Serial.print("UA=");
        Serial.print(r.UA);
        if ((r.flags & kRingFlagInvalidUa) != 0u) {
          Serial.print(" wireUA_invalid=1");
        }
        Serial.print(" mt=");
        Serial.print(static_cast<char>(r.mt));
        Serial.print(" len=");
        Serial.print(r.len);
        Serial.print(" n=");
        Serial.println(i);
      }
      Serial.println("END DUMP");
    } else if (strcmp(verb, "reset") == 0) {
      run_active = false;
      ring_used = 0;
      pollCount = 0;
      initCount = 0;
      txCount = 0;
      replyCount = 0;
      seq = 0;
      Serial.println("{\"event\":\"reset\"}");
    } else if (verb[0] != '\0') {
      Serial.print("{\"event\":\"error\",\"error\":\"unknownVerb\",\"verb\":\"");
      Serial.print(verb);
      Serial.println("\"}");
    }
  }

  // OLED refresh
  if (nowMs - lastDisplayMs >= kDisplayRefreshMs || lastDisplayMs == 0) {
    drawStatus();
    lastDisplayMs = nowMs;
  }
}
